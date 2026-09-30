/****************************************************************************
 * apps/graphics/fpcdesk/desk_sheet_eng.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Spreadsheet's engine (desk_sheet.h): cells, formulas, recalculation.
 *
 * Cells are kept only where there is something (a value or a format), in
 * an open-addressing hash table of pointers keyed by row and column; the
 * cells themselves come from slabs of 128.  A formula is parsed once into
 * tokens in reverse Polish order (references as absolute rows and columns
 * with their '$' flags, parentheses kept so that its text can be made
 * again exactly), evaluated on a small stack of values.
 *
 * Who reads what: a cell's readers are in a hash of key -> keys; a range's
 * readers in lists by column (a range over more than a few columns in one
 * list of its own).  A change marks the cells it reaches (breadth first,
 * through the readers), counts for each how many of the marked cells it
 * reads, and evaluates them in topological order (Kahn's): a cell when all
 * it reads are done.  What is left belongs to a cycle, or reads one:
 * #CIRC!.  Nothing recurses, so a chain of 65536 cells costs no stack.
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "desk_num.h"
#include "desk_sheet.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SLAB_N          128              /* Cells a slab */
#define PMAX_TOK        512              /* A formula's tokens */
#define PMAX_POOL       1024             /* ... its strings */
#define PMAX_DEPTH      48               /* Parentheses and such */
#define EV_STACK        64               /* Evaluation's values */
#define ARENA           8192             /* Evaluation's strings */
#define WIDE            8                /* A range over more columns: the
                                          * wide list */

/* Tokens */

#define T_NUM           1
#define T_STR           2
#define T_BOOL          3
#define T_ERR           4
#define T_REF           5
#define T_RANGE         6
#define T_NAME          7                /* Unknown: #NAME? */
#define T_MISSING       8                /* An argument left out */
#define T_ADD           9
#define T_SUB           10
#define T_MUL           11
#define T_DIV           12
#define T_POW           13
#define T_CAT           14
#define T_EQ            15
#define T_NE            16
#define T_LT            17
#define T_GT            18
#define T_LE            19
#define T_GE            20
#define T_NEG           21
#define T_PLUS          22
#define T_PCT           23
#define T_PAREN         24
#define T_FUNC          25
#define T_BADFUNC       26               /* An unknown function: #NAME? */

/* References: which parts are absolute ('$'), whole columns or rows */

#define RF_AR0          0x01
#define RF_AC0          0x02
#define RF_AR1          0x04
#define RF_AC1          0x08
#define RF_WCOL         0x10             /* A:C */
#define RF_WROW         0x20             /* 1:3 */

#define POOL(f)         ((char *)&(f)->tok[(f)->ntok])

#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sh_tok_s
{
  uint8_t op;
  uint8_t fl;                            /* RF_*, T_ERR: the error */
  uint8_t argc;                          /* T_FUNC */
  uint8_t fn;                            /* T_FUNC: g_fn's index */
  union
  {
    double num;
    struct
    {
      uint16_t r0;
      uint16_t c0;
      uint16_t r1;
      uint16_t c1;
    } rf;
    struct
    {
      uint16_t off;
      uint16_t len;
    } s;
    int b;
  } u;
};

struct sh_formula_s
{
  uint16_t ntok;
  uint16_t npool;
  uint8_t  vol;                          /* NOW(), TODAY() */
  struct sh_tok_s tok[];                 /* Then the strings */
};

struct sh_rdep_s
{
  uint16_t r0;
  uint16_t r1;
  uint16_t c0;
  uint16_t c1;
  uint32_t dep;
};

struct sh_dnode_s
{
  uint32_t key;
  uint16_t n;
  uint16_t cap;
  uint32_t *d;
};

struct sh_slab_s
{
  struct sh_slab_s *next;
  struct sh_cell_s cell[SLAB_N];
};

/* A value while evaluating */

struct sv_s
{
  uint8_t t;                             /* SV_* */
  uint8_t e;                             /* SE_* */
  uint16_t r0;                           /* SV_RANGE */
  uint16_t c0;
  uint16_t r1;
  uint16_t c1;
  double n;
  const char *s;                         /* SV_STR: in a cell, a formula or
                                          * the arena */
};

struct parse_s
{
  const char *p;
  struct sh_tok_s tok[PMAX_TOK];
  int nt;
  char pool[PMAX_POOL];
  int np;
  int depth;
  bool bad;
};

struct ev_s
{
  struct sheet_s *sh;
  const struct sh_formula_s *f;
  int r;                                 /* The cell evaluated */
  int c;
};

typedef void (*fn_t)(struct ev_s *e, struct sv_s *a, int n,
                     struct sv_s *out);

struct fn_s
{
  const char *name;
  uint8_t min;
  uint8_t max;                           /* 255: any number */
  bool vol;
  fn_t fn;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_errname[SE_COUNT] =
{
  "", "#DIV/0!", "#REF!", "#NAME?", "#VALUE!", "#CIRC!", "#N/A", "#NUM!"
};


/****************************************************************************
 * Private Functions: keys
 ****************************************************************************/

int sheet_keys_add(struct sh_keys_s *ks, uint32_t k)
{
  if (ks->n == ks->cap)
    {
      int cap = ks->cap < 16 ? 16 : ks->cap * 2;
      uint32_t *p = realloc(ks->k, cap * sizeof(uint32_t));

      if (p == NULL)
        {
          return -ENOMEM;
        }

      ks->k = p;
      ks->cap = cap;
    }

  ks->k[ks->n++] = k;
  return 0;
}

void sheet_keys_free(struct sh_keys_s *ks)
{
  free(ks->k);
  ks->k = NULL;
  ks->n = 0;
  ks->cap = 0;
}

static uint32_t hash32(uint32_t k)
{
  k ^= k >> 16;
  k *= 0x7feb352du;
  k ^= k >> 15;
  k *= 0x846ca68bu;
  k ^= k >> 16;
  return k;
}

/****************************************************************************
 * Private Functions: cells
 ****************************************************************************/

static struct sh_cell_s *cell_alloc(struct sheet_s *sh)
{
  struct sh_cell_s *c;

  if (sh->free == NULL)
    {
      struct sh_slab_s *s = malloc(sizeof(*s));
      int i;

      if (s == NULL)
        {
          return NULL;
        }

      s->next = sh->slabs;
      sh->slabs = s;
      for (i = SLAB_N - 1; i >= 0; i--)
        {
          s->cell[i].str = (char *)sh->free;
          sh->free = &s->cell[i];
        }
    }

  c = sh->free;
  sh->free = (struct sh_cell_s *)c->str;
  memset(c, 0, sizeof(*c));
  return c;
}

static void cell_release(struct sheet_s *sh, struct sh_cell_s *c)
{
  free(c->str);
  sheet_formula_free(c->f);
  c->str = (char *)sh->free;
  c->f = NULL;
  sh->free = c;
}

static uint32_t slot_of(struct sheet_s *sh, uint32_t key)
{
  uint32_t m = sh->cap - 1;
  uint32_t i = hash32(key) & m;

  while (sh->tab[i] != NULL && sh->tab[i]->key != key)
    {
      i = (i + 1) & m;
    }

  return i;
}

static struct sh_cell_s *find(struct sheet_s *sh, uint32_t key)
{
  return sh->cap == 0 ? NULL : sh->tab[slot_of(sh, key)];
}

static int table_grow(struct sheet_s *sh)
{
  struct sh_cell_s **old = sh->tab;
  uint32_t ocap = sh->cap;
  uint32_t i;

  sh->cap = ocap == 0 ? 256 : ocap * 2;
  sh->tab = calloc(sh->cap, sizeof(*sh->tab));
  if (sh->tab == NULL)
    {
      sh->tab = old;
      sh->cap = ocap;
      return -ENOMEM;
    }

  for (i = 0; i < ocap; i++)
    {
      if (old[i] != NULL)
        {
          sh->tab[slot_of(sh, old[i]->key)] = old[i];
        }
    }

  free(old);
  return 0;
}

/* Put a cell (its key set) into the table */

static int table_put(struct sheet_s *sh, struct sh_cell_s *c)
{
  if ((sh->n + 1) * 10 >= sh->cap * 7 && table_grow(sh) < 0)
    {
      return -ENOMEM;
    }

  sh->tab[slot_of(sh, c->key)] = c;
  sh->n++;
  if (SH_ROW(c->key) > sh->maxrow)
    {
      sh->maxrow = SH_ROW(c->key);
    }

  if (SH_COL(c->key) > sh->maxcol)
    {
      sh->maxcol = SH_COL(c->key);
    }

  return 0;
}

/* Take a cell out of the table (backward shift: no tombstones) */

static void table_take(struct sheet_s *sh, uint32_t key)
{
  uint32_t m = sh->cap - 1;
  uint32_t i;
  uint32_t j;

  if (sh->cap == 0)
    {
      return;
    }

  i = slot_of(sh, key);
  if (sh->tab[i] == NULL)
    {
      return;
    }

  sh->tab[i] = NULL;
  sh->n--;
  sh->extstale = true;
  for (j = (i + 1) & m; sh->tab[j] != NULL; j = (j + 1) & m)
    {
      uint32_t h = hash32(sh->tab[j]->key) & m;

      /* Does j's home lie cyclically in (i, j]?  Else it moves to i */

      if ((i < j) ? (h <= i || h > j) : (h <= i && h > j))
        {
          sh->tab[i] = sh->tab[j];
          sh->tab[j] = NULL;
          i = j;
        }
    }
}

static struct sh_cell_s *cell_make(struct sheet_s *sh, int r, int c)
{
  uint32_t key = SH_KEY(r, c);
  struct sh_cell_s *cell = find(sh, key);

  if (cell != NULL)
    {
      return cell;
    }

  cell = cell_alloc(sh);
  if (cell == NULL)
    {
      return NULL;
    }

  cell->key = key;
  if (table_put(sh, cell) < 0)
    {
      cell_release(sh, cell);
      return NULL;
    }

