/****************************************************************************
 * apps/graphics/fpcdesk/desk_sheet_io.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Spreadsheet's files (desk_sheet.h): CSV (values: a formula is saved
 * as its value; a .tsv file has tabs instead of commas) and the native
 * text format, which keeps formulas, formats and the columns' widths
 * (docs/desktop.md):
 *
 *   FPCSHEET 1
 *   width B 120
 *   A1<TAB>bold,centre<TAB>Item
 *   B2<TAB>currency:2<TAB>=SUM(B3:B9)
 *
 * a cell a line: where, its format ("-": none), its text as typed (\t,
 * \n and \\ escaped).  Rows and columns beyond the sheet's are left out.
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "desk_num.h"
#include "desk_sheet.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IO_BUF          1024

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Reading a file a byte at a time, through a buffer */

struct rd_s
{
  int fd;
  char buf[IO_BUF];
  int n;
  int pos;
};

/* Writing one, through a buffer */

struct wr_s
{
  int fd;
  char buf[IO_BUF];
  int n;
  int err;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_nfname[] =
{
  "general", "fixed", "thousands", "currency", "percent", "date",
  "datetime"
};

static const char * const g_alname[] =
{
  "auto", "left", "centre", "right"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int rd_get(struct rd_s *r)
{
  if (r->pos == r->n)
    {
      r->n = read(r->fd, r->buf, sizeof(r->buf));
      r->pos = 0;
      if (r->n <= 0)
        {
          r->n = 0;
          return -1;
        }
    }

  return (unsigned char)r->buf[r->pos++];
}

static int rd_peek(struct rd_s *r)
{
  int c = rd_get(r);

  if (c >= 0)
    {
      r->pos--;
    }

  return c;
}

static void wr_flush(struct wr_s *w)
{
  int off = 0;

  while (off < w->n && w->err == 0)
    {
      ssize_t k = write(w->fd, w->buf + off, w->n - off);

      if (k <= 0)
        {
          w->err = k < 0 ? -errno : -EIO;
          break;
        }

      off += k;
    }

  w->n = 0;
}

static void wr_put(struct wr_s *w, const char *s, int n)
{
  while (n > 0)
    {
      int k = n < IO_BUF - w->n ? n : IO_BUF - w->n;

      memcpy(w->buf + w->n, s, k);
      w->n += k;
      s += k;
      n -= k;
      if (w->n == IO_BUF)
        {
          wr_flush(w);
        }
    }
}

static void wr_str(struct wr_s *w, const char *s)
{
  wr_put(w, s, strlen(s));
}

static int wr_open(struct wr_s *w, const char *path)
{
  w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  w->n = 0;
  w->err = 0;
  return w->fd < 0 ? -errno : 0;
}

static int wr_close(struct wr_s *w)
{
  wr_flush(w);
  if (close(w->fd) < 0 && w->err == 0)
    {
      w->err = -errno;
    }

  return w->err;
}

/* A cell's value as a CSV field has it (a formula's value) */

static void value_text(struct sh_cell_s *c, char *buf, size_t size)
{
  buf[0] = '\0';
  if (c == NULL || c->kind == SK_NONE)
    {
      return;
    }

  switch (c->vt)
    {
      case SV_NUM:
        num_plain(c->num, 15, buf, size);
        break;

      case SV_STR:
        strlcpy(buf, c->str != NULL ? c->str : "", size);
        break;

      case SV_BOOL:
        strlcpy(buf, c->num != 0 ? "TRUE" : "FALSE", size);
        break;

      case SV_ERR:
        strlcpy(buf, sheet_errname(c->err), size);
        break;

      default:
        break;
    }
}

static void csv_field(struct wr_s *w, const char *s, char sep)
{
  int n = strlen(s);
  bool quote = n > 0 && (s[0] == ' ' || s[n - 1] == ' ');
  int i;

  for (i = 0; i < n && !quote; i++)
    {
      quote = s[i] == sep || s[i] == '"' || s[i] == '\n' || s[i] == '\r';
    }

  if (!quote)
    {
      wr_put(w, s, n);
      return;
    }

  wr_put(w, "\"", 1);
  for (i = 0; i < n; i++)
    {
      wr_put(w, &s[i], 1);
      if (s[i] == '"')
        {
          wr_put(w, "\"", 1);
        }
    }

  wr_put(w, "\"", 1);
}

static char csv_sep(const char *path)
{
  const char *ext = strrchr(path, '.');

  return ext != NULL && strcasecmp(ext, ".tsv") == 0 ? '\t' : ',';
}

/* All the cells, row by row */

static struct sh_cell_s **all_cells(struct sheet_s *sh, int *n)
{
  struct sh_rect_s r;
  int maxr;
  int maxc;

  sheet_extent(sh, &maxr, &maxc);
  *n = 0;
  if (maxr < 0)
    {
      return NULL;
    }

  r.r0 = 0;
  r.c0 = 0;
  r.r1 = maxr;
  r.c1 = maxc;
  return sheet_cells_in(sh, &r, n);
}

/* A word of a format and its number (-1: none), then a comma */

static void fmt_word(char *buf, size_t size, const char *w, int v)
{
  char n[16];

  strlcat(buf, w, size);
  if (v >= 0)
    {
      n[0] = ':';
      num_int(n + 1, v, 0);
      strlcat(buf, n, size);
    }

  strlcat(buf, ",", size);
}

static void fmt_text(uint32_t f, char *buf, size_t size)
{
  buf[0] = '\0';
  if (SF_NF(f) != SF_GENERAL && SF_NF(f) <= SF_NFMAX)
    {
      fmt_word(buf, size, g_nfname[SF_NF(f)],
               SF_NF(f) == SF_DATE || SF_NF(f) == SF_DATETIME ? -1 :
               SF_DEC(f));
    }

  if (f & SF_BOLD)
    {
      fmt_word(buf, size, "bold", -1);
    }

  if (SF_ALIGN(f) != SF_AUTO)
    {
      fmt_word(buf, size, g_alname[SF_ALIGN(f)], -1);
    }

  if (SF_FG(f) != 0)
    {
      fmt_word(buf, size, "colour", SF_FG(f));
    }

  if (SF_BG(f) != 0)
    {
      fmt_word(buf, size, "fill", SF_BG(f));
    }

  if (buf[0] == '\0')
    {
      strlcpy(buf, "-", size);
    }
  else
    {
      buf[strlen(buf) - 1] = '\0';
    }
}

static uint32_t fmt_parse(char *s)
{
  uint32_t f = 0;
  char *save = NULL;
  char *tok;
  int i;

  for (tok = strtok_r(s, ",", &save); tok != NULL;
       tok = strtok_r(NULL, ",", &save))
    {
      char *arg = strchr(tok, ':');
      int v = 0;

      if (arg != NULL)
        {
          *arg++ = '\0';
          v = atoi(arg);
        }

      for (i = 1; i <= SF_NFMAX; i++)
        {
          if (strcmp(tok, g_nfname[i]) == 0)
            {
              f = SF_SET_DEC(SF_SET_NF(f, i), v < 0 ? 0 : v > 10 ? 10 : v);
            }
        }

      for (i = 1; i < 4; i++)
        {
          if (strcmp(tok, g_alname[i]) == 0)
            {
              f = SF_SET_ALIGN(f, i);
            }
        }

      if (strcmp(tok, "bold") == 0)
        {
          f |= SF_BOLD;
        }
      else if (strcmp(tok, "colour") == 0 && v > 0 && v < SF_COLOURS)
        {
          f = SF_SET_FG(f, v);
        }
      else if (strcmp(tok, "fill") == 0 && v > 0 && v < SF_COLOURS)
        {
          f = SF_SET_BG(f, v);
        }
    }

  return f;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool sheet_is_native(const char *path)
{
  const char *ext = strrchr(path, '.');

  return ext != NULL && strcasecmp(ext, ".sheet") == 0;
}

int sheet_save_csv(struct sheet_s *sh, const char *path)
{
  char sep = csv_sep(path);
  struct sh_cell_s **v;
  struct wr_s *w;
  char *buf;
  int row = 0;
  int col = 0;
  int ret;
  int n;
  int i;

  w = malloc(sizeof(*w));
  buf = malloc(SH_INPUT_MAX);
  if (w == NULL || buf == NULL)
    {
      free(w);
      free(buf);
      return -ENOMEM;
    }

  ret = wr_open(w, path);
  if (ret < 0)
    {
      free(w);
      free(buf);
      return ret;
    }

  v = all_cells(sh, &n);
  for (i = 0; i < n; i++)
    {
      struct sh_cell_s *c = v[i];
      int r = SH_ROW(c->key);
      int k = SH_COL(c->key);

      if (c->kind == SK_NONE)
        {
          continue;
        }

      for (; row < r; row++)
        {
          wr_put(w, "\n", 1);
          col = 0;
        }

      for (; col < k; col++)
        {
          wr_put(w, &sep, 1);
        }

      value_text(c, buf, SH_INPUT_MAX);
      csv_field(w, buf, sep);
    }

  if (n > 0)
    {
      wr_put(w, "\n", 1);
    }

  free(v);
  free(buf);
  ret = wr_close(w);
  free(w);
  return ret;
}

int sheet_load_csv(struct sheet_s *sh, const char *path)
{
  char sep = csv_sep(path);
  struct rd_s *rd;
  char *field;
  int len = 0;
  int row = 0;
  int col = 0;
  bool quoted = false;
  bool any = false;
  int c;

  rd = malloc(sizeof(*rd));
  field = malloc(SH_INPUT_MAX);
  if (rd == NULL || field == NULL)
    {
      free(rd);
      free(field);
      return -ENOMEM;
    }

  rd->fd = open(path, O_RDONLY);
  rd->n = 0;
  rd->pos = 0;
  if (rd->fd < 0)
    {
      int ret = -errno;

      free(rd);
      free(field);
      return ret;
    }

  sheet_clear_all(sh);
  sh->batch = true;
  for (; ; )
    {
      c = rd_get(rd);
      if (quoted)
        {
          if (c < 0)
            {
              quoted = false;
            }
          else if (c == '"')
            {
              if (rd_peek(rd) == '"')
                {
                  rd_get(rd);
                }
              else
                {
                  quoted = false;
                  continue;
                }
            }

          if (c >= 0 && len < SH_INPUT_MAX - 1)
            {
              field[len++] = c;
            }

          if (c >= 0)
            {
              continue;
            }
        }

      if (c == '"' && len == 0)
        {
          quoted = true;
          any = true;
          continue;
        }

      if (c == sep || c == '\n' || c < 0)
        {
          field[len] = '\0';
          if (len > 0 && row < SH_ROWS && col < SH_COLS)
            {
              sheet_set_input(sh, row, col, field, false);
            }

          len = 0;
          any = false;
          if (c == sep)
            {
              col++;
            }
          else
            {
              row++;
              col = 0;
            }

          if (c < 0)
            {
              break;
            }

          continue;
        }

      if (c == '\r')
        {
          continue;
        }

      if (len < SH_INPUT_MAX - 1)
        {
          field[len++] = c;
        }
    }

  (void)any;
  close(rd->fd);
  free(rd);
  free(field);
  sh->batch = false;
  sheet_recalc_all(sh);
  return 0;
}

int sheet_save_native(struct sheet_s *sh, const char *path)
{
  struct sh_cell_s **v;
  struct wr_s *w;
  char *buf;
  char ref[12];
  char fmt[96];
  int ret;
  int n;
  int i;

  w = malloc(sizeof(*w));
  buf = malloc(SH_INPUT_MAX);
  if (w == NULL || buf == NULL)
    {
      free(w);
      free(buf);
      return -ENOMEM;
    }

  ret = wr_open(w, path);
  if (ret < 0)
    {
      free(w);
      free(buf);
      return ret;
    }

  wr_str(w, "FPCSHEET 1\n");
  for (i = 0; i < SH_COLS; i++)
    {
      if (sh->colw[i] != 0)
        {
          char col[4];

          sheet_colname(i, col);
          wr_str(w, "width ");
          wr_str(w, col);
          wr_put(w, " ", 1);
          {
            char *e = num_int(fmt, sh->colw[i], 0);

            *e++ = '\n';
            wr_put(w, fmt, e - fmt);
          }
        }
    }

  v = all_cells(sh, &n);
  for (i = 0; i < n; i++)
    {
      struct sh_cell_s *c = v[i];
      const char *p;

      sheet_refname(SH_ROW(c->key), SH_COL(c->key), ref);
      fmt_text(c->fmt, fmt, sizeof(fmt));
      sheet_input(sh, c, buf, SH_INPUT_MAX);
      wr_str(w, ref);
      wr_put(w, "\t", 1);
      wr_str(w, fmt);
      wr_put(w, "\t", 1);
      for (p = buf; *p != '\0'; p++)
        {
          if (*p == '\\' || *p == '\t' || *p == '\n' || *p == '\r')
            {
              wr_put(w, "\\", 1);
              wr_put(w, *p == '\t' ? "t" : *p == '\n' ? "n" :
                        *p == '\r' ? "r" : "\\", 1);
            }
          else
            {
              wr_put(w, p, 1);
            }
        }

      wr_put(w, "\n", 1);
    }

  free(v);
  free(buf);
  ret = wr_close(w);
  free(w);
  return ret;
}

int sheet_load_native(struct sheet_s *sh, const char *path)
{
  struct rd_s *rd;
  char *line;
  int len = 0;
  int lineno = 0;
  int ret = 0;
  int c;

  rd = malloc(sizeof(*rd));
  line = malloc(SH_INPUT_MAX + 128);
  if (rd == NULL || line == NULL)
    {
      free(rd);
      free(line);
      return -ENOMEM;
    }

  rd->fd = open(path, O_RDONLY);
  rd->n = 0;
  rd->pos = 0;
  if (rd->fd < 0)
    {
      ret = -errno;
      free(rd);
      free(line);
      return ret;
    }

  sheet_clear_all(sh);
  sh->batch = true;
  do
    {
      c = rd_get(rd);
      if (c >= 0 && c != '\n')
        {
          if (c != '\r' && len < SH_INPUT_MAX + 127)
            {
              line[len++] = c;
            }

          continue;
        }

      line[len] = '\0';
      len = 0;
      if (lineno++ == 0)
        {
          if (strncmp(line, "FPCSHEET", 8) != 0)
            {
              ret = -EINVAL;
              break;
            }

          continue;
        }

      if (line[0] == '#' || line[0] == '\0')
        {
          continue;
        }

      if (strncmp(line, "width ", 6) == 0)
        {
          char *p = line + 6;
          int col = -1;
          int k;

          k = 0;
          while (isalpha((unsigned char)p[k]))
            {
              k++;
            }

          if (k > 0 && k <= 2)
            {
              int r;
              char tmp[8];

              snprintf(tmp, sizeof(tmp), "%.*s1", k, p);
              if (sheet_parse_ref(tmp, &r, &col) > 0 && col >= 0)
                {
                  int wd = atoi(p + k);

                  sh->colw[col] = wd < SH_COLW_MIN ? SH_COLW_MIN :
                                  wd > SH_COLW_MAX ? SH_COLW_MAX : wd;
                }
            }

          continue;
        }

      {
        char *tab1 = strchr(line, '\t');
        char *tab2 = tab1 != NULL ? strchr(tab1 + 1, '\t') : NULL;
        char *s;
        char *d;
        uint32_t fmt;
        int r;
        int col;

        if (tab2 == NULL)
          {
            continue;
          }

        *tab1 = '\0';
        *tab2 = '\0';
        if (sheet_parse_ref(line, &r, &col) == 0)
          {
            continue;
          }

        fmt = strcmp(tab1 + 1, "-") == 0 ? 0 : fmt_parse(tab1 + 1);

        /* Unescape in place */

        for (s = d = tab2 + 1; *s != '\0'; s++)
          {
            if (*s == '\\' && s[1] != '\0')
              {
                s++;
                *d++ = *s == 't' ? '\t' : *s == 'n' ? '\n' :
                       *s == 'r' ? '\r' : *s;
              }
            else
              {
                *d++ = *s;
              }
          }

        *d = '\0';
        sheet_set_fmt(sh, r, col, fmt);
        sheet_set_input(sh, r, col, tab2 + 1, false);
        if (sheet_fmt(sh, r, col) != fmt)
          {
            sheet_set_fmt(sh, r, col, fmt);
          }
      }
    }
  while (c >= 0);

  close(rd->fd);
  free(rd);
  free(line);
  sh->batch = false;
  sheet_recalc_all(sh);
  return ret;
}

char *sheet_tsv(struct sheet_s *sh, const struct sh_rect_s *r, size_t *n)
{
  char buf[256];
  size_t cap = 256;
  size_t len = 0;
  char *out = malloc(cap);
  int y;
  int x;

  if (out == NULL)
    {
      return NULL;
    }

  for (y = r->r0; y <= r->r1; y++)
    {
      for (x = r->c0; x <= r->c1; x++)
        {
          size_t k;

          sheet_display(sh, sheet_get(sh, y, x), 15, buf, sizeof(buf));
          k = strlen(buf);
          if (len + k + 2 > cap)
            {
              char *p;

              cap = (len + k + 2) * 2;
              p = realloc(out, cap);
              if (p == NULL)
                {
                  free(out);
                  return NULL;
                }

              out = p;
            }

          memcpy(out + len, buf, k);
          len += k;
          out[len++] = x < r->c1 ? '\t' : '\n';
        }
    }

  out[len] = '\0';
  *n = len;
  return out;
}