  return cell;
}

/****************************************************************************
 * Private Functions: names
 ****************************************************************************/

void sheet_colname(int c, char *buf)
{
  if (c < 26)
    {
      buf[0] = 'A' + c;
      buf[1] = '\0';
    }
  else
    {
      buf[0] = 'A' + (c / 26) - 1;
      buf[1] = 'A' + c % 26;
      buf[2] = '\0';
    }
}

void sheet_refname(int r, int c, char *buf)
{
  sheet_colname(c, buf);
  num_int(buf + strlen(buf), (r + 1) & 0x1ffff, 0);
}

/* [$]letters: a column, the characters used (0: none) */

static int parse_col(const char *s, int *c, bool *abs)
{
  const char *p = s;
  int v = 0;
  int n = 0;

  *abs = *p == '$';
  p += *abs;
  while (isalpha((unsigned char)*p) && n < 3)
    {
      v = v * 26 + (toupper((unsigned char)*p) - 'A' + 1);
      p++;
      n++;
    }

  if (n == 0 || n > 2 || isalpha((unsigned char)*p) || v > SH_COLS)
    {
      return 0;
    }

  *c = v - 1;
  return p - s;
}

/* [$]digits: a row */

static int parse_row(const char *s, int *r, bool *abs)
{
  const char *p = s;
  long v = 0;
  int n = 0;

  *abs = *p == '$';
  p += *abs;
  while (isdigit((unsigned char)*p) && n < 7)
    {
      v = v * 10 + (*p - '0');
      p++;
      n++;
    }

  if (n == 0 || v < 1 || v > SH_ROWS)
    {
      return 0;
    }

  *r = (int)v - 1;
  return p - s;
}

static int parse_ref2(const char *s, int *r, int *c, bool *ar, bool *ac)
{
  int a = parse_col(s, c, ac);
  int b;

  if (a == 0)
    {
      return 0;
    }

  b = parse_row(s + a, r, ar);
  if (b == 0 || isalnum((unsigned char)s[a + b]) || s[a + b] == '_')
    {
      return 0;
    }

  return a + b;
}

int sheet_parse_ref(const char *s, int *r, int *c)
{
  bool ar;
  bool ac;

  return parse_ref2(s, r, c, &ar, &ac);
}

const char *sheet_errname(int err)
{
  return err > 0 && err < SE_COUNT ? g_errname[err] : "#ERR";
}

/****************************************************************************
 * Private Functions: dates
 ****************************************************************************/

/* Days from 1970-01-01 of a date in the proleptic Gregorian calendar */

static long days_civil(long y, int m, int d)
{
  long era;
  long yoe;
  long doy;
  long doe;

  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

double sheet_serial(int y, int m, int d)
{
  return (double)(days_civil(y, m, d) - days_civil(1899, 12, 30));
}

void sheet_date(double serial, int *y, int *m, int *d, int *hh, int *mm,
                int *ss)
{
  long z = (long)floor(serial);
  long secs = (long)floor((serial - (double)z) * 86400.0 + 0.5);
  long era;
  long doe;
  long yoe;
  long doy;
  long mp;

  if (secs >= 86400)
    {
      secs -= 86400;
      z++;
    }

  z += days_civil(1899, 12, 30) + 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = z - era * 146097;
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = (int)(yoe + era * 400 + (*m <= 2));
  *hh = (int)(secs / 3600);
  *mm = (int)(secs / 60 % 60);
  *ss = (int)(secs % 60);
}

static double default_now(void)
{
  time_t t = time(NULL);

  return (double)t / 86400.0 + sheet_serial(1970, 1, 1);
}

/****************************************************************************
 * Private Functions: the functions' table (below) and the parser
 ****************************************************************************/

static const struct fn_s g_fn[];
static const int g_nfn;

static int fn_find(const char *name, int n)
{
  int i;

  for (i = 0; i < g_nfn; i++)
    {
      if ((int)strlen(g_fn[i].name) == n &&
          strncasecmp(g_fn[i].name, name, n) == 0)
        {
          return i;
        }
    }

  return -1;
}

static void skip_ws(struct parse_s *ps)
{
  while (*ps->p == ' ' || *ps->p == '\t')
    {
      ps->p++;
    }
}

static struct sh_tok_s *emit(struct parse_s *ps, int op)
{
  struct sh_tok_s *t;

  if (ps->nt >= PMAX_TOK)
    {
      ps->bad = true;
      return &ps->tok[0];
    }

  t = &ps->tok[ps->nt++];
  memset(t, 0, sizeof(*t));
  t->op = op;
  return t;
}

static int pool_add(struct parse_s *ps, const char *s, int n)
{
  int off = ps->np;

  if (ps->np + n > PMAX_POOL)
    {
      ps->bad = true;
      return 0;
    }

  memcpy(ps->pool + ps->np, s, n);
  ps->np += n;
  return off;
}

static void parse_expr(struct parse_s *ps);

/* A reference or a range at ps->p, if there is one */

static bool parse_refs(struct parse_s *ps)
{
  const char *s = ps->p;
  struct sh_tok_s *t;
  int r0;
  int c0;
  int r1;
  int c1;
  bool ar0;
  bool ac0;
  bool ar1;
  bool ac1;
  int n;
  int m;
  int sep;

  /* A1, A1:B2 (or Lotus' A1..B2) */

  n = parse_ref2(s, &r0, &c0, &ar0, &ac0);
  if (n > 0)
    {
      sep = s[n] == ':' ? 1 : s[n] == '.' && s[n + 1] == '.' ? 2 : 0;
      if (sep > 0 &&
          (m = parse_ref2(s + n + sep, &r1, &c1, &ar1, &ac1)) > 0)
        {
          t = emit(ps, T_RANGE);
          if (r1 < r0)
            {
              int x = r0;
              bool y = ar0;

              r0 = r1;
              r1 = x;
              ar0 = ar1;
              ar1 = y;
            }

          if (c1 < c0)
            {
              int x = c0;
              bool y = ac0;

              c0 = c1;
              c1 = x;
              ac0 = ac1;
              ac1 = y;
            }

          t->u.rf.r0 = r0;
          t->u.rf.c0 = c0;
          t->u.rf.r1 = r1;
          t->u.rf.c1 = c1;
          t->fl = (ar0 ? RF_AR0 : 0) | (ac0 ? RF_AC0 : 0) |
                  (ar1 ? RF_AR1 : 0) | (ac1 ? RF_AC1 : 0);
          ps->p = s + n + sep + m;
          return true;
        }

      if (s[n] == '(')
        {
          return false;                  /* A function's name */
        }

      t = emit(ps, T_REF);
      t->u.rf.r0 = r0;
      t->u.rf.c0 = c0;
      t->u.rf.r1 = r0;
      t->u.rf.c1 = c0;
      t->fl = (ar0 ? RF_AR0 : 0) | (ac0 ? RF_AC0 : 0);
      ps->p = s + n;
      return true;
    }

  /* A:C */

  n = parse_col(s, &c0, &ac0);
  if (n > 0 && s[n] == ':' && (m = parse_col(s + n + 1, &c1, &ac1)) > 0 &&
      !isdigit((unsigned char)s[n + 1 + m]))
    {
      t = emit(ps, T_RANGE);
      t->u.rf.r0 = 0;
      t->u.rf.r1 = SH_ROWS - 1;
      t->u.rf.c0 = MIN(c0, c1);
      t->u.rf.c1 = MAX(c0, c1);
      t->fl = RF_WCOL | ((c0 <= c1 ? ac0 : ac1) ? RF_AC0 : 0) |
              ((c0 <= c1 ? ac1 : ac0) ? RF_AC1 : 0);
      ps->p = s + n + 1 + m;
      return true;
    }

  /* 1:3 */

  n = parse_row(s, &r0, &ar0);
  if (n > 0 && s[n] == ':' && (m = parse_row(s + n + 1, &r1, &ar1)) > 0 &&
      !isalnum((unsigned char)s[n + 1 + m]) && s[n + 1 + m] != '.')
    {
      t = emit(ps, T_RANGE);
      t->u.rf.c0 = 0;
      t->u.rf.c1 = SH_COLS - 1;
      t->u.rf.r0 = MIN(r0, r1);
      t->u.rf.r1 = MAX(r0, r1);
      t->fl = RF_WROW | ((r0 <= r1 ? ar0 : ar1) ? RF_AR0 : 0) |
              ((r0 <= r1 ? ar1 : ar0) ? RF_AR1 : 0);
      ps->p = s + n + 1 + m;
      return true;
    }

  return false;
}

static void parse_primary(struct parse_s *ps)
{
  struct sh_tok_s *t;
  const char *s;
  double v;
  int n;
  int i;

  skip_ws(ps);
  s = ps->p;
  if (++ps->depth > PMAX_DEPTH)
    {
      ps->bad = true;
      return;
    }

  if (*s == '(')
    {
      ps->p++;
      parse_expr(ps);
      skip_ws(ps);
      if (*ps->p != ')')
        {
          ps->bad = true;
        }
      else
        {
          ps->p++;
        }

      emit(ps, T_PAREN);
    }
  else if (*s == '"')
    {
      /* Straight into the strings ("" is a quote) */

      int off = ps->np;

      s++;
      for (; ; )
        {
          if (*s == '\0')
            {
              ps->bad = true;
              break;
            }

          if (*s == '"')
            {
              if (s[1] != '"')
                {
                  s++;
                  break;
                }

              s++;
            }

          if (ps->np < PMAX_POOL)
            {
              ps->pool[ps->np++] = *s;
            }
          else
            {
              ps->bad = true;
            }

          s++;
        }

      t = emit(ps, T_STR);
      t->u.s.len = ps->np - off;
      t->u.s.off = off;
      ps->p = s;
    }
  else if (*s == '#')
    {
      for (i = 1; i < SE_COUNT; i++)
        {
          n = strlen(g_errname[i]);
          if (strncasecmp(s, g_errname[i], n) == 0)
            {
              t = emit(ps, T_ERR);
              t->fl = i;
              ps->p = s + n;
              break;
            }
        }

      if (i == SE_COUNT)
        {
          ps->bad = true;
        }
    }
  else if (parse_refs(ps))
    {
    }
  else if (isdigit((unsigned char)*s) || (*s == '.' &&
                                          isdigit((unsigned char)s[1])))
    {
      n = num_parse(s, &v);
      if (n == 0)
        {
          ps->bad = true;
          return;
        }

      t = emit(ps, T_NUM);
      t->u.num = v;
      ps->p = s + n;
    }
  else if (isalpha((unsigned char)*s) || *s == '_' || *s == '@')
    {
      const char *name;
      int len;

      s += *s == '@';                    /* Lotus: @SUM() */
      name = s;
      while (isalnum((unsigned char)*s) || *s == '_' || *s == '.')
        {
          s++;
        }

      len = s - name;
      ps->p = s;
      skip_ws(ps);
      if (*ps->p == '(')
        {
          int argc = 0;
          int fn = fn_find(name, len);

          ps->p++;
          skip_ws(ps);
          if (*ps->p != ')')
            {
              for (; ; )
                {
                  skip_ws(ps);
                  if (*ps->p == ',' || *ps->p == ')')
                    {
                      emit(ps, T_MISSING);
                    }
                  else
                    {
                      parse_expr(ps);
                    }

                  argc++;
                  skip_ws(ps);
                  if (*ps->p == ',')
                    {
                      ps->p++;
                      continue;
                    }

                  break;
                }
            }

          if (*ps->p != ')' || argc > 255)
            {
              ps->bad = true;
              return;
            }

          ps->p++;
          if (fn >= 0)
            {
              if (argc < g_fn[fn].min || argc > g_fn[fn].max)
                {
                  ps->bad = true;
                  return;
                }

              t = emit(ps, T_FUNC);
              t->fn = fn;
            }
          else
            {
              t = emit(ps, T_BADFUNC);
              t->u.s.len = len;
              t->u.s.off = pool_add(ps, name, len);
            }

          t->argc = argc;
        }
      else if (len == 4 && strncasecmp(name, "TRUE", 4) == 0)
        {
          emit(ps, T_BOOL)->u.b = 1;
        }
      else if (len == 5 && strncasecmp(name, "FALSE", 5) == 0)
        {
          emit(ps, T_BOOL)->u.b = 0;
        }
      else
        {
          t = emit(ps, T_NAME);
          t->u.s.len = len;
          t->u.s.off = pool_add(ps, name, len);
        }
    }
  else
    {
      ps->bad = true;
    }

  ps->depth--;
}

static void parse_postfix(struct parse_s *ps)
{
  parse_primary(ps);
  for (; ; )
    {
      skip_ws(ps);
      if (*ps->p != '%')
        {
          break;
        }

      ps->p++;
      emit(ps, T_PCT);
    }
}

static void parse_unary(struct parse_s *ps)
{
  skip_ws(ps);
  if (*ps->p == '-' || *ps->p == '+')
    {
      int op = *ps->p == '-' ? T_NEG : T_PLUS;

      ps->p++;
      if (++ps->depth > PMAX_DEPTH)
        {
          ps->bad = true;
          return;
        }

      parse_unary(ps);
      ps->depth--;
      emit(ps, op);
    }
  else
    {
      parse_postfix(ps);
    }
}

static void parse_pow(struct parse_s *ps)
{
  parse_unary(ps);
  for (; ; )
    {
      skip_ws(ps);
      if (*ps->p != '^')
        {
          break;
        }

      ps->p++;
      parse_unary(ps);
      emit(ps, T_POW);
    }
}

static void parse_mul(struct parse_s *ps)
{
  parse_pow(ps);
  for (; ; )
    {
      int op;

      skip_ws(ps);
      op = *ps->p == '*' ? T_MUL : *ps->p == '/' ? T_DIV : 0;
      if (op == 0)
        {
          break;
        }

      ps->p++;
      parse_pow(ps);
      emit(ps, op);
    }
}

static void parse_add(struct parse_s *ps)
{
  parse_mul(ps);
  for (; ; )
    {
      int op;

      skip_ws(ps);
      op = *ps->p == '+' ? T_ADD : *ps->p == '-' ? T_SUB : 0;
      if (op == 0)
        {
          break;
        }

      ps->p++;
      parse_mul(ps);
      emit(ps, op);
    }
}

static void parse_cat(struct parse_s *ps)
{
  parse_add(ps);
  for (; ; )
    {
      skip_ws(ps);
      if (*ps->p != '&')
        {
          break;
        }

      ps->p++;
      parse_add(ps);
      emit(ps, T_CAT);
    }
}

static void parse_expr(struct parse_s *ps)
{
  if (++ps->depth > PMAX_DEPTH)
    {
      ps->bad = true;
      return;
    }

  parse_cat(ps);
  for (; ; )
    {
      const char *p;
      int op = 0;

      skip_ws(ps);
      p = ps->p;
      if (p[0] == '<' && p[1] == '>')
        {
          op = T_NE;
          ps->p += 2;
        }
      else if (p[0] == '<' && p[1] == '=')
        {
          op = T_LE;
          ps->p += 2;
        }
      else if (p[0] == '>' && p[1] == '=')
        {
          op = T_GE;
          ps->p += 2;
        }
      else if (p[0] == '<' || p[0] == '>' || p[0] == '=')
        {
          op = p[0] == '<' ? T_LT : p[0] == '>' ? T_GT : T_EQ;
          ps->p++;
        }

      if (op == 0)
        {
          break;
        }

      parse_cat(ps);
      emit(ps, op);
    }

  ps->depth--;
}

/* A formula's text (after the '='): its tokens, or NULL */

static struct sh_formula_s *parse(const char *s)
{
  struct parse_s *ps = malloc(sizeof(*ps));
  struct sh_formula_s *f = NULL;
  int i;

  if (ps == NULL)
    {
      return NULL;
    }

  ps->p = s;
  ps->nt = 0;
  ps->np = 0;
  ps->depth = 0;
  ps->bad = false;
  parse_expr(ps);
  skip_ws(ps);
  if (!ps->bad && *ps->p == '\0' && ps->nt > 0)
    {
      f = malloc(sizeof(*f) + ps->nt * sizeof(struct sh_tok_s) + ps->np);
      if (f != NULL)
        {
          f->ntok = ps->nt;
          f->npool = ps->np;
          f->vol = 0;
          memcpy(f->tok, ps->tok, ps->nt * sizeof(struct sh_tok_s));
          memcpy(POOL(f), ps->pool, ps->np);
          for (i = 0; i < f->ntok; i++)
            {
              if (f->tok[i].op == T_FUNC && g_fn[f->tok[i].fn].vol)
                {
                  f->vol = 1;
                }
            }
        }
    }

  free(ps);
  return f;
}

void sheet_formula_free(struct sh_formula_s *f)
{
  free(f);
}

static size_t formula_size(const struct sh_formula_s *f)
{
  return sizeof(*f) + f->ntok * sizeof(struct sh_tok_s) + f->npool;
}

/****************************************************************************
 * Private Functions: a formula's text again
 ****************************************************************************/

/* A stack of pieces of text in one buffer */

struct dec_s
{
  char *buf;
  int len;
  int cap;
  int off[PMAX_TOK];
  int prev[PMAX_TOK];
  int n;
  bool bad;
};

static void dec_put(struct dec_s *d, const char *s, int n)
{
  if (d->len + n + 1 > d->cap)
    {
      int cap = (d->len + n + 1) * 2;
      char *b = realloc(d->buf, cap);

      if (b == NULL)
        {
          d->bad = true;
          return;
        }

      d->buf = b;
      d->cap = cap;
    }

  memcpy(d->buf + d->len, s, n);
  d->len += n;
  d->buf[d->len] = '\0';
}

/* Pop the top k pieces, joined by the caller: a new piece at the end */

static int dec_begin(struct dec_s *d)
{
  return d->len;
}

static void dec_push(struct dec_s *d, int start)
{
  if (d->n >= PMAX_TOK)
    {
      d->bad = true;
      return;
    }

  d->off[d->n++] = start;
}

static const char *dec_arg(struct dec_s *d, int from_top, int *len)
{
  int i = d->n - 1 - from_top;
  int end;

  if (i < 0)
    {
      d->bad = true;
      *len = 0;
      return "";
    }

  /* The pieces are one after the other in the buffer */

  end = i + 1 < d->n ? d->off[i + 1] : d->len;
  *len = end - d->off[i];
  return d->buf + d->off[i];
}

static void ref_text(char *out, int r, int c, bool ar, bool ac)
{
  char col[4];

  sheet_colname(c, col);
  if (ac)
    {
      *out++ = '$';
    }

  strcpy(out, col);
  out += strlen(col);
  if (ar)
    {
      *out++ = '$';
    }

  num_int(out, r + 1, 0);
}

static void tok_text(const struct sh_formula_s *f, const struct sh_tok_s *t,
                     char *out, int size)
{
  char a[16];
  char b[16];
  char col[4];

  switch (t->op)
    {
      case T_NUM:
        num_plain(t->u.num, 15, out, size);
        break;

      case T_BOOL:
        strlcpy(out, t->u.b ? "TRUE" : "FALSE", size);
        break;

      case T_ERR:
        strlcpy(out, sheet_errname(t->fl), size);
        break;

      case T_REF:
        ref_text(out, t->u.rf.r0, t->u.rf.c0, t->fl & RF_AR0,
                 t->fl & RF_AC0);
        break;

      case T_RANGE:
        if (t->fl & RF_WCOL)
          {
            sheet_colname(t->u.rf.c0, col);
            strcpy(a, t->fl & RF_AC0 ? "$" : "");
            strcat(a, col);
            sheet_colname(t->u.rf.c1, col);
            strcpy(b, t->fl & RF_AC1 ? "$" : "");
            strcat(b, col);
          }
        else if (t->fl & RF_WROW)
          {
            strcpy(a, t->fl & RF_AR0 ? "$" : "");
            num_int(a + strlen(a), t->u.rf.r0 + 1, 0);
            strcpy(b, t->fl & RF_AR1 ? "$" : "");
            num_int(b + strlen(b), t->u.rf.r1 + 1, 0);
          }
        else
          {
            ref_text(a, t->u.rf.r0, t->u.rf.c0, t->fl & RF_AR0,
                     t->fl & RF_AC0);
            ref_text(b, t->u.rf.r1, t->u.rf.c1, t->fl & RF_AR1,
                     t->fl & RF_AC1);
          }

        strlcpy(out, a, size);
        strlcat(out, ":", size);
        strlcat(out, b, size);
        break;

      case T_NAME:
        {
          int n = t->u.s.len < size - 1 ? t->u.s.len : size - 1;

          memcpy(out, POOL(f) + t->u.s.off, n);
          out[n] = '\0';
        }
        break;

      default:
        out[0] = '\0';
        break;
    }
}

int sheet_formula_text(const struct sh_formula_s *f, char *buf, size_t size)
{
  static const char * const ops[] =
  {
    "+", "-", "*", "/", "^", "&", "=", "<>", "<", ">", "<=", ">="
  };

  struct dec_s *d = calloc(1, sizeof(*d));
  char tmp[40];
  int ret = 0;
  int i;

  if (d == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < f->ntok && !d->bad; i++)
    {
      const struct sh_tok_s *t = &f->tok[i];
      int start = dec_begin(d);
      const char *s;
      int len;
      int k;

      switch (t->op)
        {
          case T_STR:
            {
              const char *p = POOL(f) + t->u.s.off;
              int j;

              dec_put(d, "\"", 1);
              for (j = 0; j < t->u.s.len; j++)
                {
                  dec_put(d, &p[j], 1);
                  if (p[j] == '"')
                    {
                      dec_put(d, "\"", 1);
                    }
                }

              dec_put(d, "\"", 1);
              dec_push(d, start);
            }
            break;

          case T_MISSING:
            dec_push(d, start);
            break;

          case T_ADD:
          case T_SUB:
          case T_MUL:
          case T_DIV:
          case T_POW:
          case T_CAT:
          case T_EQ:
          case T_NE:
          case T_LT:
          case T_GT:
          case T_LE:
          case T_GE:
          case T_NEG:
          case T_PLUS:
          case T_PCT:
          case T_PAREN:
          case T_FUNC:
          case T_BADFUNC:
            {
              /* Its operands are the top pieces: copied out, joined, and
               * the result put where the first of them began
               */

              int nargs = t->op == T_FUNC || t->op == T_BADFUNC ? t->argc :
                          t->op >= T_NEG ? 1 : 2;
              int first;
              int total;
              char *joined;
              int jn = 0;

              if (nargs > d->n)
                {
                  d->bad = true;
                  break;
                }

              first = d->n - nargs;
              total = d->len - (nargs > 0 ? d->off[first] : d->len) + 64 +
                      (t->op == T_BADFUNC ? t->u.s.len : 0) + nargs;
              joined = malloc(total);
              if (joined == NULL)
                {
                  d->bad = true;
                  break;
                }

#define JOIN(s_, n_) do { memcpy(joined + jn, (s_), (n_)); jn += (n_); } \
                     while (0)

              if (t->op == T_FUNC || t->op == T_BADFUNC)
                {
                  const char *name = t->op == T_FUNC ? g_fn[t->fn].name :
                                     POOL(f) + t->u.s.off;
                  int nn = t->op == T_FUNC ? (int)strlen(name) :
                           t->u.s.len;

                  JOIN(name, nn);
                  JOIN("(", 1);
                  for (k = 0; k < nargs; k++)
                    {
                      s = dec_arg(d, nargs - 1 - k, &len);
                      if (k > 0)
                        {
                          JOIN(",", 1);
                        }

                      JOIN(s, len);
                    }

                  JOIN(")", 1);
                }
              else if (t->op == T_NEG || t->op == T_PLUS)
                {
                  s = dec_arg(d, 0, &len);
                  JOIN(t->op == T_NEG ? "-" : "+", 1);
                  JOIN(s, len);
                }
              else if (t->op == T_PCT)
                {
                  s = dec_arg(d, 0, &len);
                  JOIN(s, len);
                  JOIN("%", 1);
                }
              else if (t->op == T_PAREN)
                {
                  s = dec_arg(d, 0, &len);
                  JOIN("(", 1);
                  JOIN(s, len);
                  JOIN(")", 1);
                }
              else
                {
                  const char *o = ops[t->op - T_ADD];

                  s = dec_arg(d, 1, &len);
                  JOIN(s, len);
                  JOIN(o, strlen(o));
                  s = dec_arg(d, 0, &len);
                  JOIN(s, len);
                }

#undef JOIN
              if (nargs > 0)
                {
                  d->len = d->off[first];
                  d->n = first;
                }

              start = d->len;
              dec_put(d, joined, jn);
              dec_push(d, start);
              free(joined);
            }
            break;

          default:
            tok_text(f, t, tmp, sizeof(tmp));
            dec_put(d, tmp, strlen(tmp));
            dec_push(d, start);
            break;
        }
    }

  if (d->bad || d->n != 1)
    {
      strlcpy(buf, "#ERR", size);
      ret = -EINVAL;
    }
  else
    {
      strlcpy(buf, d->buf, size);
      ret = (int)strlen(d->buf);
    }

  free(d->buf);
  free(d);
  return ret;
}

/****************************************************************************
 * Private Functions: moving references
 ****************************************************************************/

/* Relative parts moved by (dr, dc): false if off the sheet */

static bool shift_ref(struct sh_tok_s *t, int dr, int dc)
{
  int r0 = t->u.rf.r0;
  int c0 = t->u.rf.c0;
  int r1 = t->u.rf.r1;
  int c1 = t->u.rf.c1;

  if (!(t->fl & RF_WCOL))
    {
      r0 += t->fl & RF_AR0 ? 0 : dr;
      r1 += t->fl & (t->op == T_REF ? RF_AR0 : RF_AR1) ? 0 : dr;
    }

  if (!(t->fl & RF_WROW))
    {
      c0 += t->fl & RF_AC0 ? 0 : dc;
      c1 += t->fl & (t->op == T_REF ? RF_AC0 : RF_AC1) ? 0 : dc;
    }

  if (r0 < 0 || r1 < 0 || c0 < 0 || c1 < 0 || r0 >= SH_ROWS ||
      r1 >= SH_ROWS || c0 >= SH_COLS || c1 >= SH_COLS)
    {
      return false;
    }

  if (t->op == T_REF)
    {
      r1 = r0;
      c1 = c0;
    }

  t->u.rf.r0 = MIN(r0, r1);
  t->u.rf.r1 = MAX(r0, r1);
  t->u.rf.c0 = MIN(c0, c1);
  t->u.rf.c1 = MAX(c0, c1);
  return true;
}

static void make_referr(struct sh_tok_s *t)
{
  t->op = T_ERR;
  t->fl = SE_REF;
}

struct sh_formula_s *sheet_formula_dup(const struct sh_formula_s *f,
                                       int dr, int dc)
{
  struct sh_formula_s *n;
  int i;

  if (f == NULL)
    {
      return NULL;
    }

  n = malloc(formula_size(f));
  if (n == NULL)
    {
      return NULL;
    }

  memcpy(n, f, formula_size(f));
  for (i = 0; i < n->ntok && (dr != 0 || dc != 0); i++)
    {
      struct sh_tok_s *t = &n->tok[i];

      if ((t->op == T_REF || t->op == T_RANGE) && !shift_ref(t, dr, dc))
        {
          make_referr(t);
        }
    }

  return n;
}

/* Rows (or columns) [at, at + n) inserted (n > 0) or deleted (n < 0): a
 * formula's references follow.  True if anything changed.
 */

static bool adjust_one(int *a, int *b, bool range, int at, int n)
{
  if (n > 0)
    {
      if (*a >= at)
        {
          *a += n;
        }

      if (*b >= at)
        {
          *b += n;
        }

      return true;
    }

  n = -n;
  if (*a >= at + n)
    {
      *a -= n;
    }
  else if (*a >= at)
    {
      if (!range || *b < at + n)
        {
          return false;                  /* All of it deleted */
        }

      *a = at;
    }

  if (*b >= at + n)
    {
      *b -= n;
    }
  else if (*b >= at)
    {
      *b = at - 1;
    }

  return *b >= *a;
}

static bool adjust_formula(struct sh_formula_s *f, bool rows, int at, int n)
{
  bool changed = false;
  int lim = rows ? SH_ROWS : SH_COLS;
  int i;

  for (i = 0; i < f->ntok; i++)
    {
      struct sh_tok_s *t = &f->tok[i];
      int a;
      int b;
      bool ok;

      if (t->op != T_REF && t->op != T_RANGE)
        {
          continue;
        }

      if (rows ? (t->fl & RF_WCOL) : (t->fl & RF_WROW))
        {
          continue;
        }

      a = rows ? t->u.rf.r0 : t->u.rf.c0;
      b = rows ? t->u.rf.r1 : t->u.rf.c1;
      ok = adjust_one(&a, &b, t->op == T_RANGE, at, n);
      if (ok && (a >= lim || b >= lim))
        {
          if (t->op == T_RANGE && a < lim)
            {
              b = lim - 1;               /* Pushed off the end: cut */
            }
          else
            {
              ok = false;
            }
        }

      if (!ok)
        {
          make_referr(t);
          changed = true;
          continue;
        }

      if (rows && (a != t->u.rf.r0 || b != t->u.rf.r1))
        {
          t->u.rf.r0 = a;
          t->u.rf.r1 = b;
          changed = true;
        }
      else if (!rows && (a != t->u.rf.c0 || b != t->u.rf.c1))
        {
          t->u.rf.c0 = a;
          t->u.rf.c1 = b;
          changed = true;
        }
    }

  return changed;
}

/* References into src follow it by (dr, dc) (a move) */

static bool move_formula(struct sh_formula_s *f, const struct sh_rect_s *src,
                         int dr, int dc)
{
  bool changed = false;
  int i;

  for (i = 0; i < f->ntok; i++)
    {
      struct sh_tok_s *t = &f->tok[i];

      if ((t->op != T_REF && t->op != T_RANGE) ||
          (t->fl & (RF_WCOL | RF_WROW)))
        {
          continue;
        }

      if (t->u.rf.r0 >= src->r0 && t->u.rf.r1 <= src->r1 &&
          t->u.rf.c0 >= src->c0 && t->u.rf.c1 <= src->c1)
        {
          int r0 = t->u.rf.r0 + dr;
          int r1 = t->u.rf.r1 + dr;
          int c0 = t->u.rf.c0 + dc;
          int c1 = t->u.rf.c1 + dc;

          if (r0 < 0 || c0 < 0 || r1 >= SH_ROWS || c1 >= SH_COLS)
            {
              make_referr(t);
            }
          else
            {
              t->u.rf.r0 = r0;
              t->u.rf.r1 = r1;
              t->u.rf.c0 = c0;
              t->u.rf.c1 = c1;
            }

          changed = true;
        }
    }

  return changed;
}

/****************************************************************************
 * Private Functions: who reads what
 ****************************************************************************/

static uint32_t dslot(struct sheet_s *sh, uint32_t key)
{
  uint32_t m = sh->dcap - 1;
  uint32_t i = hash32(key) & m;

  while (sh->dtab[i].key != SH_NOKEY && sh->dtab[i].key != key)
    {
      i = (i + 1) & m;
    }

  return i;
}

static struct sh_dnode_s *dnode(struct sheet_s *sh, uint32_t key, bool make)
{
  uint32_t i;

  if (sh->dcap == 0 || (make && (sh->dn + 1) * 10 >= sh->dcap * 7))
    {
      struct sh_dnode_s *old = sh->dtab;
      uint32_t ocap = sh->dcap;
      uint32_t j;

      if (!make)
        {
          return NULL;
        }

      sh->dcap = ocap == 0 ? 256 : ocap * 2;
      sh->dtab = malloc(sh->dcap * sizeof(*sh->dtab));
      if (sh->dtab == NULL)
        {
          sh->dtab = old;
          sh->dcap = ocap;
          return NULL;
        }

      for (j = 0; j < sh->dcap; j++)
        {
          sh->dtab[j].key = SH_NOKEY;
        }

      for (j = 0; j < ocap; j++)
        {
          if (old[j].key != SH_NOKEY)
            {
              sh->dtab[dslot(sh, old[j].key)] = old[j];
            }
        }

      free(old);
    }

  i = dslot(sh, key);
  if (sh->dtab[i].key == SH_NOKEY)
    {
      if (!make)
        {
          return NULL;
        }

      sh->dtab[i].key = key;
      sh->dtab[i].n = 0;
      sh->dtab[i].cap = 0;
      sh->dtab[i].d = NULL;
      sh->dn++;
    }

  return &sh->dtab[i];
}

static void rvec_add(struct sh_rvec_s *v, const struct sh_rdep_s *d)
{
  if (v->n == v->cap)
    {
      int cap = v->cap < 4 ? 4 : v->cap * 2;
      struct sh_rdep_s *p = realloc(v->d, cap * sizeof(*p));

      if (p == NULL)
        {
          return;
        }

      v->d = p;
      v->cap = cap;
    }

  v->d[v->n++] = *d;
}

static void rvec_del(struct sh_rvec_s *v, const struct sh_rdep_s *d)
{
  int i;

  for (i = 0; i < v->n; i++)
    {
      if (memcmp(&v->d[i], d, sizeof(*d)) == 0)
        {
          v->d[i] = v->d[--v->n];
          return;
        }
    }
}

/* A formula at key registers (add) or unregisters what it reads */

static void deps(struct sheet_s *sh, uint32_t key,
                 const struct sh_formula_s *f, bool add)
{
  int i;
  int c;

  if (f->vol)
    {
      if (add)
        {
          sheet_keys_add(&sh->vol, key);
        }
      else
        {
          for (i = 0; i < sh->vol.n; i++)
            {
              if (sh->vol.k[i] == key)
                {
                  sh->vol.k[i] = sh->vol.k[--sh->vol.n];
                  break;
                }
            }
        }
    }

  for (i = 0; i < f->ntok; i++)
    {
      const struct sh_tok_s *t = &f->tok[i];

      if (t->op == T_REF || (t->op == T_RANGE && t->u.rf.r0 == t->u.rf.r1 &&
                             t->u.rf.c0 == t->u.rf.c1))
        {
          struct sh_dnode_s *d = dnode(sh, SH_KEY(t->u.rf.r0, t->u.rf.c0),
                                       add);
          int j;

          if (d == NULL)
            {
              continue;
            }

          if (add)
            {
              if (d->n == d->cap)
                {
                  int cap = d->cap < 2 ? 2 : d->cap * 2;
                  uint32_t *p;

                  if (cap > 65535)
                    {
                      continue;
                    }

                  p = realloc(d->d, cap * sizeof(uint32_t));
                  if (p == NULL)
                    {
                      continue;
                    }

                  d->d = p;
                  d->cap = cap;
                }

              d->d[d->n++] = key;
            }
          else
            {
              for (j = 0; j < d->n; j++)
                {
                  if (d->d[j] == key)
                    {
                      d->d[j] = d->d[--d->n];
                      break;
                    }
                }
            }
        }
      else if (t->op == T_RANGE)
        {
          struct sh_rdep_s rd;

          rd.r0 = t->u.rf.r0;
          rd.r1 = t->u.rf.r1;
          rd.c0 = t->u.rf.c0;
          rd.c1 = t->u.rf.c1;
          rd.dep = key;
          if (rd.c1 - rd.c0 >= WIDE)
            {
              (add ? rvec_add : rvec_del)(&sh->rwide, &rd);
            }
          else
            {
              for (c = rd.c0; c <= rd.c1; c++)
                {
                  (add ? rvec_add : rvec_del)(&sh->rcol[c], &rd);
                }
            }
        }
    }
}

static void deps_clear(struct sheet_s *sh)
{
  uint32_t i;

  for (i = 0; i < sh->dcap; i++)
    {
      if (sh->dtab[i].key != SH_NOKEY)
        {
          free(sh->dtab[i].d);
        }
    }

  free(sh->dtab);
  sh->dtab = NULL;
  sh->dcap = 0;
  sh->dn = 0;
  for (i = 0; i < SH_COLS; i++)
    {
      free(sh->rcol[i].d);
      sh->rcol[i].d = NULL;
      sh->rcol[i].n = 0;
      sh->rcol[i].cap = 0;
    }

  free(sh->rwide.d);
  sh->rwide.d = NULL;
  sh->rwide.n = 0;
  sh->rwide.cap = 0;
  sheet_keys_free(&sh->vol);
}

static void deps_rebuild(struct sheet_s *sh)
{
  uint32_t i;

  deps_clear(sh);
  for (i = 0; i < sh->cap; i++)
    {
      struct sh_cell_s *c = sh->tab[i];

      if (c != NULL && c->kind == SK_FORMULA)
        {
          deps(sh, c->key, c->f, true);
        }
    }
}

/* The readers of the cell at key, each given to fn */

typedef void (*reader_t)(struct sheet_s *sh, uint32_t reader, void *arg);

static void readers(struct sheet_s *sh, uint32_t key, reader_t fn,
                    void *arg)
{
  struct sh_dnode_s *d = dnode(sh, key, false);
  struct sh_rvec_s *v = &sh->rcol[SH_COL(key)];
  int r = SH_ROW(key);
  int c = SH_COL(key);
  int i;

  if (d != NULL)
    {
      for (i = 0; i < d->n; i++)
        {
          fn(sh, d->d[i], arg);
        }
    }

  for (i = 0; i < v->n; i++)
    {
      if (r >= v->d[i].r0 && r <= v->d[i].r1)
        {
          fn(sh, v->d[i].dep, arg);
        }
    }

  v = &sh->rwide;
  for (i = 0; i < v->n; i++)
    {
      if (r >= v->d[i].r0 && r <= v->d[i].r1 && c >= v->d[i].c0 &&
          c <= v->d[i].c1)
        {
          fn(sh, v->d[i].dep, arg);
        }
    }
}

/****************************************************************************
 * Private Functions: values
 ****************************************************************************/

static void v_num(struct sv_s *v, double n)
{
  v->t = isfinite(n) ? SV_NUM : SV_ERR;
  v->e = isfinite(n) ? SE_NONE : SE_NUM;
  v->n = n;
  v->s = NULL;
}

static void v_err(struct sv_s *v, int e)
{
  v->t = SV_ERR;
  v->e = e;
  v->n = 0;
  v->s = NULL;
}

static void v_bool(struct sv_s *v, bool b)
{
  v->t = SV_BOOL;
  v->e = SE_NONE;
  v->n = b ? 1 : 0;
  v->s = NULL;
}

static void v_str(struct sv_s *v, const char *s)
{
  v->t = s != NULL ? SV_STR : SV_ERR;
  v->e = s != NULL ? SE_NONE : SE_VALUE;
  v->n = 0;
  v->s = s;
}

/* A string of n bytes in the arena (NULL: full) */

static char *arena(struct ev_s *e, int n)
{
  struct sheet_s *sh = e->sh;
  char *p;

  if (sh->arena == NULL)
    {
      sh->arena = malloc(ARENA);
      sh->arenan = 0;
      if (sh->arena == NULL)
        {
          return NULL;
        }
    }

  if (sh->arenan + n + 1 > ARENA)
    {
      return NULL;
    }

  p = sh->arena + sh->arenan;
  sh->arenan += n + 1;
  p[n] = '\0';
  return p;
}

static void cell_value(struct sh_cell_s *cell, struct sv_s *v)
{
  if (cell == NULL || cell->kind == SK_NONE)
    {
      v->t = SV_EMPTY;
      v->e = SE_NONE;
      v->n = 0;
      v->s = NULL;
      return;
    }

  v->t = cell->vt;
  v->e = cell->err;
  v->n = cell->num;
  v->s = cell->vt == SV_STR ? (cell->str != NULL ? cell->str : "") : NULL;
}

/* A range of one cell is that cell's value; of more, #VALUE! (no implicit
 * intersection)
 */

static void deref(struct ev_s *e, struct sv_s *v)
{
  if (v->t != SV_RANGE)
    {
      return;
    }

  if (v->r0 == v->r1 && v->c0 == v->c1)
    {
      cell_value(find(e->sh, SH_KEY(v->r0, v->c0)), v);
    }
  else
    {
      v_err(v, SE_VALUE);
    }
}

/* Text as a number: the typed forms (1,234  $5  15%  1e3) */

static bool text_number(const char *s, double *v);

static bool to_num(struct ev_s *e, struct sv_s *v, double *n, int *err)
{
  deref(e, v);
  switch (v->t)
    {
      case SV_EMPTY:
        *n = 0;
        return true;

      case SV_NUM:
      case SV_BOOL:
        *n = v->n;
        return true;

      case SV_STR:
        if (text_number(v->s, n))
          {
            return true;
          }

        *err = SE_VALUE;
        return false;

      case SV_ERR:
        *err = v->e;
        return false;

      default:
        *err = SE_VALUE;
        return false;
    }
}

static const char *to_str(struct ev_s *e, struct sv_s *v, int *err)
{
  char buf[40];
  char *s;

  deref(e, v);
  switch (v->t)
    {
      case SV_EMPTY:
        return "";

      case SV_STR:
        return v->s;

      case SV_BOOL:
        return v->n != 0 ? "TRUE" : "FALSE";

      case SV_NUM:
        num_plain(v->n, 15, buf, sizeof(buf));
        s = arena(e, strlen(buf));
        if (s == NULL)
          {
            *err = SE_VALUE;
            return NULL;
          }

        strcpy(s, buf);
        return s;

      case SV_ERR:
        *err = v->e;
        return NULL;

      default:
        *err = SE_VALUE;
        return NULL;
    }
}

static bool to_bool(struct ev_s *e, struct sv_s *v, bool *b, int *err)
{
  deref(e, v);
  switch (v->t)
    {
      case SV_EMPTY:
        *b = false;
        return true;

      case SV_NUM:
      case SV_BOOL:
        *b = v->n != 0;
        return true;

      case SV_STR:
        if (strcasecmp(v->s, "TRUE") == 0 || strcasecmp(v->s, "FALSE") == 0)
          {
            *b = toupper((unsigned char)v->s[0]) == 'T';
            return true;
          }

        *err = SE_VALUE;
        return false;

      case SV_ERR:
        *err = v->e;
        return false;

      default:
        *err = SE_VALUE;
        return false;
    }
}

/* Comparing values as the spreadsheets do: numbers < text < booleans;
 * text without regard to case; empty is 0, "" or FALSE
 */

static int cmp_values(const struct sv_s *a, const struct sv_s *b)
{
  int ta = a->t;
  int tb = b->t;
  double x;
  double y;

  if (ta == SV_EMPTY)
    {
      ta = tb == SV_STR ? SV_STR : tb == SV_BOOL ? SV_BOOL : SV_NUM;
    }

  if (tb == SV_EMPTY)
    {
      tb = ta == SV_STR ? SV_STR : ta == SV_BOOL ? SV_BOOL : SV_NUM;
    }

  if (ta != tb)
    {
      static const int rank[] =
      {
        0, 0, 1, 2, 3, 3
      };

      return rank[ta] < rank[tb] ? -1 : 1;
    }

  if (ta == SV_STR)
    {
      return strcasecmp(a->t == SV_EMPTY ? "" : a->s,
                        b->t == SV_EMPTY ? "" : b->s);
    }

  x = a->t == SV_EMPTY ? 0 : a->n;
  y = b->t == SV_EMPTY ? 0 : b->n;
  return x < y ? -1 : x > y ? 1 : 0;
}

/****************************************************************************
 * Private Functions: ranges
 ****************************************************************************/

typedef bool (*cell_fn_t)(void *arg, struct sh_cell_s *c);

/* The cells there are in a range, in no particular order: each probed
 * when the range is small, else the whole table looked through
 */

static void range_each(struct sheet_s *sh, int r0, int c0, int r1, int c1,
                       cell_fn_t fn, void *arg)
{
  uint64_t area = (uint64_t)(r1 - r0 + 1) * (uint64_t)(c1 - c0 + 1);
  uint32_t i;
  int r;
  int c;

  if (area <= (uint64_t)sh->n * 2 + 64)
    {
      for (r = r0; r <= r1; r++)
        {
          for (c = c0; c <= c1; c++)
            {
              struct sh_cell_s *cell = find(sh, SH_KEY(r, c));

              if (cell != NULL && cell->kind != SK_NONE && !fn(arg, cell))
                {
                  return;
                }
            }
        }

      return;
    }

  for (i = 0; i < sh->cap; i++)
    {
      struct sh_cell_s *cell = sh->tab[i];

      if (cell != NULL && cell->kind != SK_NONE &&
          SH_ROW(cell->key) >= r0 && SH_ROW(cell->key) <= r1 &&
          SH_COL(cell->key) >= c0 && SH_COL(cell->key) <= c1 &&
          !fn(arg, cell))
        {
          return;
        }
    }
}

static int key_cmp(const void *a, const void *b)
{
  uint32_t x = (*(struct sh_cell_s * const *)a)->key;
  uint32_t y = (*(struct sh_cell_s * const *)b)->key;
  int rx = SH_ROW(x);
  int ry = SH_ROW(y);

  if (rx != ry)
    {
      return rx < ry ? -1 : 1;
    }

  return SH_COL(x) < SH_COL(y) ? -1 : SH_COL(x) > SH_COL(y) ? 1 : 0;
}

struct collect_s
{
  struct sh_cell_s **v;
  int n;
  int cap;
  bool all;                              /* Format-only cells too */
};

static bool collect_one(void *arg, struct sh_cell_s *c)
{
  struct collect_s *cl = arg;

  if (cl->n == cl->cap)
    {
      int cap = cl->cap < 16 ? 16 : cl->cap * 2;
      struct sh_cell_s **p = realloc(cl->v, cap * sizeof(*p));

      if (p == NULL)
        {
          return false;
        }

      cl->v = p;
      cl->cap = cap;
    }

  cl->v[cl->n++] = c;
  return true;
}

static void collect(struct sheet_s *sh, const struct sh_rect_s *r,
                    struct collect_s *cl, bool all)
{
  uint64_t area = (uint64_t)(r->r1 - r->r0 + 1) *
                  (uint64_t)(r->c1 - r->c0 + 1);
  uint32_t i;
  int y;
  int x;

  memset(cl, 0, sizeof(*cl));
  if (area <= (uint64_t)sh->n * 2 + 64)
    {
      for (y = r->r0; y <= r->r1; y++)
        {
          for (x = r->c0; x <= r->c1; x++)
            {
              struct sh_cell_s *c = find(sh, SH_KEY(y, x));

              if (c != NULL && (all || c->kind != SK_NONE))
                {
                  collect_one(cl, c);
                }
            }
        }

      return;
    }

  for (i = 0; i < sh->cap; i++)
    {
      struct sh_cell_s *c = sh->tab[i];

      if (c != NULL && (all || c->kind != SK_NONE) &&
          SH_ROW(c->key) >= r->r0 && SH_ROW(c->key) <= r->r1 &&
          SH_COL(c->key) >= r->c0 && SH_COL(c->key) <= r->c1)
        {
          collect_one(cl, c);
        }
    }

  if (cl->n > 1)
    {
      qsort(cl->v, cl->n, sizeof(*cl->v), key_cmp);
    }
}

void sheet_each_in(struct sheet_s *sh, const struct sh_rect_s *r,
                   bool (*fn)(void *arg, struct sh_cell_s *cell), void *arg)
{
  range_each(sh, r->r0, r->c0, r->r1, r->c1, fn, arg);
}

struct sh_cell_s **sheet_cells_in(struct sheet_s *sh,
                                  const struct sh_rect_s *r, int *n)
{
  struct collect_s cl;

  collect(sh, r, &cl, true);
  *n = cl.n;
  return cl.v;
}

/****************************************************************************
 * Private Functions: the functions
 ****************************************************************************/

/* Aggregates: SUM, AVERAGE, MIN, MAX, COUNT, COUNTA, PRODUCT.  In ranges
 * only numbers count (COUNTA: anything); given directly, booleans and
 * numbers as text count too.  An error anywhere is the answer.
 */

struct agg_s
{
  double sum;
  double prod;
  double min;
  double max;
  int count;
  int counta;
  int err;
};

static void agg_num(struct agg_s *g, double v)
{
  g->sum += v;
  g->prod *= v;
  g->min = g->count == 0 || v < g->min ? v : g->min;
  g->max = g->count == 0 || v > g->max ? v : g->max;
  g->count++;
}

static bool agg_cell(void *arg, struct sh_cell_s *c)
{
  struct agg_s *g = arg;

  g->counta++;
  if (c->vt == SV_ERR)
    {
      g->err = g->err != SE_NONE ? g->err : c->err;
    }
  else if (c->vt == SV_NUM)
    {
      agg_num(g, c->num);
    }

  return true;
}

static void aggregate(struct ev_s *e, struct sv_s *a, int n,
                      struct agg_s *g)
{
  int i;

  memset(g, 0, sizeof(*g));
  g->prod = 1;
  for (i = 0; i < n; i++)
    {
      struct sv_s *v = &a[i];
      double x;

      if (v->t == SV_RANGE)
        {
          range_each(e->sh, v->r0, v->c0, v->r1, v->c1, agg_cell, g);
          continue;
        }

      switch (v->t)
        {
          case SV_EMPTY:
            break;

          case SV_NUM:
          case SV_BOOL:
            g->counta++;
            agg_num(g, v->n);
            break;

          case SV_STR:
            g->counta++;
            if (text_number(v->s, &x))
              {
                agg_num(g, x);
              }
            else
              {
                g->err = g->err != SE_NONE ? g->err : SE_VALUE;
              }
            break;

          case SV_ERR:
            g->counta++;
            g->err = g->err != SE_NONE ? g->err : v->e;
            break;
        }
    }
}

static void f_sum(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  struct agg_s g;

  aggregate(e, a, n, &g);
  g.err ? v_err(out, g.err) : v_num(out, g.sum);
}

static void f_product(struct ev_s *e, struct sv_s *a, int n,
                      struct sv_s *out)
{
  struct agg_s g;

  aggregate(e, a, n, &g);
  g.err ? v_err(out, g.err) : v_num(out, g.count ? g.prod : 0);
}

static void f_average(struct ev_s *e, struct sv_s *a, int n,
                      struct sv_s *out)
{
  struct agg_s g;

  aggregate(e, a, n, &g);
  if (g.err)
    {
      v_err(out, g.err);
    }
  else if (g.count == 0)
    {
      v_err(out, SE_DIV0);
    }
  else
    {
      v_num(out, g.sum / g.count);
    }
}

static void f_min(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  struct agg_s g;

  aggregate(e, a, n, &g);
  g.err ? v_err(out, g.err) : v_num(out, g.count ? g.min : 0);
}

static void f_max(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  struct agg_s g;

  aggregate(e, a, n, &g);
  g.err ? v_err(out, g.err) : v_num(out, g.count ? g.max : 0);
}

/* COUNT and COUNTA do not fail on errors: they count */

static bool count_cell(void *arg, struct sh_cell_s *c)
{
  int *k = arg;

  k[0] += c->vt == SV_NUM;
  k[1]++;
  return true;
}

static void counts(struct ev_s *e, struct sv_s *a, int n, int *k)
{
  double x;
  int i;

  k[0] = 0;
  k[1] = 0;
  for (i = 0; i < n; i++)
    {
      struct sv_s *v = &a[i];

      if (v->t == SV_RANGE)
        {
          range_each(e->sh, v->r0, v->c0, v->r1, v->c1, count_cell, k);
        }
      else if (v->t != SV_EMPTY)
        {
          k[1]++;
          k[0] += v->t == SV_NUM || v->t == SV_BOOL ||
                  (v->t == SV_STR && text_number(v->s, &x));
        }
    }
}

static void f_count(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  int k[2];

  counts(e, a, n, k);
  v_num(out, k[0]);
}

static void f_counta(struct ev_s *e, struct sv_s *a, int n,
                     struct sv_s *out)
{
  int k[2];

  counts(e, a, n, k);
  v_num(out, k[1]);
}

static void f_if(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  int err = SE_NONE;
  bool b;

  if (!to_bool(e, &a[0], &b, &err))
    {
      v_err(out, err);
      return;
    }

  if (b)
    {
      *out = a[1];
    }
  else if (n > 2)
    {
      *out = a[2];
    }
  else
    {
      v_bool(out, false);
    }

  if (out->t == SV_EMPTY && ((b && n > 1) || (!b && n > 2)))
    {
      v_num(out, 0);                     /* IF(x,,) is 0 */
    }
}

/* AND, OR: booleans (and numbers); text in ranges is left out */

struct logic_s
{
  bool and;
  bool r;
  int count;
  int err;
};

static bool logic_cell(void *arg, struct sh_cell_s *c)
{
  struct logic_s *l = arg;

  if (c->vt == SV_ERR)
    {
      l->err = l->err != SE_NONE ? l->err : c->err;
    }
  else if (c->vt == SV_NUM || c->vt == SV_BOOL)
    {
      l->r = l->and ? (l->r && c->num != 0) : (l->r || c->num != 0);
      l->count++;
    }

  return true;
}

static void logic(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out,
                  bool and)
{
  struct logic_s l;
  int i;

  l.and = and;
  l.r = and;
  l.count = 0;
  l.err = SE_NONE;
  for (i = 0; i < n; i++)
    {
      bool b;

      if (a[i].t == SV_RANGE)
        {
          range_each(e->sh, a[i].r0, a[i].c0, a[i].r1, a[i].c1, logic_cell,
                     &l);
        }
      else if (a[i].t != SV_EMPTY)
        {
          if (!to_bool(e, &a[i], &b, &l.err))
            {
              break;
            }

          l.r = and ? (l.r && b) : (l.r || b);
          l.count++;
        }
    }

  if (l.err != SE_NONE)
    {
      v_err(out, l.err);
    }
  else if (l.count == 0)
    {
      v_err(out, SE_VALUE);
    }
  else
    {
      v_bool(out, l.r);
    }
}

static void f_and(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  logic(e, a, n, out, true);
}

static void f_or(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  logic(e, a, n, out, false);
}

static void f_not(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  int err = SE_NONE;
  bool b;

  to_bool(e, &a[0], &b, &err) ? v_bool(out, !b) : v_err(out, err);
}

/* Numbers in, a number out */

static bool args_num(struct ev_s *e, struct sv_s *a, int n, double *x,
                     struct sv_s *out)
{
  int err = SE_NONE;
  int i;

  for (i = 0; i < n; i++)
    {
      if (!to_num(e, &a[i], &x[i], &err))
        {
          v_err(out, err);
          return false;
        }
    }

  return true;
}

/* x rounded to d decimals: half away from zero, the last bit forgiven
 * (ROUND(2.675, 2) is 2.68, as the decimal number typed)
 */

static double round_to(double x, int d, int how)
{
  double p = num_p10(d < 0 ? -d : d);
  double y = fabs(x);
  double r;

  d > 0 ? (y *= p) : (y /= p);
  if (how == 0)
    {
      r = floor(y + 0.5 + y * 4e-16);
    }
  else if (how > 0)
    {
      r = ceil(y - y * 4e-16);
    }
  else
    {
      r = floor(y + y * 4e-16);
    }

  r = d > 0 ? r / p : r * p;
  return x < 0 ? -r : r;
}

static void rounding(struct ev_s *e, struct sv_s *a, int n,
                     struct sv_s *out, int how)
{
  double x[2] =
    {
      0, 0
    };

  if (args_num(e, a, n, x, out))
    {
      v_num(out, round_to(x[0], (int)x[1], how));
    }
}

static void f_round(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  rounding(e, a, n, out, 0);
}

static void f_roundup(struct ev_s *e, struct sv_s *a, int n,
                      struct sv_s *out)
{
  rounding(e, a, n, out, 1);
}

static void f_rounddown(struct ev_s *e, struct sv_s *a, int n,
                        struct sv_s *out)
{
  rounding(e, a, n, out, -1);
}

static void f_math(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out,
                   int which)
{
  double x[2];

  if (!args_num(e, a, n, x, out))
    {
      return;
    }

  switch (which)
    {
      case 0:
        v_num(out, fabs(x[0]));
        break;

      case 1:
        v_num(out, floor(x[0]));
        break;

      case 2:
        if (x[1] == 0)
          {
            v_err(out, SE_DIV0);
          }
        else
          {
            v_num(out, x[0] - x[1] * floor(x[0] / x[1]));
          }
        break;

      case 3:
        x[0] < 0 ? v_err(out, SE_NUM) : v_num(out, sqrt(x[0]));
        break;

      case 4:
        if (x[0] == 0 && x[1] < 0)
          {
            v_err(out, SE_DIV0);
          }
        else if (x[0] < 0 && x[1] != floor(x[1]))
          {
            v_err(out, SE_NUM);
          }
        else
          {
            v_num(out, pow(x[0], x[1]));
          }
        break;

      case 5:
        v_num(out, exp(x[0]));
        break;

      case 6:
        x[0] <= 0 ? v_err(out, SE_NUM) : v_num(out, log(x[0]));
        break;

      case 7:
        x[0] <= 0 ? v_err(out, SE_NUM) : v_num(out, log10(x[0]));
        break;
    }
}

static void f_abs(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  f_math(e, a, n, out, 0);
}

static void f_int(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  f_math(e, a, n, out, 1);
}

static void f_mod(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  f_math(e, a, n, out, 2);
}

static void f_sqrt(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  f_math(e, a, n, out, 3);
}

static void f_power(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  f_math(e, a, n, out, 4);
}

static void f_exp(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  f_math(e, a, n, out, 5);
}

static void f_ln(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  f_math(e, a, n, out, 6);
}

static void f_log10(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  f_math(e, a, n, out, 7);
}

static void f_pi(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  v_num(out, 3.14159265358979323846);
}

/* Text */

static void f_len(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  int err = SE_NONE;
  const char *s = to_str(e, &a[0], &err);

  s != NULL ? v_num(out, strlen(s)) : v_err(out, err);
}

static void substr(struct ev_s *e, struct sv_s *out, const char *s,
                   int from, int len)
{
  int sl = strlen(s);
  char *p;

  from = from < 0 ? 0 : from > sl ? sl : from;
  len = len < 0 ? 0 : len > sl - from ? sl - from : len;
  p = arena(e, len);
  if (p == NULL)
    {
      v_err(out, SE_VALUE);
      return;
    }

  memcpy(p, s + from, len);
  v_str(out, p);
}

static void leftright(struct ev_s *e, struct sv_s *a, int n,
                      struct sv_s *out, bool right)
{
  int err = SE_NONE;
  const char *s = to_str(e, &a[0], &err);
  double k = 1;

  if (s == NULL || (n > 1 && a[1].t != SV_EMPTY &&
                    !to_num(e, &a[1], &k, &err)))
    {
      v_err(out, err);
      return;
    }

  if (k < 0)
    {
      v_err(out, SE_VALUE);
      return;
    }

  k = k > 1e6 ? 1e6 : k;
  substr(e, out, s, right ? (int)strlen(s) - (int)k : 0, (int)k);
}

static void f_left(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  leftright(e, a, n, out, false);
}

static void f_right(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  leftright(e, a, n, out, true);
}

static void f_mid(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  int err = SE_NONE;
  const char *s = to_str(e, &a[0], &err);
  double x[2];

  if (s == NULL || !to_num(e, &a[1], &x[0], &err) ||
      !to_num(e, &a[2], &x[1], &err))
    {
      v_err(out, err);
      return;
    }

  if (x[0] < 1 || x[1] < 0)
    {
      v_err(out, SE_VALUE);
      return;
    }

  substr(e, out, s, x[0] > 1e6 ? 1000000 : (int)x[0] - 1,
         x[1] > 1e6 ? 1000000 : (int)x[1]);
}

static void recase(struct ev_s *e, struct sv_s *a, struct sv_s *out,
                   int how)
{
  int err = SE_NONE;
  const char *s = to_str(e, &a[0], &err);
  char *p;
  int i;
  int j = 0;

  if (s == NULL)
    {
      v_err(out, err);
      return;
    }

  p = arena(e, strlen(s));
  if (p == NULL)
    {
      v_err(out, SE_VALUE);
      return;
    }

  for (i = 0; s[i] != '\0'; i++)
    {
      if (how == 2)
        {
          /* TRIM: no spaces at the ends, one between words */

          if (s[i] == ' ' && (j == 0 || p[j - 1] == ' '))
            {
              continue;
            }

          p[j++] = s[i];
        }
      else
        {
          p[j++] = how ? toupper((unsigned char)s[i]) :
                         tolower((unsigned char)s[i]);
        }
    }

  while (how == 2 && j > 0 && p[j - 1] == ' ')
    {
      j--;
    }

  p[j] = '\0';
  v_str(out, p);
}

static void f_upper(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  recase(e, a, out, 1);
}

static void f_lower(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  recase(e, a, out, 0);
}

static void f_trim(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  recase(e, a, out, 2);
}

/* CONCAT: every argument, ranges row by row, gathered in a buffer of its
 * own and put in the arena at the end
 */

static bool cat_piece(struct ev_s *e, char **p, int *len, const char *s)
{
  int n = strlen(s);
  char *q;

  if (n == 0)
    {
      return true;
    }

  if (*len + n >= ARENA)
    {
      return false;
    }

  q = realloc(*p, *len + n + 1);
  if (q == NULL)
    {
      return false;
    }

  memcpy(q + *len, s, n);
  *len += n;
  q[*len] = '\0';
  *p = q;
  return true;
}

static void f_concat(struct ev_s *e, struct sv_s *a, int n,
                     struct sv_s *out)
{
  char *p = NULL;
  char *r;
  int len = 0;
  int err = SE_NONE;
  int i;
  int j;

  for (i = 0; i < n; i++)
    {
      if (a[i].t == SV_RANGE)
        {
          struct sh_rect_s rr;
          struct collect_s cl;

          rr.r0 = a[i].r0;
          rr.c0 = a[i].c0;
          rr.r1 = a[i].r1;
          rr.c1 = a[i].c1;
          collect(e->sh, &rr, &cl, false);
          for (j = 0; j < cl.n; j++)
            {
              struct sv_s v;
              const char *s;

              cell_value(cl.v[j], &v);
              s = to_str(e, &v, &err);
              if (s == NULL || !cat_piece(e, &p, &len, s))
                {
                  free(cl.v);
                  free(p);
                  v_err(out, err != SE_NONE ? err : SE_VALUE);
                  return;
                }
            }

          free(cl.v);
        }
      else
        {
          const char *s = to_str(e, &a[i], &err);

          if (s == NULL || !cat_piece(e, &p, &len, s))
            {
              free(p);
              v_err(out, err != SE_NONE ? err : SE_VALUE);
              return;
            }
        }
    }

  r = arena(e, len);
  if (r != NULL && len > 0)
    {
      memcpy(r, p, len);
    }

  free(p);
  v_str(out, r);
}

/* TEXT(value, format): the basic formats.  Dates by yyyy, yy, mm (months,
 * or minutes after hh), dd, hh, ss; numbers by 0 and # (the decimals
 * after '.'), ',' groups, '%', and text around them ("$#,##0.00")
 */

static void text_format(double v, const char *fmt, char *out, size_t size)
{
  const char *p;
  size_t o = 0;

  if (strpbrk(fmt, "yYdD") != NULL || strstr(fmt, "hh") != NULL ||
      strstr(fmt, "HH") != NULL)
    {
      int y;
      int m;
      int d;
      int hh;
      int mm;
      int ss;
      bool hour = false;

      sheet_date(v, &y, &m, &d, &hh, &mm, &ss);
      for (p = fmt; *p != '\0' && o + 8 < size; )
        {
          int k = 1;
          char c = tolower((unsigned char)*p);

          while (tolower((unsigned char)p[k]) == c && c != '\0' &&
                 strchr("ymdhs", c) != NULL)
            {
              k++;
            }

          switch (c)
            {
              case 'y':
                o += snprintf(out + o, size - o, k <= 2 ? "%02d" : "%04d",
                              k <= 2 ? y % 100 : y);
                break;

              case 'm':
                o += snprintf(out + o, size - o, k >= 2 ? "%02d" : "%d",
                              hour ? mm : m);
                break;

              case 'd':
                o += snprintf(out + o, size - o, k >= 2 ? "%02d" : "%d", d);
                break;

              case 'h':
                o += snprintf(out + o, size - o, k >= 2 ? "%02d" : "%d", hh);
                hour = true;
                break;

              case 's':
                o += snprintf(out + o, size - o, "%02d", ss);
                break;

              default:
                out[o++] = *p;
                k = 1;
                break;
            }

          p += k;
        }

      out[o] = '\0';
      return;
    }

  {
    const char *first = strpbrk(fmt, "0#");
    const char *last;
    const char *dot;
    char num[48];
    int dec = 0;
    bool pct = strchr(fmt, '%') != NULL;

    if (first == NULL)
      {
        strlcpy(out, fmt, size);
        return;
      }

    last = first;
    for (p = first; *p == '0' || *p == '#' || *p == ',' || *p == '.'; p++)
      {
        last = p;
      }

    dot = memchr(first, '.', last - first + 1);
    if (dot != NULL)
      {
        for (p = dot + 1; p <= last && (*p == '0' || *p == '#'); p++)
          {
            dec++;
          }
      }

    num_fixed(pct ? v * 100 : v, dec,
              memchr(first, ',', last - first + 1) != NULL, num,
              sizeof(num));
    snprintf(out, size, "%.*s%s%s", (int)(first - fmt), fmt, num, last + 1);
  }
}

static void f_text(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  int err = SE_NONE;
  const char *fmt = to_str(e, &a[1], &err);
  char buf[96];
  double v;
  char *p;

  deref(e, &a[0]);
  if (fmt == NULL)
    {
      v_err(out, err);
      return;
    }

  if (a[0].t == SV_STR && !text_number(a[0].s, &v))
    {
      *out = a[0];                       /* Text stays as it is */
      return;
    }

  if (!to_num(e, &a[0], &v, &err))
    {
      v_err(out, err);
      return;
    }

  text_format(v, fmt, buf, sizeof(buf));
  p = arena(e, strlen(buf));
  if (p == NULL)
    {
      v_err(out, SE_VALUE);
      return;
    }

  strcpy(p, buf);
  v_str(out, p);
}

static void f_now(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  v_num(out, e->sh->now());
}

static void f_today(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  v_num(out, floor(e->sh->now()));
}

/* Criteria (SUMIF, COUNTIF): a value (equal to it), or text with an
 * operator in front (">=10", "<>done"); text compares without regard to
 * case, with * and ? as wildcards for = and <>
 */

struct crit_s
{
  int op;                                /* T_EQ ... T_GE */
  struct sv_s v;
  char text[64];
};

static bool wild(const char *p, const char *s)
{
  for (; *p != '\0'; p++, s++)
    {
      if (*p == '*')
        {
          for (; ; s++)
            {
              if (wild(p + 1, s))
                {
                  return true;
                }

              if (*s == '\0')
                {
                  return false;
                }
            }
        }

      if (*s == '\0' || (*p != '?' &&
                         tolower((unsigned char)*p) !=
                         tolower((unsigned char)*s)))
        {
          return false;
        }
    }

  return *s == '\0';
}

static bool crit_make(struct ev_s *e, struct sv_s *a, struct crit_s *c)
{
  const char *s;
  double x;

  deref(e, a);
  c->op = T_EQ;
  if (a->t != SV_STR)
    {
      c->v = *a;
      if (c->v.t == SV_EMPTY)
        {
          v_num(&c->v, 0);
        }

      return a->t != SV_ERR;
    }

  s = a->s;
  if (s[0] == '<' && s[1] == '>')
    {
      c->op = T_NE;
      s += 2;
    }
  else if (s[0] == '<' && s[1] == '=')
    {
      c->op = T_LE;
      s += 2;
    }
  else if (s[0] == '>' && s[1] == '=')
    {
      c->op = T_GE;
      s += 2;
    }
  else if (s[0] == '<' || s[0] == '>' || s[0] == '=')
    {
      c->op = s[0] == '<' ? T_LT : s[0] == '>' ? T_GT : T_EQ;
      s++;
    }

  strlcpy(c->text, s, sizeof(c->text));
  if (text_number(s, &x))
    {
      v_num(&c->v, x);
    }
  else if (strcasecmp(s, "TRUE") == 0 || strcasecmp(s, "FALSE") == 0)
    {
      v_bool(&c->v, toupper((unsigned char)s[0]) == 'T');
    }
  else
    {
      v_str(&c->v, c->text);
    }

  return true;
}

static bool crit_match(const struct crit_s *c, struct sh_cell_s *cell)
{
  struct sv_s v;
  int k;

  cell_value(cell, &v);
  if (v.t == SV_ERR || (v.t == SV_EMPTY && c->v.t != SV_STR))
    {
      return c->op == T_NE;
    }

  if (c->v.t == SV_STR && (c->op == T_EQ || c->op == T_NE))
    {
      bool m = v.t == SV_STR && wild(c->text, v.s);

      return c->op == T_EQ ? m : !m;
    }

  if ((c->v.t == SV_STR) != (v.t == SV_STR) ||
      (c->v.t == SV_BOOL) != (v.t == SV_BOOL))
    {
      return c->op == T_NE;              /* Types differ */
    }

  k = cmp_values(&v, &c->v);
  switch (c->op)
    {
      case T_EQ:
        return k == 0;

      case T_NE:
        return k != 0;

      case T_LT:
        return k < 0;

      case T_GT:
        return k > 0;

      case T_LE:
        return k <= 0;

      default:
        return k >= 0;
    }
}

struct ifs_s
{
  struct ev_s *e;
  struct crit_s crit;
  int r0;                                /* The range's corner */
  int c0;
  int sr0;                               /* The sums' corner */
  int sc0;
  bool sum;
  double total;
  int count;
  int err;
};

static bool ifs_cell(void *arg, struct sh_cell_s *cell)
{
  struct ifs_s *f = arg;

  if (!crit_match(&f->crit, cell))
    {
      return true;
    }

  f->count++;
  if (f->sum)
    {
      int r = SH_ROW(cell->key) - f->r0 + f->sr0;
      int c = SH_COL(cell->key) - f->c0 + f->sc0;
      struct sh_cell_s *s = r < SH_ROWS && c < SH_COLS ?
                            find(f->e->sh, SH_KEY(r, c)) : NULL;

      if (s != NULL && s->vt == SV_NUM)
        {
          f->total += s->num;
        }
      else if (s != NULL && s->vt == SV_ERR)
        {
          f->err = f->err != SE_NONE ? f->err : s->err;
        }
    }

  return true;
}

static void ifs(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out,
                bool sum)
{
  struct ifs_s f;

  if (a[0].t != SV_RANGE || (n > 2 && a[2].t != SV_RANGE))
    {
      v_err(out, SE_VALUE);
      return;
    }

  memset(&f, 0, sizeof(f));
  f.e = e;
  f.sum = sum;
  f.r0 = a[0].r0;
  f.c0 = a[0].c0;
  f.sr0 = n > 2 ? a[2].r0 : a[0].r0;
  f.sc0 = n > 2 ? a[2].c0 : a[0].c0;
  if (!crit_make(e, &a[1], &f.crit))
    {
      v_err(out, a[1].e);
      return;
    }

  range_each(e->sh, a[0].r0, a[0].c0, a[0].r1, a[0].c1, ifs_cell, &f);
  if (f.err != SE_NONE)
    {
      v_err(out, f.err);
    }
  else
    {
      v_num(out, sum ? f.total : f.count);
    }
}

static void f_sumif(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  ifs(e, a, n, out, true);
}

static void f_countif(struct ev_s *e, struct sv_s *a, int n,
                      struct sv_s *out)
{
  ifs(e, a, n, out, false);
}

/* VLOOKUP(value, table, column, [sorted = TRUE]): the first match down
 * the table's first column (sorted: the last row not above the value)
 */

static void f_vlookup(struct ev_s *e, struct sv_s *a, int n,
                      struct sv_s *out)
{
  struct sh_rect_s r;
  struct collect_s cl;
  struct sv_s key;
  int err = SE_NONE;
  bool sorted = true;
  double col;
  int hit = -1;
  int i;

  if (a[1].t != SV_RANGE)
    {
      v_err(out, SE_VALUE);
      return;
    }

  key = a[0];
  deref(e, &key);
  if (key.t == SV_ERR)
    {
      v_err(out, key.e);
      return;
    }

  if (!to_num(e, &a[2], &col, &err) ||
      (n > 3 && a[3].t != SV_EMPTY && !to_bool(e, &a[3], &sorted, &err)))
    {
      v_err(out, err);
      return;
    }

  if (col < 1)
    {
      v_err(out, SE_VALUE);
      return;
    }

  if (col > a[1].c1 - a[1].c0 + 1)
    {
      v_err(out, SE_REF);
      return;
    }

  r.r0 = a[1].r0;
  r.r1 = a[1].r1;
  r.c0 = a[1].c0;
  r.c1 = a[1].c0;
  collect(e->sh, &r, &cl, false);
  for (i = 0; i < cl.n; i++)
    {
      struct sv_s v;
      int k;

      cell_value(cl.v[i], &v);
      if (v.t == SV_ERR || (v.t == SV_STR) != (key.t == SV_STR))
        {
          continue;
        }

      k = cmp_values(&v, &key);
      if (!sorted)
        {
          if (v.t == SV_STR ? wild(key.s, v.s) : k == 0)
            {
              hit = i;
              break;
            }
        }
      else if (k <= 0)
        {
          hit = i;
        }
      else
        {
          break;
        }
    }

  if (hit < 0)
    {
      v_err(out, SE_NA);
    }
  else
    {
      cell_value(find(e->sh, SH_KEY(SH_ROW(cl.v[hit]->key),
                                    a[1].c0 + (int)col - 1)), out);
      if (out->t == SV_EMPTY)
        {
          v_num(out, 0);
        }
    }

  free(cl.v);
}

/* PMT(rate, periods, present value, [future value], [type]) */

static void f_pmt(struct ev_s *e, struct sv_s *a, int n, struct sv_s *out)
{
  double x[5] =
    {
      0, 0, 0, 0, 0
    };

  double q;

  if (!args_num(e, a, n, x, out))
    {
      return;
    }

  if (x[1] == 0)
    {
      v_err(out, SE_NUM);
      return;
    }

  if (x[0] == 0)
    {
      v_num(out, -(x[2] + x[3]) / x[1]);
      return;
    }

  q = pow(1 + x[0], x[1]);
  v_num(out, -x[0] * (x[3] + x[2] * q) /
             ((1 + x[0] * (x[4] != 0)) * (q - 1)));
}

static void f_iferror(struct ev_s *e, struct sv_s *a, int n,
                      struct sv_s *out)
{
  deref(e, &a[0]);
  *out = a[0].t == SV_ERR ? a[1] : a[0];
  deref(e, out);
  if (out->t == SV_EMPTY)
    {
      v_num(out, 0);
    }
}

static void is_what(struct ev_s *e, struct sv_s *a, struct sv_s *out,
                    int t)
{
  deref(e, &a[0]);
  v_bool(out, a[0].t == t);
}

static void f_isblank(struct ev_s *e, struct sv_s *a, int n,
                      struct sv_s *out)
{
  is_what(e, a, out, SV_EMPTY);
}

static void f_isnumber(struct ev_s *e, struct sv_s *a, int n,
                       struct sv_s *out)
{
  is_what(e, a, out, SV_NUM);
}

static void f_istext(struct ev_s *e, struct sv_s *a, int n,
                     struct sv_s *out)
{
  is_what(e, a, out, SV_STR);
}

static void f_iserror(struct ev_s *e, struct sv_s *a, int n,
                      struct sv_s *out)
{
  is_what(e, a, out, SV_ERR);
}

static const struct fn_s g_fn[] =
{
  { "SUM",         1, 255, false, f_sum },
  { "AVERAGE",     1, 255, false, f_average },
  { "MIN",         1, 255, false, f_min },
  { "MAX",         1, 255, false, f_max },
  { "COUNT",       1, 255, false, f_count },
  { "COUNTA",      1, 255, false, f_counta },
  { "PRODUCT",     1, 255, false, f_product },
  { "IF",          2, 3,   false, f_if },
  { "AND",         1, 255, false, f_and },
  { "OR",          1, 255, false, f_or },
  { "NOT",         1, 1,   false, f_not },
  { "ROUND",       2, 2,   false, f_round },
  { "ROUNDUP",     2, 2,   false, f_roundup },
  { "ROUNDDOWN",   2, 2,   false, f_rounddown },
  { "ABS",         1, 1,   false, f_abs },
  { "INT",         1, 1,   false, f_int },
  { "MOD",         2, 2,   false, f_mod },
  { "SQRT",        1, 1,   false, f_sqrt },
  { "POWER",       2, 2,   false, f_power },
  { "EXP",         1, 1,   false, f_exp },
  { "LN",          1, 1,   false, f_ln },
  { "LOG10",       1, 1,   false, f_log10 },
  { "PI",          0, 0,   false, f_pi },
  { "LEN",         1, 1,   false, f_len },
  { "LEFT",        1, 2,   false, f_left },
  { "RIGHT",       1, 2,   false, f_right },
  { "MID",         3, 3,   false, f_mid },
  { "UPPER",       1, 1,   false, f_upper },
  { "LOWER",       1, 1,   false, f_lower },
  { "TRIM",        1, 1,   false, f_trim },
  { "CONCAT",      1, 255, false, f_concat },
  { "CONCATENATE", 1, 255, false, f_concat },
  { "TEXT",        2, 2,   false, f_text },
  { "NOW",         0, 0,   true,  f_now },
  { "TODAY",       0, 0,   true,  f_today },
  { "SUMIF",       2, 3,   false, f_sumif },
  { "COUNTIF",     2, 2,   false, f_countif },
  { "VLOOKUP",     3, 4,   false, f_vlookup },
  { "PMT",         3, 5,   false, f_pmt },
  { "IFERROR",     2, 2,   false, f_iferror },
  { "ISBLANK",     1, 1,   false, f_isblank },
  { "ISNUMBER",    1, 1,   false, f_isnumber },
  { "ISTEXT",      1, 1,   false, f_istext },
  { "ISERROR",     1, 1,   false, f_iserror },
};

static const int g_nfn = sizeof(g_fn) / sizeof(g_fn[0]);

/****************************************************************************
 * Private Functions: evaluation
 ****************************************************************************/

static void binary(struct ev_s *e, int op, struct sv_s *a, struct sv_s *b,
                   struct sv_s *out)
{
  int err = SE_NONE;
  double x;
  double y;

  if (op == T_CAT)
    {
      const char *s = to_str(e, a, &err);
      const char *t = s != NULL ? to_str(e, b, &err) : NULL;
      char *p;

      if (t == NULL)
        {
          v_err(out, err);
          return;
        }

      p = arena(e, strlen(s) + strlen(t));
      if (p == NULL)
        {
          v_err(out, SE_VALUE);
          return;
        }

      strcpy(p, s);
      strcat(p, t);
      v_str(out, p);
      return;
    }

  if (op >= T_EQ && op <= T_GE)
    {
      int k;

      deref(e, a);
      deref(e, b);
      if (a->t == SV_ERR || b->t == SV_ERR)
        {
          v_err(out, a->t == SV_ERR ? a->e : b->e);
          return;
        }

      k = cmp_values(a, b);
      v_bool(out, op == T_EQ ? k == 0 : op == T_NE ? k != 0 :
                  op == T_LT ? k < 0 : op == T_GT ? k > 0 :
                  op == T_LE ? k <= 0 : k >= 0);
      return;
    }

  if (!to_num(e, a, &x, &err) || !to_num(e, b, &y, &err))
    {
      v_err(out, err);
      return;
    }

  switch (op)
    {
      case T_ADD:
        v_num(out, x + y);
        break;

      case T_SUB:
        v_num(out, x - y);
        break;

      case T_MUL:
        v_num(out, x * y);
        break;

      case T_DIV:
        y == 0 ? v_err(out, SE_DIV0) : v_num(out, x / y);
        break;

      default:
        if (x == 0 && y < 0)
          {
            v_err(out, SE_DIV0);
          }
        else if (x < 0 && y != floor(y))
          {
            v_err(out, SE_NUM);
          }
        else
          {
            v_num(out, pow(x, y));
          }
        break;
    }
}

/* A formula's value, as if in cell (r, c) */

static void run(struct sheet_s *sh, const struct sh_formula_s *f, int r,
                int c, struct sv_s *out)
{
  struct sv_s *st = sh->stack;
  struct ev_s e;
  int sp = 0;
  int i;

  if (st == NULL)
    {
      st = sh->stack = malloc(EV_STACK * sizeof(struct sv_s));
      if (st == NULL)
        {
          v_err(out, SE_VALUE);
          return;
        }
    }

  e.sh = sh;
  e.f = f;
  e.r = r;
  e.c = c;
  sh->arenan = 0;
  for (i = 0; i < f->ntok; i++)
    {
      const struct sh_tok_s *t = &f->tok[i];
      int err = SE_NONE;
      struct sv_s v;
      double x;

      if (sp >= EV_STACK - 1)
        {
          v_err(out, SE_VALUE);
          return;
        }

      switch (t->op)
        {
          case T_NUM:
            v_num(&st[sp++], t->u.num);
            break;

          case T_STR:
            {
              char *p = arena(&e, t->u.s.len);

              if (p != NULL)
                {
                  memcpy(p, POOL(f) + t->u.s.off, t->u.s.len);
                }

              v_str(&st[sp++], p);
            }
            break;

          case T_BOOL:
            v_bool(&st[sp++], t->u.b != 0);
            break;

          case T_ERR:
            v_err(&st[sp++], t->fl);
            break;

          case T_REF:
          case T_RANGE:
            st[sp].t = SV_RANGE;
            st[sp].e = SE_NONE;
            st[sp].r0 = t->u.rf.r0;
            st[sp].c0 = t->u.rf.c0;
            st[sp].r1 = t->u.rf.r1;
            st[sp].c1 = t->u.rf.c1;
            sp++;
            break;

          case T_NAME:
          case T_BADFUNC:
            sp -= t->op == T_BADFUNC ? t->argc : 0;
            v_err(&st[sp++], SE_NAME);
            break;

          case T_MISSING:
            st[sp].t = SV_EMPTY;
            st[sp].e = SE_NONE;
            st[sp].n = 0;
            st[sp].s = NULL;
            sp++;
            break;

          case T_NEG:
          case T_PLUS:
          case T_PCT:
            if (sp < 1)
              {
                v_err(out, SE_VALUE);
                return;
              }

            if (!to_num(&e, &st[sp - 1], &x, &err))
              {
                v_err(&st[sp - 1], err);
              }
            else
              {
                v_num(&st[sp - 1], t->op == T_NEG ? -x :
                                   t->op == T_PCT ? x / 100 : x);
              }
            break;

          case T_PAREN:
            break;

          case T_FUNC:
            if (sp < t->argc)
              {
                v_err(out, SE_VALUE);
                return;
              }

            sp -= t->argc;
            v_err(&v, SE_VALUE);
            g_fn[t->fn].fn(&e, &st[sp], t->argc, &v);
            st[sp++] = v;
            break;

          default:
            if (sp < 2)
              {
                v_err(out, SE_VALUE);
                return;
              }

            binary(&e, t->op, &st[sp - 2], &st[sp - 1], &v);
            sp--;
            st[sp - 1] = v;
            break;
        }
    }

  if (sp != 1)
    {
      v_err(out, SE_VALUE);
      return;
    }

  *out = st[0];
  deref(&e, out);
  if (out->t == SV_EMPTY)
    {
      v_num(out, 0);
    }
}

static void store(struct sh_cell_s *cell, const struct sv_s *v)
{
  free(cell->str);
  cell->str = NULL;
  cell->vt = v->t;
  cell->err = v->t == SV_ERR ? v->e : SE_NONE;
  cell->num = v->t == SV_NUM || v->t == SV_BOOL ? v->n : 0;
  if (v->t == SV_STR)
    {
      cell->str = strdup(v->s);
      if (cell->str == NULL)
        {
          cell->vt = SV_ERR;
          cell->err = SE_VALUE;
        }
    }
}

static void evaluate(struct sheet_s *sh, struct sh_cell_s *cell)
{
  struct sv_s v;

  run(sh, cell->f, SH_ROW(cell->key), SH_COL(cell->key), &v);
  store(cell, &v);
}

/****************************************************************************
 * Private Functions: recalculation
 ****************************************************************************/

struct rc_s
{
  struct sh_keys_s dirty;                /* Marked formulas */
  struct sh_keys_s queue;
};

static void rc_mark(struct sheet_s *sh, uint32_t reader, void *arg)
{
  struct rc_s *rc = arg;
  struct sh_cell_s *c = find(sh, reader);

  if (c != NULL && c->kind == SK_FORMULA && c->mark == 0)
    {
      c->mark = 1;
      c->indeg = 0;
      sheet_keys_add(&rc->dirty, reader);
      sheet_keys_add(&rc->queue, reader);
    }
}

static void rc_count(struct sheet_s *sh, uint32_t reader, void *arg)
{
  struct sh_cell_s *c = find(sh, reader);

  if (c != NULL && c->mark == 1)
    {
      c->indeg++;
    }
}

static void rc_done(struct sheet_s *sh, uint32_t reader, void *arg)
{
  struct rc_s *rc = arg;
  struct sh_cell_s *c = find(sh, reader);

  if (c != NULL && c->mark == 1 && --c->indeg == 0)
    {
      sheet_keys_add(&rc->queue, reader);
    }
}

/* Evaluate the marked formulas (rc->dirty) in topological order */

static void rc_run(struct sheet_s *sh, struct rc_s *rc)
{
  struct sv_s circ;
  int i;

  for (i = 0; i < rc->dirty.n; i++)
    {
      readers(sh, rc->dirty.k[i], rc_count, rc);
    }

  rc->queue.n = 0;
  for (i = 0; i < rc->dirty.n; i++)
    {
      struct sh_cell_s *c = find(sh, rc->dirty.k[i]);

      if (c->indeg == 0)
        {
          sheet_keys_add(&rc->queue, c->key);
        }
    }

  for (i = 0; i < rc->queue.n; i++)
    {
      struct sh_cell_s *c = find(sh, rc->queue.k[i]);

      evaluate(sh, c);
      c->mark = 2;
      readers(sh, c->key, rc_done, rc);
    }

  /* What is left is in a cycle or reads one */

  v_err(&circ, SE_CIRC);
  for (i = 0; i < rc->dirty.n; i++)
    {
      struct sh_cell_s *c = find(sh, rc->dirty.k[i]);

      if (c->mark == 1)
        {
          store(c, &circ);
        }

      c->mark = 0;
      if (!sh->alltouched)
        {
          sheet_keys_add(&sh->touched, c->key);
        }
    }

  sheet_keys_free(&rc->dirty);
  sheet_keys_free(&rc->queue);
}

void sheet_recalc(struct sheet_s *sh)
{
  struct sh_cell_s *c;
  struct rc_s rc;
  int i;

  if (sh->changed.n == 0 || sh->batch)
    {
      return;
    }

  memset(&rc, 0, sizeof(rc));

  /* The changed cells (formulas among them are evaluated too) and the
   * volatile ones, then all they reach
   */

  for (i = 0; i < sh->changed.n; i++)
    {
      uint32_t k = sh->changed.k[i];

      if (!sh->alltouched)
        {
          sheet_keys_add(&sh->touched, k);
        }

      c = find(sh, k);
      if (c != NULL && c->kind == SK_FORMULA && c->mark == 0)
        {
          c->mark = 1;
          c->indeg = 0;
          sheet_keys_add(&rc.dirty, k);
        }

      sheet_keys_add(&rc.queue, k);
    }

  for (i = 0; i < sh->vol.n; i++)
    {
      rc_mark(sh, sh->vol.k[i], &rc);
    }

  for (i = 0; i < rc.queue.n; i++)
    {
      readers(sh, rc.queue.k[i], rc_mark, &rc);
    }

  sh->changed.n = 0;
  if (sh->touched.n > 4096)
    {
      sh->alltouched = true;
      sh->touched.n = 0;
    }

  rc_run(sh, &rc);
}

void sheet_recalc_all(struct sheet_s *sh)
{
  struct rc_s rc;
  uint32_t i;

  memset(&rc, 0, sizeof(rc));
  sh->changed.n = 0;
  sh->alltouched = true;
  sh->touched.n = 0;
  for (i = 0; i < sh->cap; i++)
    {
      struct sh_cell_s *c = sh->tab[i];

      if (c != NULL && c->kind == SK_FORMULA)
        {
          c->mark = 1;
          c->indeg = 0;
          sheet_keys_add(&rc.dirty, c->key);
        }
    }

  rc_run(sh, &rc);
}

/****************************************************************************
 * Private Functions: input
 ****************************************************************************/

/* A typed number: [-+][$]digits with commas in threes[.digits][e..][%];
 * *fmt what it says about its format (currency, thousands, percent), -1
 * nothing
 */

static bool parse_typed(const char *s, double *v, int *nf, int *dec)
{
  const char *p = s;
  char digits[64];
  bool neg = false;
  bool cur = false;
  bool grouped = false;
  bool pct = false;
  int n = 0;
  int grp = -1;
  int k;
  double x;

  *nf = -1;
  *dec = 0;
  while (*p == ' ')
    {
      p++;
    }

  if (*p == '-' || *p == '+')
    {
      neg = *p++ == '-';
    }

  if (*p == '$')
    {
      cur = true;
      p++;
    }

  /* The integer part, commas in threes */

  while ((isdigit((unsigned char)*p) || *p == ',') && n < 40)
    {
      if (*p == ',')
        {
          if (n == 0 || (grp >= 0 && grp != 3) ||
              (grp < 0 && n > 3) || !isdigit((unsigned char)p[1]))
            {
              return false;
            }

          grouped = true;
          grp = 0;
          p++;
          continue;
        }

      digits[n++] = *p++;
      grp += grp >= 0;
    }

  if (grouped && grp != 3)
    {
      return false;
    }

  if (*p == '.')
    {
      digits[n++] = *p++;
      while (isdigit((unsigned char)*p) && n < 60)
        {
          digits[n++] = *p++;
          (*dec)++;
        }
    }

  if (n == 0 || (n == 1 && digits[0] == '.'))
    {
      return false;
    }

  if ((*p == 'e' || *p == 'E') && !cur)
    {
      digits[n++] = *p++;
      if (*p == '-' || *p == '+')
        {
          digits[n++] = *p++;
        }

      if (!isdigit((unsigned char)*p))
        {
          return false;
        }

      while (isdigit((unsigned char)*p) && n < 63)
        {
          digits[n++] = *p++;
        }
    }

  digits[n] = '\0';
  if (*p == '%')
    {
      pct = true;
      p++;
    }

  while (*p == ' ')
    {
      p++;
    }

  if (*p != '\0')
    {
      return false;
    }

  k = num_parse(digits, &x);
  if (k != n)
    {
      return false;
    }

  x = pct ? x / 100 : x;
  *v = neg ? -x : x;
  *nf = pct ? SF_PERCENT : cur ? SF_CURRENCY : grouped ? SF_THOUSANDS : -1;
  if (cur && *dec == 0)
    {
      *dec = 2;
    }

  return true;
}

static bool text_number(const char *s, double *v)
{
  int nf;
  int dec;

  return s != NULL && parse_typed(s, v, &nf, &dec);
}

/* yyyy-mm-dd */

static bool parse_date(const char *s, double *v)
{
  int y = 0;
  int m = 0;
  int d = 0;
  int n = 0;

  while (isdigit((unsigned char)*s) && n < 4)
    {
      y = y * 10 + (*s++ - '0');
      n++;
    }

  if (n != 4 || *s++ != '-' || !isdigit((unsigned char)*s))
    {
      return false;
    }

  m = *s++ - '0';
  if (isdigit((unsigned char)*s))
    {
      m = m * 10 + (*s++ - '0');
    }

  if (*s++ != '-' || !isdigit((unsigned char)*s))
    {
      return false;
    }

  d = *s++ - '0';
  if (isdigit((unsigned char)*s))
    {
      d = d * 10 + (*s++ - '0');
    }

  if (*s != '\0' || m < 1 || m > 12 || d < 1 || d > 31 || y < 1900)
    {
      return false;
    }

  *v = sheet_serial(y, m, d);
  return true;
}

/* A cell's content goes (and what it reads) */

static void content_clear(struct sheet_s *sh, struct sh_cell_s *cell)
{
  if (cell->kind == SK_FORMULA)
    {
      deps(sh, cell->key, cell->f, false);
      sheet_formula_free(cell->f);
      cell->f = NULL;
    }

  free(cell->str);
  cell->str = NULL;
  cell->kind = SK_NONE;
  cell->vt = SV_EMPTY;
  cell->err = SE_NONE;
  cell->num = 0;
}

/* After a change: recalculated from here; an empty cell with no format
 * goes
 */

static void changed(struct sheet_s *sh, struct sh_cell_s *cell,
                    uint32_t key)
{
  sheet_keys_add(&sh->changed, key);
  if (cell != NULL && cell->kind == SK_NONE && cell->fmt == 0)
    {
      table_take(sh, key);
      cell_release(sh, cell);
    }
}

void sheet_set_formula(struct sheet_s *sh, int r, int c,
                       struct sh_formula_s *f)
{
  struct sh_cell_s *cell = cell_make(sh, r, c);

  if (cell == NULL)
    {
      sheet_formula_free(f);
      return;
    }

  content_clear(sh, cell);
  if (f != NULL)
    {
      cell->kind = SK_FORMULA;
      cell->f = f;
      cell->vt = SV_NUM;
      deps(sh, cell->key, f, true);
    }

  changed(sh, cell, SH_KEY(r, c));
}

int sheet_set_input(struct sheet_s *sh, int r, int c, const char *s,
                    bool strict)
{
  struct sh_cell_s *cell;
  struct sh_formula_s *f = NULL;
  double v;
  int nf;
  int dec;

  if (r < 0 || c < 0 || r >= SH_ROWS || c >= SH_COLS)
    {
      return -EINVAL;
    }

  if (s[0] == '=' || s[0] == '+' || s[0] == '@')
    {
      if (s[0] != '=' && parse_typed(s, &v, &nf, &dec))
        {
          goto number;
        }

      if (s[1] != '\0')
        {
          f = parse(s[0] == '=' ? s + 1 : s);
        }

      if (f == NULL && s[0] == '=' && strict && s[1] != '\0')
        {
          return -EINVAL;
        }

      if (f != NULL)
        {
          struct sh_cell_s *old = sheet_get(sh, r, c);

          /* NOW() and TODAY() show as dates */

          if (f->vol && f->ntok == 1 && (old == NULL ||
                                         SF_NF(old->fmt) == SF_GENERAL))
            {
              sheet_set_fmt(sh, r, c, SF_SET_NF(old != NULL ? old->fmt : 0,
                            g_fn[f->tok[0].fn].fn == f_now ? SF_DATETIME :
                                                             SF_DATE));
            }

          sheet_set_formula(sh, r, c, f);
          return 0;
        }
    }

  cell = cell_make(sh, r, c);
  if (cell == NULL)
    {
      return -ENOMEM;
    }

  content_clear(sh, cell);
  if (s[0] == '\0')
    {
      changed(sh, cell, SH_KEY(r, c));
      return 0;
    }

  if (s[0] == '\'')
    {
      goto text;
    }

  if (parse_typed(s, &v, &nf, &dec))
    {
number:
      cell = cell_make(sh, r, c);
      if (cell == NULL)
        {
          return -ENOMEM;
        }

      content_clear(sh, cell);
      cell->kind = SK_NUM;
      cell->vt = SV_NUM;
      cell->num = v;
      if (nf >= 0 && SF_NF(cell->fmt) == SF_GENERAL)
        {
          cell->fmt = SF_SET_DEC(SF_SET_NF(cell->fmt, nf), dec > 10 ? 10 :
                                 dec);
        }

      changed(sh, cell, SH_KEY(r, c));
      return 0;
    }

  if (parse_date(s, &v))
    {
      cell->kind = SK_NUM;
      cell->vt = SV_NUM;
      cell->num = v;
      if (SF_NF(cell->fmt) == SF_GENERAL)
        {
          cell->fmt = SF_SET_NF(cell->fmt, SF_DATE);
        }

      changed(sh, cell, SH_KEY(r, c));
      return 0;
    }

  if (strcasecmp(s, "TRUE") == 0 || strcasecmp(s, "FALSE") == 0)
    {
      cell->kind = SK_BOOL;
      cell->vt = SV_BOOL;
      cell->num = toupper((unsigned char)s[0]) == 'T';
      changed(sh, cell, SH_KEY(r, c));
      return 0;
    }

text:
  cell->kind = SK_TEXT;
  cell->vt = SV_STR;
  cell->str = strdup(s[0] == '\'' ? s + 1 : s);
  if (cell->str == NULL)
    {
      cell->kind = SK_NONE;
      cell->vt = SV_EMPTY;
    }

  changed(sh, cell, SH_KEY(r, c));
  return 0;
}

static bool looks_other(const char *s)
{
  double v;
  int nf;
  int dec;

  return s[0] == '=' || s[0] == '+' || s[0] == '@' || s[0] == '\'' ||
         parse_typed(s, &v, &nf, &dec) || parse_date(s, &v) ||
         strcasecmp(s, "TRUE") == 0 || strcasecmp(s, "FALSE") == 0;
}

void sheet_input(struct sheet_s *sh, struct sh_cell_s *cell, char *buf,
                 size_t size)
{
  buf[0] = '\0';
  if (cell == NULL)
    {
      return;
    }

  switch (cell->kind)
    {
      case SK_NUM:
        if (SF_NF(cell->fmt) == SF_PERCENT)
          {
            num_plain(cell->num * 100, 15, buf, size);
            strlcat(buf, "%", size);
          }
        else if (SF_NF(cell->fmt) == SF_DATE && cell->num == floor(cell->num))
          {
            int y;
            int m;
            int d;
            int hh;
            int mm;
            int ss;

            sheet_date(cell->num, &y, &m, &d, &hh, &mm, &ss);
            if (size >= 16)
              {
                char *p = num_int(buf, y, 4);

                *p++ = '-';
                p = num_int(p, m, 2);
                *p++ = '-';
                num_int(p, d, 2);
              }
          }
        else
          {
            num_plain(cell->num, 15, buf, size);
          }
        break;

      case SK_TEXT:
        strlcpy(buf, looks_other(cell->str) ? "'" : "", size);
        strlcat(buf, cell->str, size);
        break;

      case SK_BOOL:
        strlcpy(buf, cell->num != 0 ? "TRUE" : "FALSE", size);
        break;

      case SK_FORMULA:
        buf[0] = '=';
        sheet_formula_text(cell->f, buf + 1, size - 1);
        break;

      default:
        break;
    }
}

void sheet_set_fmt(struct sheet_s *sh, int r, int c, uint32_t fmt)
{
  struct sh_cell_s *cell = sheet_get(sh, r, c);

  if (cell == NULL)
    {
      if (fmt == 0)
        {
          return;
        }

      cell = cell_make(sh, r, c);
      if (cell == NULL)
        {
          return;
        }
    }

  cell->fmt = fmt;
  if (cell->kind == SK_NONE && fmt == 0)
    {
      table_take(sh, cell->key);
      cell_release(sh, cell);
    }
}

void sheet_clear_rect(struct sheet_s *sh, const struct sh_rect_s *r,
                      bool formats)
{
  struct collect_s cl;
  int i;

  collect(sh, r, &cl, true);
  for (i = 0; i < cl.n; i++)
    {
      struct sh_cell_s *c = cl.v[i];

      content_clear(sh, c);
      if (formats)
        {
          c->fmt = 0;
        }

      changed(sh, c, c->key);
    }

  free(cl.v);
}

uint32_t sheet_fmt(struct sheet_s *sh, int r, int c)
{
  struct sh_cell_s *cell = sheet_get(sh, r, c);

  return cell != NULL ? cell->fmt : 0;
}

/****************************************************************************
 * Private Functions: display
 ****************************************************************************/

bool sheet_is_number(struct sh_cell_s *cell)
{
  return cell != NULL && cell->kind != SK_NONE && cell->vt == SV_NUM;
}

int sheet_display(struct sheet_s *sh, struct sh_cell_s *cell, int prec,
                  char *buf, size_t size)
{
  int align = SF_LEFT;
  uint32_t fmt;

  buf[0] = '\0';
  if (cell == NULL || size < 40)
    {
      return SF_LEFT;
    }

  fmt = cell->fmt;
  if (cell->kind == SK_NONE)
    {
      return SF_LEFT;
    }

  switch (cell->vt)
    {
      case SV_NUM:
        {
          double v = cell->num;
          int dec = SF_DEC(fmt);

          align = SF_RIGHT;
          switch (SF_NF(fmt))
            {
              case SF_FIXED:
                num_fixed(v, dec, false, buf, size);
                break;

              case SF_THOUSANDS:
                num_fixed(v, dec, true, buf, size);
                break;

              case SF_CURRENCY:
                {
                  /* -$1,234.50: the number after room for the signs */

                  char *n = buf + 2;
                  bool neg;

                  num_fixed(v < 0 ? -v : v, dec, true, n, size - 2);
                  neg = v < 0 && strspn(n, "0.,") != strlen(n);
                  n[-1] = '$';
                  n[-2] = '-';
                  if (!neg)
                    {
                      memmove(buf, buf + 1, strlen(buf + 1) + 1);
                    }
                }
                break;

              case SF_PERCENT:
                num_fixed(v * 100, dec, false, buf, size - 1);
                strcat(buf, "%");
                break;

              case SF_DATE:
              case SF_DATETIME:
                {
                  int y;
                  int m;
                  int d;
                  int hh;
                  int mm;
                  int ss;

                  char *p;

                  sheet_date(v, &y, &m, &d, &hh, &mm, &ss);
                  p = num_int(buf, y, 4);
                  *p++ = '-';
                  p = num_int(p, m, 2);
                  *p++ = '-';
                  p = num_int(p, d, 2);
                  if (SF_NF(fmt) == SF_DATETIME)
                    {
                      *p++ = ' ';
                      p = num_int(p, hh, 2);
                      *p++ = ':';
                      num_int(p, mm, 2);
                    }
                }
                break;

              default:
                num_plain(v, prec, buf, size);
                break;
            }
        }
        break;

      case SV_STR:
        strlcpy(buf, cell->str != NULL ? cell->str : "", size);
        align = SF_LEFT;
        break;

      case SV_BOOL:
        strlcpy(buf, cell->num != 0 ? "TRUE" : "FALSE", size);
        align = SF_CENTRE;
        break;

      case SV_ERR:
        strlcpy(buf, sheet_errname(cell->err), size);
        align = SF_CENTRE;
        break;

      default:
        break;
    }

  return SF_ALIGN(fmt) != SF_AUTO ? SF_ALIGN(fmt) : align;
}

/****************************************************************************
 * Public Functions: sheets, cells
 ****************************************************************************/

struct sheet_s *sheet_new(void)
{
  struct sheet_s *sh = calloc(1, sizeof(*sh));

  if (sh != NULL)
    {
      sh->maxrow = -1;
      sh->maxcol = -1;
      sh->now = default_now;
    }

  return sh;
}

void sheet_clear_all(struct sheet_s *sh)
{
  struct sh_slab_s *s;
  uint32_t i;

  for (i = 0; i < sh->cap; i++)
    {
      if (sh->tab[i] != NULL)
        {
          free(sh->tab[i]->str);
          sheet_formula_free(sh->tab[i]->f);
        }
    }

  while ((s = sh->slabs) != NULL)
    {
      sh->slabs = s->next;
      free(s);
    }

  free(sh->tab);
  sh->tab = NULL;
  sh->cap = 0;
  sh->n = 0;
  sh->free = NULL;
  deps_clear(sh);
  sheet_keys_free(&sh->changed);
  sheet_keys_free(&sh->touched);
  sh->alltouched = true;
  sh->maxrow = -1;
  sh->maxcol = -1;
  sh->extstale = false;
  memset(sh->colw, 0, sizeof(sh->colw));
}

void sheet_free(struct sheet_s *sh)
{
  if (sh != NULL)
    {
      sheet_clear_all(sh);
      free(sh->arena);
      free(sh->stack);
      free(sh);
    }
}

struct sh_cell_s *sheet_get(struct sheet_s *sh, int r, int c)
{
  if (r < 0 || c < 0 || r >= SH_ROWS || c >= SH_COLS)
    {
      return NULL;
    }

  return find(sh, SH_KEY(r, c));
}

void sheet_extent(struct sheet_s *sh, int *maxr, int *maxc)
{
  uint32_t i;

  if (sh->extstale)
    {
      sh->maxrow = -1;
      sh->maxcol = -1;
      for (i = 0; i < sh->cap; i++)
        {
          struct sh_cell_s *c = sh->tab[i];

          if (c != NULL)
            {
              sh->maxrow = MAX(sh->maxrow, SH_ROW(c->key));
              sh->maxcol = MAX(sh->maxcol, SH_COL(c->key));
            }
        }

      sh->extstale = false;
    }

  *maxr = sh->maxrow;
  *maxc = sh->maxcol;
}

void sheet_each(struct sheet_s *sh, bool (*fn)(struct sh_cell_s *cell,
                                              void *arg), void *arg)
{
  uint32_t i;

  for (i = 0; i < sh->cap; i++)
    {
      if (sh->tab[i] != NULL && !fn(sh->tab[i], arg))
        {
          return;
        }
    }
}

int sheet_eval_text(struct sheet_s *sh, int r, int c, const char *s,
                    struct sh_cell_s *out)
{
  struct sh_formula_s *f = parse(s[0] == '=' ? s + 1 : s);
  struct sv_s v;

  if (f == NULL)
    {
      return -EINVAL;
    }

  run(sh, f, r, c, &v);
  memset(out, 0, sizeof(*out));
  out->kind = SK_FORMULA;
  store(out, &v);
  sheet_formula_free(f);
  return 0;
}

/****************************************************************************
 * Public Functions: snapshots
 ****************************************************************************/

void sheet_snap_cell(struct sheet_s *sh, struct sh_cell_s *cell,
                     struct sh_snap_s *s)
{
  char *buf = malloc(SH_INPUT_MAX);

  s->key = cell->key;
  s->fmt = cell->fmt;
  s->input = NULL;
  if (buf != NULL && cell->kind != SK_NONE)
    {
      sheet_input(sh, cell, buf, SH_INPUT_MAX);
      s->input = strdup(buf);
    }

  free(buf);
}

void sheet_snap(struct sheet_s *sh, uint32_t key, struct sh_snap_s *s)
{
  struct sh_cell_s *cell = find(sh, key);

  if (cell == NULL)
    {
      s->key = key;
      s->fmt = 0;
      s->input = NULL;
      return;
    }

  sheet_snap_cell(sh, cell, s);
}

void sheet_unsnap(struct sheet_s *sh, const struct sh_snap_s *s)
{
  int r = SH_ROW(s->key);
  int c = SH_COL(s->key);
  struct sh_cell_s *cell;

  /* The format first: typing may give one ("15%"), the snapshot's wins */

  sheet_set_fmt(sh, r, c, s->fmt);
  sheet_set_input(sh, r, c, s->input != NULL ? s->input : "", false);
  cell = sheet_get(sh, r, c);
  if (cell != NULL && cell->fmt != s->fmt)
    {
      sheet_set_fmt(sh, r, c, s->fmt);
    }
}

void sheet_snap_free(struct sh_snap_s *s)
{
  free(s->input);
  s->input = NULL;
}

/****************************************************************************
 * Public Functions: rows and columns, moves, sorting
 ****************************************************************************/

/* Every cell out of the table, into an array (the table empties) */

static struct sh_cell_s **take_all(struct sheet_s *sh, uint32_t *n)
{
  struct sh_cell_s **v = malloc((sh->n + 1) * sizeof(*v));
  uint32_t i;
  uint32_t k = 0;

  if (v == NULL)
    {
      return NULL;
    }

  for (i = 0; i < sh->cap; i++)
    {
      if (sh->tab[i] != NULL)
        {
          v[k++] = sh->tab[i];
          sh->tab[i] = NULL;
        }
    }

  *n = k;
  sh->n = 0;
  sh->maxrow = -1;
  sh->maxcol = -1;
  sh->extstale = false;
  return v;
}

static void restructure(struct sheet_s *sh, bool rows, int at, int n,
                        sheet_snap_t snap, void *arg)
{
  struct sh_cell_s **v;
  uint32_t cnt;
  uint32_t i;
  int lim = rows ? SH_ROWS : SH_COLS;

  /* Formulas whose references change, before they do */

  if (snap != NULL)
    {
      for (i = 0; i < sh->cap; i++)
        {
          struct sh_cell_s *c = sh->tab[i];
          int p;

          if (c == NULL)
            {
              continue;
            }

          p = rows ? SH_ROW(c->key) : SH_COL(c->key);
          if (n < 0 && p >= at && p < at - n)
            {
              snap(arg, c);              /* Deleted */
            }
          else if (c->kind == SK_FORMULA)
            {
              struct sh_formula_s *f = sheet_formula_dup(c->f, 0, 0);

              if (f != NULL && adjust_formula(f, rows, at, n))
                {
                  snap(arg, c);
                }

              sheet_formula_free(f);
            }
        }
    }

  v = take_all(sh, &cnt);
  if (v == NULL)
    {
      return;
    }

  for (i = 0; i < cnt; i++)
    {
      struct sh_cell_s *c = v[i];
      int r = SH_ROW(c->key);
      int col = SH_COL(c->key);
      int *p = rows ? &r : &col;

      if (c->kind == SK_FORMULA)
        {
          adjust_formula(c->f, rows, at, n);
        }

      if (n > 0 && *p >= at)
        {
          *p += n;
        }
      else if (n < 0 && *p >= at - n)
        {
          *p += n;
        }
      else if (n < 0 && *p >= at)
        {
          *p = lim;                      /* Deleted */
        }

      if (*p >= lim)
        {
          cell_release(sh, c);
          continue;
        }

      c->key = SH_KEY(r, col);
      table_put(sh, c);
    }

  free(v);

  /* The columns' widths move with them */

  if (!rows && n > 0)
    {
      memmove(&sh->colw[at + n], &sh->colw[at],
              (SH_COLS - at - n) * sizeof(sh->colw[0]));
      memset(&sh->colw[at], 0, n * sizeof(sh->colw[0]));
    }
  else if (!rows)
    {
      memmove(&sh->colw[at], &sh->colw[at - n],
              (SH_COLS - at + n) * sizeof(sh->colw[0]));
      memset(&sh->colw[SH_COLS + n], 0, -n * sizeof(sh->colw[0]));
    }

  deps_rebuild(sh);
  sheet_recalc_all(sh);
}

void sheet_insert(struct sheet_s *sh, bool rows, int at, int n)
{
  int lim = rows ? SH_ROWS : SH_COLS;

  if (n > 0 && at >= 0 && at < lim)
    {
      restructure(sh, rows, at, MIN(n, lim - at), NULL, NULL);
    }
}

void sheet_delete(struct sheet_s *sh, bool rows, int at, int n,
                  sheet_snap_t snap, void *arg)
{
  int lim = rows ? SH_ROWS : SH_COLS;

  if (n > 0 && at >= 0 && at < lim)
    {
      restructure(sh, rows, at, -MIN(n, lim - at), snap, arg);
    }
}

void sheet_move(struct sheet_s *sh, const struct sh_rect_s *src, int dr,
                int dc, sheet_snap_t snap, void *arg)
{
  struct collect_s cl;
  struct sh_rect_s dst;
  uint32_t i;
  int j;

  if (dr == 0 && dc == 0)
    {
      return;
    }

  /* Formulas elsewhere that read the cells moved */

  for (i = 0; i < sh->cap; i++)
    {
      struct sh_cell_s *c = sh->tab[i];

      if (c != NULL && c->kind == SK_FORMULA)
        {
          int r = SH_ROW(c->key);
          int col = SH_COL(c->key);
          bool inside = r >= src->r0 && r <= src->r1 && col >= src->c0 &&
                        col <= src->c1;
          struct sh_formula_s *f = sheet_formula_dup(c->f, 0, 0);

          if (f != NULL && move_formula(f, src, dr, dc))
            {
              if (!inside && snap != NULL)
                {
                  snap(arg, c);
                }

              sheet_formula_free(c->f);
              c->f = f;
            }
          else
            {
              sheet_formula_free(f);
            }
        }
    }

  /* The cells: out of the table, what is where they go cleared, in */

  collect(sh, src, &cl, true);
  for (j = 0; j < cl.n; j++)
    {
      table_take(sh, cl.v[j]->key);
      sheet_keys_add(&sh->changed, cl.v[j]->key);
    }

  dst.r0 = MAX(0, src->r0 + dr);
  dst.c0 = MAX(0, src->c0 + dc);
  dst.r1 = MIN(SH_ROWS - 1, src->r1 + dr);
  dst.c1 = MIN(SH_COLS - 1, src->c1 + dc);
  {
    struct collect_s old;

    collect(sh, &dst, &old, true);
    for (j = 0; j < old.n; j++)
      {
        table_take(sh, old.v[j]->key);
        cell_release(sh, old.v[j]);
      }

    free(old.v);
  }

  for (j = 0; j < cl.n; j++)
    {
      struct sh_cell_s *c = cl.v[j];
      int r = SH_ROW(c->key) + dr;
      int col = SH_COL(c->key) + dc;

      if (r < 0 || col < 0 || r >= SH_ROWS || col >= SH_COLS)
        {
          cell_release(sh, c);
          continue;
        }

      c->key = SH_KEY(r, col);
      table_put(sh, c);
    }

  free(cl.v);
  deps_rebuild(sh);
  sheet_recalc_all(sh);
}

/* Sorting: rows compared by one column's values (empty ones last) */

struct sortrow_s
{
  int row;
  struct sv_s v;
};

static bool g_desc;

static int row_cmp(const void *a, const void *b)
{
  const struct sortrow_s *x = a;
  const struct sortrow_s *y = b;
  int k;

  if ((x->v.t == SV_EMPTY) != (y->v.t == SV_EMPTY))
    {
      return x->v.t == SV_EMPTY ? 1 : -1;
    }

  k = x->v.t == SV_EMPTY ? 0 : cmp_values(&x->v, &y->v);
  k = g_desc ? -k : k;
  return k != 0 ? k : x->row - y->row;
}

void sheet_sort(struct sheet_s *sh, const struct sh_rect_s *r, int col,
                bool desc)
{
  int nrows = r->r1 - r->r0 + 1;
  struct sortrow_s *rows;
  int *dest;
  struct collect_s cl;
  int i;

  if (nrows < 2 || nrows > 100000)
    {
      return;
    }

  rows = malloc(nrows * sizeof(*rows));
  dest = malloc(nrows * sizeof(*dest));
  if (rows == NULL || dest == NULL)
    {
      free(rows);
      free(dest);
      return;
    }

  for (i = 0; i < nrows; i++)
    {
      rows[i].row = r->r0 + i;
      cell_value(find(sh, SH_KEY(r->r0 + i, col)), &rows[i].v);
    }

  g_desc = desc;
  qsort(rows, nrows, sizeof(*rows), row_cmp);
  for (i = 0; i < nrows; i++)
    {
      dest[rows[i].row - r->r0] = r->r0 + i;
    }

  /* Each cell to its row: formulas as copies */

  collect(sh, r, &cl, true);
  for (i = 0; i < cl.n; i++)
    {
      struct sh_cell_s *c = cl.v[i];

      if (c->kind == SK_FORMULA)
        {
          deps(sh, c->key, c->f, false);
        }

      table_take(sh, c->key);
      sheet_keys_add(&sh->changed, c->key);
    }

  for (i = 0; i < cl.n; i++)
    {
      struct sh_cell_s *c = cl.v[i];
      int from = SH_ROW(c->key);
      int to = dest[from - r->r0];

      if (c->kind == SK_FORMULA && to != from)
        {
          struct sh_formula_s *f = sheet_formula_dup(c->f, to - from, 0);

          if (f != NULL)
            {
              sheet_formula_free(c->f);
              c->f = f;
            }
        }

      c->key = SH_KEY(to, SH_COL(c->key));
      table_put(sh, c);
      sheet_keys_add(&sh->changed, c->key);
      if (c->kind == SK_FORMULA)
        {
          deps(sh, c->key, c->f, true);
        }
    }

  free(cl.v);
  free(rows);
  free(dest);
}
