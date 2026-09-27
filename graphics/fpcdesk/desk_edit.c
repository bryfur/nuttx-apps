/****************************************************************************
 * apps/graphics/fpcdesk/desk_edit.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Text Editor.  The text is an array of lines, each its own allocation
 * grown as needed (so a file costs about its size plus 16 bytes a line).
 * The view: a tool bar (New, Save, Undo, Redo, the file's path), the
 * lines in IBM Plex Mono with their numbers in a gutter, a scroll bar and
 * a status bar.  Light colouring for C and for shell-like files.
 *
 * Drawing is by line: an edit marks the lines it changed (dirty), a line
 * added or removed moves the lines below it with a COPY within the window,
 * scrolling moves the view with a COPY, and only the lines that change or
 * come into view are drawn, each cut to the text area (ui_clip()).
 *
 * Keys: the VT100 sequences for arrows (with Shift: select; Ctrl: by
 * word), Home / End, Page Up / Down, Delete; Backspace, Enter (keeping the
 * indent), Tab; Ctrl-A select all, Ctrl-C / X / V copy / cut / paste (the
 * desktop's clipboard), Ctrl-S save, Ctrl-Z / Y undo / redo, Ctrl-N new.
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include "desk_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define EDIT_W         680
#define EDIT_H         460

#define TB_H           42                /* Tool bar */
#define ST_H           26                /* Status bar */
#define SB_W           12                /* Scroll bar */
#define LH             20                /* A line */
#define CW             9                 /* A character (Plex Mono 15) */
#define TABW           4
#define MAX_FILE       (4 * 1024 * 1024)
#define UNDO_MAX       200
#define DBLCLICK_MS    400

#define C_EDIT_BG      RGB(0x1c, 0x1c, 0x20)
#define C_LINENO       RGB(0x62, 0x62, 0x6a)
#define C_CARET        RGB(0xf2, 0xf2, 0xf4)
#define C_THUMB        RGB(0x80, 0x80, 0x88)

/* Syntax classes and their colours (g_syncol) */

#define S_TEXT         0
#define S_KEYWORD      1
#define S_TYPE         2
#define S_STRING       3
#define S_COMMENT      4
#define S_PREPROC      5
#define S_NUMBER       6

#define LANG_PLAIN     0
#define LANG_C         1
#define LANG_SHELL     2

/* Zones */

#define ZG_TOOL        0
#define ZG_CARD        1

#define Z_NEW          1
#define Z_SAVE         2
#define Z_UNDO         3
#define Z_REDO         4
#define Z_DISCARD      5
#define Z_CANCEL       6
#define Z_OK           7

/* Questions, on a card over the text */

#define P_NONE         0
#define P_CLOSE        1                 /* Unsaved changes: save first? */
#define P_SAVEAS       2                 /* A file name */

/* Mouse drags */

#define D_NONE         0
#define D_TEXT         1
#define D_THUMB        2

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct line_s
{
  char *s;
  int len;
  int cap;
  uint8_t cst;                           /* Starts in a block comment */
};

struct undo_s
{
  bool ins;                              /* Inserted s at (l, c), or
                                          * removed it from there */
  int l;
  int c;
  char *s;
  int n;
  uint32_t id;                           /* The state it leads to */
};

struct edit_s
{
  char path[PATH_MAX];                   /* "": untitled */
  struct line_s *ln;
  int nl;
  int capl;
  bool modified;
  bool crlf;                             /* Lines end in CR LF */
  int lang;

  int cl;                                /* The cursor: line, byte */
  int cc;
  int want;                              /* Column to keep going up/down */
  int al;                                /* The selection's other end */
  int ac;
  bool sel;
  int top;                               /* First line shown */
  int left;                              /* First column shown */
  bool blink;                            /* The cursor is drawn */

  struct undo_s undo[UNDO_MAX];
  int nundo;
  struct undo_s redo[UNDO_MAX];
  int nredo;
  bool typing;                           /* The last undo can grow */
  uint32_t ids;                          /* Undo states made */
  uint32_t savedid;                      /* The state saved (0: opened) */

  int dlo;                               /* Lines to draw, dlo > dhi: none */
  int dhi;
  bool full;                             /* Everything */
  int gw;                                /* Gutter width drawn with */
  char status[200];                      /* The status bar as drawn */
  char title[40];
  uint8_t *cls;                          /* Syntax classes of a line */
  int clscap;

  int prompt;
  bool closeafter;                       /* Save, then close */
  struct ui_field_s field;
  struct ui_keys_s keys;
  struct ui_zones_s zs;
  bool lastcr;

  uint8_t buttons;
  int drag;
  int grab;
  uint32_t clickms;
  int clickl;
  int clickc;
  int clicks;

  char msg[80];
  bool msgerr;
};

/* Where things are, in frame coordinates */

struct lay_s
{
  int cw;
  int ty;                                /* Text area */
  int th;
  int rows;                              /* Lines shown, the last may be
                                          * cut */
  int full;                              /* ... whole ones */
  int gw;                                /* Gutter */
  int x0;                                /* Text */
  int xr;                                /* Right of the text area */
  int cols;
  int sy;                                /* Status bar */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct desk_ops_s g_o;
static const char *g_openpath;

static const uint16_t g_syncol[7] =
{
  C_TEXT,
  RGB(0xff, 0x8f, 0x70),                 /* Keywords: orange */
  RGB(0x70, 0xc4, 0xff),                 /* Types: blue */
  RGB(0xa4, 0xde, 0x7c),                 /* Strings: green */
  RGB(0x7c, 0x82, 0x8c),                 /* Comments: grey */
  RGB(0xd4, 0xa4, 0xff),                 /* Preprocessor, $vars: purple */
  RGB(0xff, 0xd0, 0x60),                 /* Numbers: yellow */
};

static const char * const g_ckw[] =
{
  "break", "case", "continue", "default", "do", "else", "for", "goto", "if",
  "return", "sizeof", "switch", "while", "typedef", "struct", "union",
  "enum", "static", "extern", "const", "volatile", "inline", "register",
  "restrict", "NULL", "true", "false", "class", "public", "private",
  "namespace", "new", "delete", "template", "this", NULL
};

static const char * const g_ctype[] =
{
  "void", "char", "short", "int", "long", "float", "double", "signed",
  "unsigned", "bool", "size_t", "ssize_t", "off_t", "uint8_t", "uint16_t",
  "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
  "uintptr_t", "FAR", "FILE", NULL
};

static const char * const g_shkw[] =
{
  "if", "then", "else", "elif", "fi", "for", "in", "do", "done", "while",
  "until", "case", "esac", "function", "return", "local", "export", "set",
  "unset", "shift", "exit", "echo", "cd", "source", "def", "import", "from",
  "class", "ifeq", "ifneq", "endif", "include", "config", "bool", "string",
  "default", "depends", "select", NULL
};

/****************************************************************************
 * Private Functions: the text
 ****************************************************************************/

static int ln_reserve(struct line_s *l, int need)
{
  char *s;
  int cap;

  if (need <= l->cap)
    {
      return 0;
    }

  cap = need < 16 ? 16 : need + need / 2;
  s = realloc(l->s, cap);
  if (s == NULL)
    {
      return -ENOMEM;
    }

  l->s = s;
  l->cap = cap;
  return 0;
}

/* Make count empty lines at index at */

static int ln_make(struct edit_s *e, int at, int count)
{
  if (e->nl + count > e->capl)
    {
      int cap = (e->nl + count) + (e->nl + count) / 2 + 16;
      struct line_s *ln = realloc(e->ln, cap * sizeof(*ln));

      if (ln == NULL)
        {
          return -ENOMEM;
        }

      e->ln = ln;
      e->capl = cap;
    }

  memmove(&e->ln[at + count], &e->ln[at], (e->nl - at) * sizeof(*e->ln));
  memset(&e->ln[at], 0, count * sizeof(*e->ln));
  e->nl += count;
  return 0;
}

static void ln_free(struct edit_s *e)
{
  int i;

  for (i = 0; i < e->nl; i++)
    {
      free(e->ln[i].s);
    }

  free(e->ln);
  e->ln = NULL;
  e->nl = 0;
  e->capl = 0;
}

/* Insert n bytes at (l, c) ('\n' makes lines, '\r' before it goes); the
 * end in *el, *ec
 */

static int buf_insert(struct edit_s *e, int l, int c, const char *s, int n,
                      int *el, int *ec)
{
  struct line_s *ln;
  char *tail = NULL;
  int tlen;
  int k = 0;
  int i;
  int j;

  for (i = 0; i < n; i++)
    {
      k += s[i] == '\n';
    }

  if (k == 0)
    {
      ln = &e->ln[l];
      if (ln_reserve(ln, ln->len + n) < 0)
        {
          return -ENOMEM;
        }

      memmove(ln->s + c + n, ln->s + c, ln->len - c);
      memcpy(ln->s + c, s, n);
      ln->len += n;
      *el = l;
      *ec = c + n;
      return 0;
    }

  /* The rest of line l goes after the last piece */

  tlen = e->ln[l].len - c;
  if (tlen > 0)
    {
      tail = malloc(tlen);
      if (tail == NULL)
        {
          return -ENOMEM;
        }

      memcpy(tail, e->ln[l].s + c, tlen);
    }

  if (ln_make(e, l + 1, k) < 0)
    {
      free(tail);
      return -ENOMEM;
    }

  e->ln[l].len = c;
  for (i = 0, j = l; i <= n; j++)
    {
      int start = i;
      int end;

      while (i < n && s[i] != '\n')
        {
          i++;
        }

      end = i;
      if (i < n && end > start && s[end - 1] == '\r')
        {
          end--;
        }

      ln = &e->ln[j];
      if (ln_reserve(ln, ln->len + end - start + (i == n ? tlen : 0)) < 0)
        {
          free(tail);
          return -ENOMEM;
        }

      memcpy(ln->s + ln->len, s + start, end - start);
      ln->len += end - start;
      if (i == n)
        {
          *el = j;
          *ec = ln->len;
          if (tlen > 0)
            {
              memcpy(ln->s + ln->len, tail, tlen);
              ln->len += tlen;
            }

          break;
        }

      i++;                               /* The '\n' */
    }

  free(tail);
  return 0;
}

/* The text from (l0, c0) to (l1, c1), lines joined by '\n' (malloc'd, NUL
 * after); remove: take it out
 */

static char *buf_take(struct edit_s *e, int l0, int c0, int l1, int c1,
                      int *n, bool remove)
{
  char *s;
  int len = 0;
  int i;
  int p = 0;

  for (i = l0; i <= l1; i++)
    {
      int a = i == l0 ? c0 : 0;
      int b = i == l1 ? c1 : e->ln[i].len;

      len += b - a + (i < l1);
    }

  s = malloc(len + 1);
  if (s == NULL)
    {
      return NULL;
    }

  for (i = l0; i <= l1; i++)
    {
      int a = i == l0 ? c0 : 0;
      int b = i == l1 ? c1 : e->ln[i].len;

      memcpy(s + p, e->ln[i].s + a, b - a);
      p += b - a;
      if (i < l1)
        {
          s[p++] = '\n';
        }
    }

  s[p] = '\0';
  *n = len;

  if (remove)
    {
      struct line_s *first = &e->ln[l0];
      struct line_s *last = &e->ln[l1];
      int rest = last->len - c1;

      if (l0 == l1)
        {
          memmove(first->s + c0, first->s + c1, rest);
          first->len = c0 + rest;
        }
      else
        {
          if (ln_reserve(first, c0 + rest) < 0)
            {
              free(s);
              return NULL;
            }

          memcpy(first->s + c0, last->s + c1, rest);
          first->len = c0 + rest;
          for (i = l0 + 1; i <= l1; i++)
            {
              free(e->ln[i].s);
            }

          memmove(&e->ln[l0 + 1], &e->ln[l1 + 1],
                  (e->nl - l1 - 1) * sizeof(*e->ln));
          e->nl -= l1 - l0;
        }
    }

  return s;
}

/* Where text s inserted at (l, c) ends */

static void text_end(const char *s, int n, int l, int c, int *el, int *ec)
{
  int i;

  *el = l;
  *ec = c;
  for (i = 0; i < n; i++)
    {
      if (s[i] == '\n')
        {
          (*el)++;
          *ec = 0;
        }
      else if (s[i] != '\r' || i + 1 >= n || s[i + 1] != '\n')
        {
          (*ec)++;
        }
    }
}

/* Visual columns (tabs to multiples of TABW) */

static int vcol(struct line_s *l, int c)
{
  int v = 0;
  int i;

  for (i = 0; i < c && i < l->len; i++)
    {
      v = l->s[i] == '\t' ? (v / TABW + 1) * TABW : v + 1;
    }

  return v;
}

static int col_at(struct line_s *l, int vc)
{
  int v = 0;
  int i;

  for (i = 0; i < l->len; i++)
    {
      int nv = l->s[i] == '\t' ? (v / TABW + 1) * TABW : v + 1;

      if (vc < nv)
        {
          return vc - v <= (nv - v) / 2 || l->s[i] != '\t' ? i : i + 1;
        }

      v = nv;
    }

  return l->len;
}

static bool is_word(uint8_t c)
{
  return isalnum(c) || c == '_';
}

/****************************************************************************
 * Private Functions: colours
 ****************************************************************************/

static bool in_words(const char *s, int n, const char * const *list)
{
  int i;

  for (i = 0; list[i] != NULL; i++)
    {
      if ((int)strlen(list[i]) == n && strncmp(s, list[i], n) == 0)
        {
          return true;
        }
    }

  return false;
}

/* The classes of line i's characters into cls (may be NULL: just the
 * state at its end); returns whether it ends in a block comment
 */

static int syn_line(struct edit_s *e, int li, uint8_t *cls)
{
  struct line_s *l = &e->ln[li];
  const char *s = l->s;
  int n = l->len;
  int st = l->cst;
  int i = 0;
  int j;
  int k;
  bool first = true;

#define MARK(a, b, c) do { if (cls) memset(cls + (a), (c), (b) - (a)); } \
                      while (0)

  if (cls != NULL)
    {
      memset(cls, S_TEXT, n);
    }

  if (e->lang == LANG_PLAIN)
    {
      return 0;
    }

  while (i < n)
    {
      uint8_t c = s[i];

      if (e->lang == LANG_C && st)
        {
          for (j = i; j + 1 < n && !(s[j] == '*' && s[j + 1] == '/'); j++)
            {
            }

          if (j + 1 < n)
            {
              MARK(i, j + 2, S_COMMENT);
              i = j + 2;
              st = 0;
            }
          else
            {
              MARK(i, n, S_COMMENT);
              i = n;
            }

          continue;
        }

      if (c == ' ' || c == '\t')
        {
          i++;
          continue;
        }

      if (e->lang == LANG_C && c == '/' && i + 1 < n && s[i + 1] == '/')
        {
          MARK(i, n, S_COMMENT);
          break;
        }

      if (e->lang == LANG_C && c == '/' && i + 1 < n && s[i + 1] == '*')
        {
          MARK(i, i + 2, S_COMMENT);
          i += 2;
          st = 1;
          continue;
        }

      if (e->lang == LANG_SHELL && c == '#' &&
          (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
        {
          MARK(i, n, S_COMMENT);
          break;
        }

      if (e->lang == LANG_C && c == '#' && first)
        {
          for (j = i + 1; j < n && (s[j] == ' ' || isalpha((uint8_t)s[j]));
               j++)
            {
            }

          MARK(i, j, S_PREPROC);
          if (j < n && s[j] == '<')
            {
              for (k = j; k < n && s[k] != '>'; k++)
                {
                }

              k += k < n;
              MARK(j, k, S_STRING);
              j = k;
            }

          i = j;
          first = false;
          continue;
        }

      first = false;
      if (c == '"' || c == '\'' || (c == '`' && e->lang == LANG_SHELL))
        {
          for (j = i + 1; j < n && s[j] != c; j++)
            {
              j += s[j] == '\\' && c != '\'';
            }

          j += j < n;
          MARK(i, j > n ? n : j, S_STRING);
          i = j;
          continue;
        }

      if (e->lang == LANG_SHELL && c == '$' && i + 1 < n)
        {
          j = i + 1;
          if (s[j] == '{' || s[j] == '(')
            {
              char close = s[j] == '{' ? '}' : ')';

              while (j < n && s[j] != close)
                {
                  j++;
                }

              j += j < n;
            }
          else
            {
              while (j < n && (is_word(s[j]) || (j == i + 1 &&
                     strchr("?#@*!$-", s[j]) != NULL)))
                {
                  j++;
                  if (j == i + 2 && !is_word(s[j - 1]))
                    {
                      break;
                    }
                }
            }

          MARK(i, j, S_PREPROC);
          i = j;
          continue;
        }

      if (isdigit(c) && (i == 0 || !is_word(s[i - 1])))
        {
          for (j = i; j < n && (is_word(s[j]) || s[j] == '.'); j++)
            {
            }

          MARK(i, j, S_NUMBER);
          i = j;
          continue;
        }

      if (isalpha(c) || c == '_')
        {
          for (j = i; j < n && is_word(s[j]); j++)
            {
            }

          if (e->lang == LANG_C)
            {
              if (in_words(s + i, j - i, g_ckw))
                {
                  MARK(i, j, S_KEYWORD);
                }
              else if (in_words(s + i, j - i, g_ctype) ||
                       (j - i > 2 && s[j - 2] == '_' && s[j - 1] == 't'))
                {
                  MARK(i, j, S_TYPE);
                }
            }
          else if (in_words(s + i, j - i, g_shkw))
            {
              MARK(i, j, S_KEYWORD);
            }

          i = j;
          continue;
        }

      i++;
    }

#undef MARK

  return e->lang == LANG_C ? st : 0;
}

/* After lines [a, b] changed: carry the block comment state down, marking
 * the lines whose start changed
 */

static void dirty(struct edit_s *e, int a, int b);

static void syn_update(struct edit_s *e, int a, int b)
{
  int i;

  if (e->lang != LANG_C)
    {
      return;
    }

  if (a == 0)
    {
      e->ln[0].cst = 0;
    }

  for (i = a; i < e->nl - 1; i++)
    {
      int st = syn_line(e, i, NULL);

      if (e->ln[i + 1].cst != st)
        {
          e->ln[i + 1].cst = st;
          dirty(e, i + 1, i + 1);
        }
      else if (i >= b)
        {
          break;
        }
    }
}

static int lang_of(const char *path)
{
  const char *name = strrchr(path, '/');
  const char *ext;

  name = name != NULL ? name + 1 : path;
  ext = strrchr(name, '.');
  if (ext != NULL && (strcasecmp(ext, ".c") == 0 ||
      strcasecmp(ext, ".h") == 0 || strcasecmp(ext, ".cpp") == 0 ||
      strcasecmp(ext, ".cc") == 0 || strcasecmp(ext, ".hpp") == 0 ||
      strcasecmp(ext, ".cxx") == 0 || strcasecmp(ext, ".js") == 0 ||
      strcasecmp(ext, ".java") == 0 || strcasecmp(ext, ".v") == 0))
    {
      return LANG_C;
    }

  if ((ext != NULL && (strcasecmp(ext, ".sh") == 0 ||
      strcasecmp(ext, ".mk") == 0 || strcasecmp(ext, ".py") == 0 ||
      strcasecmp(ext, ".cfg") == 0 || strcasecmp(ext, ".conf") == 0 ||
      strcasecmp(ext, ".nsh") == 0 || strcasecmp(ext, ".toml") == 0 ||
      strcasecmp(ext, ".yml") == 0 || strcasecmp(ext, ".yaml") == 0)) ||
      strcmp(name, "Makefile") == 0 || strcmp(name, "Kconfig") == 0 ||
      strcmp(name, "Make.defs") == 0 || strcmp(name, "defconfig") == 0)
    {
      return LANG_SHELL;
    }

  return LANG_PLAIN;
}

/****************************************************************************
 * Private Functions: files
 ****************************************************************************/

static const char *base_name(struct edit_s *e)
{
  const char *s = strrchr(e->path, '/');

  return e->path[0] == '\0' ? "Untitled" : s != NULL ? s + 1 : e->path;
}

static int ed_load(struct edit_s *e, const char *path)
{
  struct stat st;
  char *buf;
  int fd;
  int ret = 0;
  bool cr = false;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &st) == 0 && st.st_size > MAX_FILE)
    {
      close(fd);
      return -EFBIG;
    }

  buf = malloc(1024);
  if (buf == NULL || ln_make(e, 0, 1) < 0)
    {
      free(buf);
      close(fd);
      return -ENOMEM;
    }

  for (; ; )
    {
      ssize_t n = read(fd, buf, 1024);
      int i = 0;

      if (n < 0)
        {
          ret = -errno;
          break;
        }

      if (n == 0)
        {
          break;
        }

      while (i < n)
        {
          struct line_s *l = &e->ln[e->nl - 1];
          int j = i;

          while (j < n && buf[j] != '\n')
            {
              j++;
            }

          if (ln_reserve(l, l->len + j - i) < 0)
            {
              ret = -ENOMEM;
              break;
            }

          memcpy(l->s + l->len, buf + i, j - i);
          l->len += j - i;
          if (j < n)
            {
              if (l->len > 0 && l->s[l->len - 1] == '\r')
                {
                  l->len--;
                  cr = true;
                }

              if (ln_make(e, e->nl, 1) < 0)
                {
                  ret = -ENOMEM;
                  break;
                }
            }

          i = j + 1;
        }

      if (ret < 0)
        {
          break;
        }
    }

  free(buf);
  close(fd);
  if (ret < 0)
    {
      return ret;
    }

  e->crlf = cr;
  strlcpy(e->path, path, sizeof(e->path));
  e->lang = lang_of(path);
  syn_update(e, 0, e->nl);
  return 0;
}

static int ed_save(struct edit_s *e, const char *path)
{
  FILE *fp;
  int ret = 0;
  int i;

  fp = fopen(path, "w");
  if (fp == NULL)
    {
      return -errno;
    }

  setvbuf(fp, NULL, _IOFBF, 1024);
  for (i = 0; i < e->nl && ret == 0; i++)
    {
      if ((e->ln[i].len > 0 &&
           fwrite(e->ln[i].s, e->ln[i].len, 1, fp) != 1) ||
          (i < e->nl - 1 && fputs(e->crlf ? "\r\n" : "\n", fp) < 0))
        {
          ret = errno ? -errno : -EIO;
        }
    }

  if (fclose(fp) != 0 && ret == 0)
    {
      ret = errno ? -errno : -EIO;
    }

  return ret;
}

/****************************************************************************
 * Private Functions: drawing
 ****************************************************************************/

static int digits(int n)
{
  int d = 1;

  while (n >= 10)
    {
      n /= 10;
      d++;
    }

  return d;
}

static void layout(struct desk_win_s *w, struct lay_s *l)
{
  struct edit_s *e = w->priv;
  int d = digits(e->nl);

  l->cw   = w->csize.w;
  l->ty   = TITLE_H + TB_H;
  l->sy   = TITLE_H + w->csize.h - ST_H;
  l->th   = l->sy - l->ty;
  l->th   = l->th < 0 ? 0 : l->th;
  l->rows = (l->th + LH - 1) / LH;
  l->full = l->th / LH;
  l->full = l->full < 1 ? 1 : l->full;
  l->gw   = (d < 3 ? 3 : d) * CW + 20;
  l->x0   = l->gw + 6;
  l->xr   = l->cw - SB_W;
  l->cols = (l->xr - l->x0 - 4) / CW;
  l->cols = l->cols < 1 ? 1 : l->cols;
}

static void dirty(struct edit_s *e, int a, int b)
{
  if (a > b)
    {
      int t = a;

      a = b;
      b = t;
    }

  if (e->dlo > e->dhi)
    {
      e->dlo = a;
      e->dhi = b;
    }
  else
    {
      e->dlo = a < e->dlo ? a : e->dlo;
      e->dhi = b > e->dhi ? b : e->dhi;
    }
}

/* The selection, ordered */

static void sel_range(struct edit_s *e, int *l0, int *c0, int *l1, int *c1)
{
  if (e->al < e->cl || (e->al == e->cl && e->ac < e->cc))
    {
      *l0 = e->al;
      *c0 = e->ac;
      *l1 = e->cl;
      *c1 = e->cc;
    }
  else
    {
      *l0 = e->cl;
      *c0 = e->cc;
      *l1 = e->al;
      *c1 = e->ac;
    }
}

/* Line i where it is shown (its number, its text, the selection, the
 * cursor), cut to the text area
 */

static void line_draw(struct desk_win_s *w, struct lay_s *l, int i)
{
  struct edit_s *e = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  const struct desk_font_s *font = &g_desk_font_mono;
  int r = i - e->top;
  int y = l->ty + r * LH;
  int ty = y + (LH - text_height(font)) / 2 + 1;
  int gy = ty + font->ascent - g_desk_font_small.ascent;
  int start;

  if (r < 0 || r >= l->rows)
    {
      return;
    }

  ui_flush(hwnd, &g_o, l->cols + 24);
  start = g_o.n;
  ops_fill(&g_o, 0, y, l->xr, LH, C_EDIT_BG);
  if (i < e->nl)
    {
      struct line_s *ln = &e->ln[i];
      char num[12];
      char ch[2];
      int v = 0;
      int k;

      snprintf(num, sizeof(num), "%d", i + 1);
      ops_text(&g_o, &g_desk_font_small,
               l->gw - 10 - text_width(&g_desk_font_small, num), gy, num,
               i == e->cl ? C_TEXT : C_LINENO, 255);

      /* The selection: behind the text, past the end if it goes on */

      if (e->sel)
        {
          int l0;
          int c0;
          int l1;
          int c1;

          sel_range(e, &l0, &c0, &l1, &c1);
          if (i >= l0 && i <= l1)
            {
              int v0 = i == l0 ? vcol(ln, c0) : 0;
              int v1 = i == l1 ? vcol(ln, c1) : vcol(ln, ln->len) + 1;

              v0 = v0 < e->left ? e->left : v0;
              v1 = v1 > e->left + l->cols ? e->left + l->cols : v1;
              if (v1 > v0)
                {
                  ops_blend(&g_o, l->x0 + (v0 - e->left) * CW, y,
                            (v1 - v0) * CW, LH, C_ACCENT, 120);
                }
            }
        }

      /* The characters in their colours */

      if (e->lang != LANG_PLAIN && ln->len > e->clscap)
        {
          uint8_t *c = realloc(e->cls, ln->len + 64);

          if (c != NULL)
            {
              e->cls = c;
              e->clscap = ln->len + 64;
            }
        }

      if (e->lang != LANG_PLAIN && ln->len <= e->clscap)
        {
          syn_line(e, i, e->cls);
        }

      ch[1] = '\0';
      for (k = 0; k < ln->len && v < e->left + l->cols; k++)
        {
          uint8_t c = ln->s[k];

          if (c == '\t')
            {
              v = (v / TABW + 1) * TABW;
              continue;
            }

          if (v >= e->left && c != ' ')
            {
              ch[0] = c >= 0x20 && c < 0x7f ? c : '?';
              ops_text(&g_o, font, l->x0 + (v - e->left) * CW, ty, ch,
                       e->lang != LANG_PLAIN && ln->len <= e->clscap ?
                       g_syncol[e->cls[k]] : C_TEXT, 255);
            }

          v++;
        }

      /* The cursor */

      if (i == e->cl && e->blink && w->active && e->prompt == P_NONE)
        {
          int cv = vcol(ln, e->cc) - e->left;

          if (cv >= 0 && cv <= l->cols)
            {
              ops_fill(&g_o, l->x0 + cv * CW - 1, y + 1, 2, LH - 2,
                       C_CARET);
            }
        }
    }

  ui_clip(&g_o, start, 0, l->ty, l->xr, l->ty + l->th);
}

static void scrollbar_draw(struct desk_win_s *w, struct lay_s *l)
{
  struct edit_s *e = w->priv;
  int total = e->nl + l->full - 1;      /* Can scroll the last to the top */

  ui_flush((NXWINDOW)w->hwnd, &g_o, 4);
  ops_fill(&g_o, l->xr, l->ty, SB_W, l->th, C_EDIT_BG);
  if (e->nl > l->full && l->th > 0)
    {
      int th = l->th * l->full / total;
      int ty;

      th = th < 24 ? 24 : th;
      ty = l->ty + (l->th - th) * e->top / (e->nl - 1);
      ops_rrect(&g_o, l->xr + 3, ty + 2, 6, th - 4, LUT_R3, 3, C_THUMB,
                e->drag == D_THUMB ? 255 : 170);
    }
}

static const char *lang_name(int lang)
{
  return lang == LANG_C ? "C" : lang == LANG_SHELL ? "Shell" : "Plain Text";
}

static void status_draw(struct desk_win_s *w, struct lay_s *l, bool force)
{
  struct edit_s *e = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  const struct desk_font_s *font = &g_desk_font_ui;
  int ty = l->sy + (ST_H - text_height(font)) / 2;
  char right[64];
  char left[96];
  char fit[96];
  char all[200];
  int rw;

  snprintf(right, sizeof(right), "Ln %d, Col %d    %s", e->cl + 1,
           vcol(&e->ln[e->cl], e->cc) + 1, lang_name(e->lang));
  if (e->msg[0] != '\0')
    {
      strlcpy(left, e->msg, sizeof(left));
    }
  else
    {
      snprintf(left, sizeof(left), "%s%d line%s%s", e->modified ?
               "Modified  " DESK_CH_MIDDOT "  " : "", e->nl,
               e->nl == 1 ? "" : "s", e->crlf ? "  " DESK_CH_MIDDOT
               "  CR LF" : "");
    }

  snprintf(all, sizeof(all), "%s|%s|%d", left, right, l->cw);
  if (!force && strcmp(all, e->status) == 0)
    {
      return;
    }

  strlcpy(e->status, all, sizeof(e->status));
  ui_flush(hwnd, &g_o, 120);
  ops_fill(&g_o, 0, l->sy, l->cw, ST_H, C_UI_BAR);
  ops_fill(&g_o, 0, l->sy, l->cw, 1, C_UI_LINE);
  rw = text_width(font, right);
  ops_text(&g_o, font, l->cw - 14 - rw, ty, right, C_TEXT_DIM, 255);
  ui_fit(font, left, l->cw - 44 - rw, fit, sizeof(fit));
  ops_text(&g_o, font, 12, ty, fit, e->msg[0] != '\0' && e->msgerr ?
           C_UI_ERROR : C_TEXT_DIM, 255);
}

static int tool_button(struct edit_s *e, int x, int y, int id,
                       const char *label, bool enabled, bool accent)
{
  int bw = ui_button_width(label);
  int lk = ui_look(&e->zs, id, enabled);

  if (accent && lk == UI_NORMAL)
    {
      lk = UI_DEFAULT;
    }

  ui_button(&g_o, x, y, bw, 28, label, lk);
  ui_zone_add(&e->zs, ZG_TOOL, id, x, y, bw, 28, enabled);
  return x + bw + 6;
}

static void toolbar_draw(struct desk_win_s *w, struct lay_s *l)
{
  struct edit_s *e = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  const struct desk_font_s *font = &g_desk_font_ui;
  int y = TITLE_H + (TB_H - 28) / 2;
  int x = 10;
  char fit[PATH_MAX];

  ui_zones_clear(&e->zs, ZG_TOOL);
  ui_flush(hwnd, &g_o, 200);
  ops_fill(&g_o, 0, TITLE_H, l->cw, TB_H, C_UI_BAR);
  ops_fill(&g_o, 0, TITLE_H + TB_H - 1, l->cw, 1, C_UI_LINE);
  x = tool_button(e, x, y, Z_NEW, "New", true, false);
  x = tool_button(e, x, y, Z_SAVE, "Save", true, e->modified);
  x += 8;
  x = tool_button(e, x, y, Z_UNDO, "Undo", e->nundo > 0, false);
  x = tool_button(e, x, y, Z_REDO, "Redo", e->nredo > 0, false);
  x += 8;
  ui_fit(font, e->path[0] != '\0' ? e->path : "Not saved yet",
         l->cw - x - 12, fit, sizeof(fit));
  ops_text(&g_o, font, x, TITLE_H + (TB_H - text_height(font)) / 2, fit,
           C_TEXT_DIM, 255);
  ops_draw(hwnd, &g_o);
}

/* The question, on a card in the middle of the text */

static void card_draw(struct desk_win_s *w, struct lay_s *l)
{
  struct edit_s *e = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  int cw = 400 < l->cw - 24 ? 400 : l->cw - 24;
  int ch = 136;
  int x = (l->cw - cw) / 2;
  int y = l->ty + (l->th - ch) / 2;
  int by = y + ch - 44;
  int bx = x + cw - 16;
  char buf[96];
  char fit[96];
  int bw;

  ui_zones_clear(&e->zs, ZG_CARD);
  if (e->prompt == P_NONE)
    {
      return;
    }

  y = y < l->ty + 4 ? l->ty + 4 : y;
  ops_draw(hwnd, &g_o);
  ops_rrect(&g_o, x - 1, y - 1, cw + 2, ch + 2, LUT_R12, 12, C_UI_LINE, 255);
  ops_rrect(&g_o, x, y, cw, ch, LUT_R12, 12, C_CARD, 255);

  if (e->prompt == P_CLOSE)
    {
      snprintf(buf, sizeof(buf), "Save changes to \"%s\"?", base_name(e));
      ui_fit(&g_desk_font_uib, buf, cw - 36, fit, sizeof(fit));
      ops_text(&g_o, &g_desk_font_uib, x + 18, y + 18, fit, C_TEXT, 255);
      ops_text(&g_o, &g_desk_font_ui, x + 18, y + 44,
               "If you don't save, the changes are lost.", C_TEXT_DIM, 255);

      bw = ui_button_width("Save");
      bx -= bw;
      ui_button(&g_o, bx, by, bw, 28, "Save",
                ui_look(&e->zs, Z_OK, true) == UI_PRESSED ? UI_PRESSED :
                UI_DEFAULT);
      ui_zone_add(&e->zs, ZG_CARD, Z_OK, bx, by, bw, 28, true);
      bw = ui_button_width("Cancel");
      bx -= bw + 8;
      ui_button(&g_o, bx, by, bw, 28, "Cancel",
                ui_look(&e->zs, Z_CANCEL, true));
      ui_zone_add(&e->zs, ZG_CARD, Z_CANCEL, bx, by, bw, 28, true);
      bw = ui_button_width("Don't Save");
      ui_button(&g_o, x + 16, by, bw, 28, "Don't Save",
                ui_look(&e->zs, Z_DISCARD, true) == UI_NORMAL ? UI_DANGER :
                ui_look(&e->zs, Z_DISCARD, true));
      ui_zone_add(&e->zs, ZG_CARD, Z_DISCARD, x + 16, by, bw, 28, true);
    }
  else
    {
      ops_text(&g_o, &g_desk_font_uib, x + 18, y + 16, "Save As", C_TEXT,
               255);
      ui_field_draw(&g_o, &e->field, x + 16, y + 44, cw - 32, 30, true);
      bw = ui_button_width("Save");
      bx -= bw;
      ui_button(&g_o, bx, by, bw, 28, "Save", e->field.len == 0 ?
                UI_DISABLED : ui_look(&e->zs, Z_OK, true) == UI_PRESSED ?
                UI_PRESSED : UI_DEFAULT);
      ui_zone_add(&e->zs, ZG_CARD, Z_OK, bx, by, bw, 28, e->field.len > 0);
      bw = ui_button_width("Cancel");
      bx -= bw + 8;
      ui_button(&g_o, bx, by, bw, 28, "Cancel",
                ui_look(&e->zs, Z_CANCEL, true));
      ui_zone_add(&e->zs, ZG_CARD, Z_CANCEL, bx, by, bw, 28, true);
    }

  ops_draw(hwnd, &g_o);
}

static void text_draw(struct desk_win_s *w, struct lay_s *l)
{
  struct edit_s *e = w->priv;
  int i;

  for (i = e->top; i < e->top + l->rows; i++)
    {
      line_draw(w, l, i);
    }
}

static void set_title(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;
  char t[40];

  snprintf(t, sizeof(t), "%s%s", base_name(e),
           e->modified ? "  " DESK_CH_BULLET : "");
  if (strcmp(t, e->title) != 0)
    {
      strlcpy(e->title, t, sizeof(e->title));
      strlcpy(w->title, t, sizeof(w->title));
      desk_win_title(w);
    }
}

static void edit_draw(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;
  struct lay_s l;

  if (e == NULL)
    {
      return;
    }

  layout(w, &l);
  e->gw = l.gw;
  ops_reset(&g_o);
  toolbar_draw(w, &l);
  text_draw(w, &l);
  scrollbar_draw(w, &l);
  status_draw(w, &l, true);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
  card_draw(w, &l);
  e->full = false;
  e->dlo = 1;
  e->dhi = 0;
}

/* Scroll the view to line top: what stays is copied */

static void scroll_to(struct desk_win_s *w, struct lay_s *l, int top)
{
  struct edit_s *e = w->priv;
  struct nx_gpuop_s *op;
  int d;

  top = top > e->nl - 1 ? e->nl - 1 : top;
  top = top < 0 ? 0 : top;
  d = top - e->top;
  if (d == 0)
    {
      return;
    }

  e->top = top;
  if (e->full || d >= l->full || -d >= l->full)
    {
      e->full = true;
      return;
    }

  ui_flush((NXWINDOW)w->hwnd, &g_o, 1);
  op = ops_add(&g_o);
  op->op = NX_GPUOP_COPY;
  op->x  = 0;
  op->w  = l->xr;
  op->sx = 0;
  op->h  = l->th - (d > 0 ? d : -d) * LH;
  if (d > 0)
    {
      op->y  = l->ty;
      op->sy = l->ty + d * LH;
      dirty(e, top + l->rows - d - 1, top + l->rows - 1);
    }
  else
    {
      op->y  = l->ty - d * LH;
      op->sy = l->ty;
      dirty(e, top, top - d);
    }
}

/* Lines after old line from moved by d (lines were added or removed): move
 * their text (not the numbers: those stay with the rows)
 */

static void shift_lines(struct desk_win_s *w, struct lay_s *l, int from,
                        int d, int oldnl)
{
  struct edit_s *e = w->priv;
  struct nx_gpuop_s *op;
  int r = from - e->top;                 /* Its row */
  int h;

  /* Rows whose number shows or goes */

  dirty(e, oldnl < e->nl ? oldnl : e->nl, oldnl > e->nl ? oldnl : e->nl);
  if (d == 0 || e->full)
    {
      return;
    }

  if (from < e->top || r >= l->rows)
    {
      if (from < e->top)
        {
          e->full = true;
        }

      return;
    }

  if (d > 0)
    {
      h = l->th - (r + d) * LH;
      if (h > 0)
        {
          ui_flush((NXWINDOW)w->hwnd, &g_o, 1);
          op = ops_add(&g_o);
          op->op = NX_GPUOP_COPY;
          op->x  = l->gw;
          op->w  = l->xr - l->gw;
          op->sx = l->gw;
          op->y  = l->ty + (r + d) * LH;
          op->sy = l->ty + r * LH;
          op->h  = h;
        }

      dirty(e, from, from + d - 1);
    }
  else
    {
      h = l->th - r * LH;
      if (h > 0 && r + d >= 0)
        {
          ui_flush((NXWINDOW)w->hwnd, &g_o, 1);
          op = ops_add(&g_o);
          op->op = NX_GPUOP_COPY;
          op->x  = l->gw;
          op->w  = l->xr - l->gw;
          op->sx = l->gw;
          op->y  = l->ty + (r + d) * LH;
          op->sy = l->ty + r * LH;
          op->h  = h;
          dirty(e, e->top + l->rows + d - 1, e->top + l->rows - 1);
        }
      else
        {
          e->full = true;
        }
    }
}

/* Bring the view up to date: what changed drawn (follow: with the cursor
 * in view)
 */

static void refresh(struct desk_win_s *w, bool follow)
{
  struct edit_s *e = w->priv;
  struct lay_s l;
  int v;
  int i;

  layout(w, &l);
  if (l.gw != e->gw)
    {
      e->gw = l.gw;
      e->full = true;
    }

  /* The cursor in view */

  if (follow && e->cl < e->top)
    {
      scroll_to(w, &l, e->cl);
    }
  else if (follow && e->cl >= e->top + l.full)
    {
      scroll_to(w, &l, e->cl - l.full + 1);
    }

  v = vcol(&e->ln[e->cl], e->cc);
  if (follow && v < e->left)
    {
      e->left = v > 8 ? v - 8 : 0;
      e->full = true;
    }
  else if (follow && v >= e->left + l.cols)
    {
      e->left = v - l.cols + 8;
      e->full = true;
    }

  if (e->full)
    {
      text_draw(w, &l);
    }
  else if (e->dlo <= e->dhi)
    {
      int a = e->dlo < e->top ? e->top : e->dlo;
      int b = e->dhi > e->top + l.rows ? e->top + l.rows : e->dhi;

      for (i = a; i <= b; i++)
        {
          line_draw(w, &l, i);
        }
    }

  scrollbar_draw(w, &l);
  status_draw(w, &l, false);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
  if (e->full && e->prompt != P_NONE)
    {
      card_draw(w, &l);
    }

  e->full = false;
  e->dlo = 1;
  e->dhi = 0;
  set_title(w);
}

/****************************************************************************
 * Private Functions: editing
 ****************************************************************************/

static void set_msg(struct edit_s *e, bool err, const char *msg,
                    const char *arg)
{
  snprintf(e->msg, sizeof(e->msg), msg, arg);
  e->msgerr = err;
}

/* Lines [first, first + oldn) became [first, first + newn) */

static void changed(struct desk_win_s *w, int first, int oldn, int newn,
                    int oldnl)
{
  struct edit_s *e = w->priv;
  struct lay_s l;
  bool was = e->modified;

  layout(w, &l);
  if (l.gw != e->gw)
    {
      e->full = true;
    }

  if (newn != oldn)
    {
      shift_lines(w, &l, first + oldn, newn - oldn, oldnl);
    }

  dirty(e, first, first + newn - 1);
  syn_update(e, first, first + newn - 1);
  e->modified = true;
  e->msg[0] = '\0';
  if (!was)
    {
      toolbar_draw(w, &l);
    }
}

/* The state the text is in: the last undo record's, 0 as opened */

static uint32_t state_id(struct edit_s *e)
{
  return e->nundo > 0 ? e->undo[e->nundo - 1].id : 0;
}

static void undo_free(struct undo_s *u, int n)
{
  int i;

  for (i = 0; i < n; i++)
    {
      free(u[i].s);
    }
}

static void undo_push(struct desk_win_s *w, struct undo_s *stack, int *n,
                      bool ins, int l, int c, char *s, int len, uint32_t id)
{
  struct undo_s *u;

  if (*n == UNDO_MAX)
    {
      free(stack[0].s);
      memmove(&stack[0], &stack[1], (UNDO_MAX - 1) * sizeof(*stack));
      (*n)--;
    }

  u = &stack[(*n)++];
  u->ins = ins;
  u->l = l;
  u->c = c;
  u->s = s;
  u->n = len;
  u->id = id;
}

/* Record an edit for undo: typing grows the last record */

static void record(struct desk_win_s *w, bool ins, int l, int c,
                   const char *s, int n, bool typing)
{
  struct edit_s *e = w->priv;
  struct undo_s *u = e->nundo > 0 ? &e->undo[e->nundo - 1] : NULL;
  bool redraw = e->nundo == 0 || e->nredo > 0;   /* Undo, Redo change */
  char *copy;

  undo_free(e->redo, e->nredo);
  e->nredo = 0;

  if (typing && e->typing && u != NULL && u->n < 200 && n == 1 &&
      s[0] != '\n')
    {
      int el;
      int ec;

      if (u->ins && ins)
        {
          text_end(u->s, u->n, u->l, u->c, &el, &ec);
          if (el == l && ec == c && (copy = realloc(u->s, u->n + 2)))
            {
              copy[u->n++] = s[0];
              copy[u->n] = '\0';
              u->s = copy;
              u->id = ++e->ids;
              return;
            }
        }
      else if (!u->ins && !ins && u->l == l && memchr(u->s, '\n', u->n) ==
               NULL && (copy = realloc(u->s, u->n + 2)))
        {
          u->s = copy;
          if (u->c == c + 1)                   /* Backspace */
            {
              memmove(copy + 1, copy, u->n + 1);
              copy[0] = s[0];
              u->n++;
              u->c = c;
              u->id = ++e->ids;
              return;
            }
          else if (u->c == c)                  /* Delete */
            {
              copy[u->n++] = s[0];
              copy[u->n] = '\0';
              u->id = ++e->ids;
              return;
            }
        }
    }

  copy = malloc(n + 1);
  if (copy != NULL)
    {
      memcpy(copy, s, n);
      copy[n] = '\0';
      undo_push(w, e->undo, &e->nundo, ins, l, c, copy, n, ++e->ids);
    }

  e->typing = typing;
  if (redraw)
    {
      struct lay_s lay;

      layout(w, &lay);
      toolbar_draw(w, &lay);
    }
}

/* The edits themselves (not recorded) */

static int raw_insert(struct desk_win_s *w, int l, int c, const char *s,
                      int n, int *el, int *ec)
{
  struct edit_s *e = w->priv;
  int oldnl = e->nl;

  if (buf_insert(e, l, c, s, n, el, ec) < 0)
    {
      set_msg(e, true, "Out of memory", NULL);
      return -ENOMEM;
    }

  changed(w, l, 1, 1 + e->nl - oldnl, oldnl);
  return 0;
}

static char *raw_delete(struct desk_win_s *w, int l0, int c0, int l1,
                        int c1, int *n)
{
  struct edit_s *e = w->priv;
  int oldnl = e->nl;
  char *s = buf_take(e, l0, c0, l1, c1, n, true);

  if (s == NULL)
    {
      set_msg(e, true, "Out of memory", NULL);
      return NULL;
    }

  changed(w, l0, l1 - l0 + 1, 1, oldnl);
  return s;
}

static void cursor_to(struct edit_s *e, int l, int c)
{
  e->cl = l;
  e->cc = c;
  e->want = vcol(&e->ln[l], c);
  e->blink = true;
}

static void sel_none(struct edit_s *e)
{
  if (e->sel)
    {
      int l0;
      int c0;
      int l1;
      int c1;

      sel_range(e, &l0, &c0, &l1, &c1);
      dirty(e, l0, l1);
      e->sel = false;
    }
}

static bool delete_sel(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;
  int l0;
  int c0;
  int l1;
  int c1;
  int n;
  char *s;

  if (!e->sel)
    {
      return false;
    }

  sel_range(e, &l0, &c0, &l1, &c1);
  e->sel = false;
  dirty(e, l0, l1);
  s = raw_delete(w, l0, c0, l1, c1, &n);
  if (s != NULL)
    {
      record(w, false, l0, c0, s, n, false);
      free(s);
    }

  cursor_to(e, l0, c0);
  return true;
}

static void ed_insert(struct desk_win_s *w, const char *s, int n,
                      bool typing)
{
  struct edit_s *e = w->priv;
  int l;
  int c;
  int el;
  int ec;

  if (delete_sel(w))
    {
      typing = false;
    }

  l = e->cl;
  c = e->cc;
  if (raw_insert(w, l, c, s, n, &el, &ec) == 0)
    {
      record(w, true, l, c, s, n, typing);
      dirty(e, e->cl, e->cl);
      cursor_to(e, el, ec);
    }
}

static void ed_delete(struct desk_win_s *w, int l0, int c0, int l1, int c1,
                      bool typing)
{
  struct edit_s *e = w->priv;
  char *s;
  int n;

  s = raw_delete(w, l0, c0, l1, c1, &n);
  if (s != NULL)
    {
      record(w, false, l0, c0, s, n, typing);
      free(s);
      cursor_to(e, l0, c0);
    }
}

static void ed_undo(struct desk_win_s *w, bool redo)
{
  struct edit_s *e = w->priv;
  struct undo_s *from = redo ? e->redo : e->undo;
  int *nfrom = redo ? &e->nredo : &e->nundo;
  struct undo_s *to = redo ? e->undo : e->redo;
  int *nto = redo ? &e->nundo : &e->nredo;
  struct undo_s u;
  struct lay_s l;
  int el;
  int ec;
  int n;

  if (*nfrom == 0)
    {
      return;
    }

  sel_none(e);
  u = from[--(*nfrom)];
  text_end(u.s, u.n, u.l, u.c, &el, &ec);
  if (u.ins == redo)
    {
      /* Insert it again */

      if (raw_insert(w, u.l, u.c, u.s, u.n, &el, &ec) == 0)
        {
          cursor_to(e, el, ec);
        }
    }
  else
    {
      char *s = raw_delete(w, u.l, u.c, el, ec, &n);

      free(s);
      cursor_to(e, u.l, u.c);
    }

  undo_push(w, to, nto, u.ins, u.l, u.c, u.s, u.n, u.id);
  e->typing = false;

  /* Back where it was saved? */

  e->modified = state_id(e) != e->savedid;

  layout(w, &l);
  toolbar_draw(w, &l);
}

static void copy_sel(struct edit_s *e)
{
  int l0;
  int c0;
  int l1;
  int c1;
  int n;
  char *s;

  if (!e->sel)
    {
      return;
    }

  sel_range(e, &l0, &c0, &l1, &c1);
  s = buf_take(e, l0, c0, l1, c1, &n, false);
  if (s != NULL)
    {
      desk_clip_set(s, n);
      free(s);
    }
}

/****************************************************************************
 * Private Functions: files, questions
 ****************************************************************************/

static void do_save(struct desk_win_s *w, const char *path)
{
  struct edit_s *e = w->priv;
  struct lay_s l;
  int ret;

  ret = ed_save(e, path);
  if (ret < 0)
    {
      snprintf(e->msg, sizeof(e->msg), "Cannot save: %s", strerror(-ret));
      e->msgerr = true;
      e->closeafter = false;
      return;
    }

  if (path != e->path)
    {
      strlcpy(e->path, path, sizeof(e->path));
    }

  e->modified = false;
  e->savedid = state_id(e);
  if (e->lang != lang_of(path))
    {
      e->lang = lang_of(path);
      syn_update(e, 0, e->nl);
      e->full = true;
    }

  set_msg(e, false, "Saved", NULL);
  layout(w, &l);
  toolbar_draw(w, &l);
  if (e->closeafter)
    {
      e->modified = false;
      desk_win_close(w);
    }
}

static void ask(struct desk_win_s *w, int prompt)
{
  struct edit_s *e = w->priv;
  struct lay_s l;

  e->prompt = prompt;
  e->zs.hot = 0;
  e->zs.pressed = 0;
  if (prompt == P_SAVEAS)
    {
      const char *home = getenv("HOME");
      char *p = malloc(PATH_MAX);

      if (p != NULL)
        {
          if (home == NULL || strcmp(home, "/") == 0)
            {
              home = access("/mnt/sd0", F_OK) == 0 ? "/mnt/sd0" : "/tmp";
            }

          snprintf(p, PATH_MAX, "%s/Untitled.txt", home);
          ui_field_set(&e->field, p);
          free(p);
        }
    }

  layout(w, &l);
  dirty(e, e->cl, e->cl);                /* The cursor goes */
  refresh(w, false);
  card_draw(w, &l);
}

static void answer(struct desk_win_s *w, int id)
{
  struct edit_s *e = w->priv;
  int prompt = e->prompt;

  e->prompt = P_NONE;
  ui_zones_clear(&e->zs, ZG_CARD);
  e->full = true;
  switch (id)
    {
      case Z_OK:
        if (prompt == P_SAVEAS)
          {
            if (e->field.len == 0)
              {
                e->prompt = prompt;
                e->full = false;
                return;
              }

            do_save(w, e->field.text);
          }
        else if (e->path[0] == '\0')
          {
            e->closeafter = true;
            ask(w, P_SAVEAS);
            return;
          }
        else
          {
            e->closeafter = true;
            do_save(w, e->path);
          }
        break;

      case Z_DISCARD:
        desk_win_close(w);
        return;

      default:
        e->closeafter = false;
        break;
    }

  if (!w->closing)
    {
      refresh(w, true);
    }
}

static void save(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;

  if (e->path[0] == '\0')
    {
      ask(w, P_SAVEAS);
    }
  else
    {
      do_save(w, e->path);
      refresh(w, false);
    }
}

/****************************************************************************
 * Private Functions: events
 ****************************************************************************/

/* Moving the cursor; with shift, the selection goes along */

static void move(struct desk_win_s *w, int l, int c, bool shift)
{
  struct edit_s *e = w->priv;

  if (shift)
    {
      if (!e->sel)
        {
          e->sel = true;
          e->al = e->cl;
          e->ac = e->cc;
        }

      dirty(e, e->cl, l);
    }
  else
    {
      sel_none(e);
    }

  dirty(e, e->cl, e->cl);
  dirty(e, l, l);
  e->cl = l;
  e->cc = c;
  e->blink = true;
  if (e->sel && e->al == e->cl && e->ac == e->cc)
    {
      e->sel = false;
    }
}

static void word_move(struct desk_win_s *w, int dir, bool shift)
{
  struct edit_s *e = w->priv;
  struct line_s *ln = &e->ln[e->cl];
  int c = e->cc;

  if (dir < 0)
    {
      if (c == 0 && e->cl > 0)
        {
          move(w, e->cl - 1, e->ln[e->cl - 1].len, shift);
          return;
        }

      while (c > 0 && !is_word(ln->s[c - 1]))
        {
          c--;
        }

      while (c > 0 && is_word(ln->s[c - 1]))
        {
          c--;
        }
    }
  else
    {
      if (c == ln->len && e->cl < e->nl - 1)
        {
          move(w, e->cl + 1, 0, shift);
          return;
        }

      while (c < ln->len && !is_word(ln->s[c]))
        {
          c++;
        }

      while (c < ln->len && is_word(ln->s[c]))
        {
          c++;
        }
    }

  move(w, e->cl, c, shift);
  e->want = vcol(&e->ln[e->cl], e->cc);
}

static void edit_keyone(struct desk_win_s *w, int k)
{
  struct edit_s *e = w->priv;
  bool shift = (k & KEY_SHIFT) != 0;
  bool ctrl = (k & KEY_CTRL) != 0;
  int key = k & KEY_MASK;
  struct lay_s l;
  int page;
  int want = e->want;

  layout(w, &l);
  page = l.full > 1 ? l.full - 1 : 1;

  if (key == '\n' && e->lastcr)
    {
      e->lastcr = false;
      return;
    }

  e->lastcr = key == '\r';
  if (key != KEY_CTL('s'))
    {
      e->msg[0] = '\0';
    }

  switch (key)
    {
      case KEY_LEFT:
        if (ctrl)
          {
            word_move(w, -1, shift);
          }
        else if (e->sel && !shift)
          {
            int l0;
            int c0;
            int l1;
            int c1;

            sel_range(e, &l0, &c0, &l1, &c1);
            move(w, l0, c0, false);
          }
        else if (e->cc > 0)
          {
            move(w, e->cl, e->cc - 1, shift);
          }
        else if (e->cl > 0)
          {
            move(w, e->cl - 1, e->ln[e->cl - 1].len, shift);
          }

        e->want = vcol(&e->ln[e->cl], e->cc);
        break;

      case KEY_RIGHT:
        if (ctrl)
          {
            word_move(w, 1, shift);
          }
        else if (e->sel && !shift)
          {
            int l0;
            int c0;
            int l1;
            int c1;

            sel_range(e, &l0, &c0, &l1, &c1);
            move(w, l1, c1, false);
          }
        else if (e->cc < e->ln[e->cl].len)
          {
            move(w, e->cl, e->cc + 1, shift);
          }
        else if (e->cl < e->nl - 1)
          {
            move(w, e->cl + 1, 0, shift);
          }

        e->want = vcol(&e->ln[e->cl], e->cc);
        break;

      case KEY_UP:
      case KEY_PGUP:
        {
          int to = e->cl - (key == KEY_UP ? 1 : page);

          if (to < 0)
            {
              move(w, 0, 0, shift);
            }
          else
            {
              move(w, to, col_at(&e->ln[to], want), shift);
            }

          if (key == KEY_PGUP)
            {
              scroll_to(w, &l, e->top - page);
            }

          e->want = to < 0 ? 0 : want;
        }
        break;

      case KEY_DOWN:
      case KEY_PGDN:
        {
          int to = e->cl + (key == KEY_DOWN ? 1 : page);

          if (to > e->nl - 1)
            {
              move(w, e->nl - 1, e->ln[e->nl - 1].len, shift);
              e->want = vcol(&e->ln[e->cl], e->cc);
            }
          else
            {
              move(w, to, col_at(&e->ln[to], want), shift);
              e->want = want;
            }

          if (key == KEY_PGDN && e->top + page <= e->nl - l.full)
            {
              scroll_to(w, &l, e->top + page);
            }
        }
        break;

      case KEY_HOME:
        if (ctrl)
          {
            move(w, 0, 0, shift);
          }
        else
          {
            /* The indent first, then the start */

            struct line_s *ln = &e->ln[e->cl];
            int c = 0;

            while (c < ln->len && (ln->s[c] == ' ' || ln->s[c] == '\t'))
              {
                c++;
              }

            move(w, e->cl, e->cc == c ? 0 : c, shift);
          }

        e->want = vcol(&e->ln[e->cl], e->cc);
        break;

      case KEY_END:
        if (ctrl)
          {
            move(w, e->nl - 1, e->ln[e->nl - 1].len, shift);
          }
        else
          {
            move(w, e->cl, e->ln[e->cl].len, shift);
          }

        e->want = vcol(&e->ln[e->cl], e->cc);
        break;

      case 0x7f:
      case 0x08:
        if (delete_sel(w))
          {
            break;
          }

        if (e->cc > 0)
          {
            ed_delete(w, e->cl, e->cc - 1, e->cl, e->cc, true);
          }
        else if (e->cl > 0)
          {
            ed_delete(w, e->cl - 1, e->ln[e->cl - 1].len, e->cl, 0, false);
          }
        break;

      case KEY_DEL:
        if (delete_sel(w))
          {
            break;
          }

        if (e->cc < e->ln[e->cl].len)
          {
            ed_delete(w, e->cl, e->cc, e->cl, e->cc + 1, true);
          }
        else if (e->cl < e->nl - 1)
          {
            ed_delete(w, e->cl, e->cc, e->cl + 1, 0, false);
          }
        break;

      case '\r':
      case '\n':
        {
          /* A new line with the indent of this one */

          struct line_s *ln = &e->ln[e->cl];
          char *s = malloc(ln->len + 2);
          int n = 1;

          if (s == NULL)
            {
              break;
            }

          s[0] = '\n';
          while (n - 1 < ln->len && n - 1 < e->cc &&
                 (ln->s[n - 1] == ' ' || ln->s[n - 1] == '\t'))
            {
              s[n] = ln->s[n - 1];
              n++;
            }

          ed_insert(w, s, n, false);
          free(s);
        }
        break;

      case '\t':
        ed_insert(w, "\t", 1, true);
        break;

      case KEY_ESC:
        sel_none(e);
        break;

      case KEY_CTL('a'):
        e->sel = true;
        e->al = 0;
        e->ac = 0;
        e->cl = e->nl - 1;
        e->cc = e->ln[e->cl].len;
        e->full = true;
        break;

      case KEY_CTL('c'):
        copy_sel(e);
        break;

      case KEY_CTL('x'):
        copy_sel(e);
        delete_sel(w);
        break;

      case KEY_CTL('v'):
        {
          size_t n;
          const char *s = desk_clip_get(&n);

          if (s != NULL && n > 0)
            {
              ed_insert(w, s, n, false);
            }
        }
        break;

      case KEY_CTL('s'):
        save(w);
        return;

      case KEY_CTL('z'):
        ed_undo(w, false);
        break;

      case KEY_CTL('y'):
        ed_undo(w, true);
        break;

      case KEY_CTL('n'):
        desk_editor_open(NULL);
        return;

      default:
        if (key >= 0x20 && key <= 0xff && key != 0x7f && !ctrl)
          {
            char c = (char)key;

            ed_insert(w, &c, 1, true);
          }
        break;
    }
}

static void edit_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct edit_s *e = w->priv;
  int keys[16];
  int nk = ui_keys(&e->keys, ch, n, keys, 16);
  int i;

  for (i = 0; i < nk && !w->closing; i++)
    {
      if (e->prompt != P_NONE)
        {
          if (keys[i] == '\r' || keys[i] == '\n')
            {
              answer(w, Z_OK);
            }
          else if (keys[i] == KEY_ESC)
            {
              answer(w, Z_CANCEL);
            }
          else if (e->prompt == P_SAVEAS && ui_field_key(&e->field, keys[i]))
            {
              struct lay_s l;

              layout(w, &l);
              card_draw(w, &l);
            }

          continue;
        }

      edit_keyone(w, keys[i]);
      if (!w->closing && e->prompt == P_NONE)
        {
          refresh(w, true);
        }
    }
}

/* The text position under (x, y), frame coordinates */

static void pos_at(struct desk_win_s *w, struct lay_s *l, int x, int y,
                   int *pl, int *pc)
{
  struct edit_s *e = w->priv;
  int row = y < l->ty ? -1 - (l->ty - y) / LH : (y - l->ty) / LH;
  int line = e->top + row;
  int vc = (x - l->x0 + CW / 2) / CW + e->left;

  line = line < 0 ? 0 : line >= e->nl ? e->nl - 1 : line;
  vc = x < l->x0 ? e->left : vc;
  *pl = line;
  *pc = col_at(&e->ln[line], vc < 0 ? 0 : vc);
}

static void select_word(struct edit_s *e)
{
  struct line_s *ln = &e->ln[e->cl];
  int a = e->cc;
  int b = e->cc;

  while (a > 0 && is_word(ln->s[a - 1]))
    {
      a--;
    }

  while (b < ln->len && is_word(ln->s[b]))
    {
      b++;
    }

  if (b > a)
    {
      e->sel = true;
      e->al = e->cl;
      e->ac = a;
      e->cc = b;
      dirty(e, e->cl, e->cl);
    }
}

static void set_hot(struct desk_win_s *w, int id)
{
  struct edit_s *e = w->priv;
  int old = e->zs.hot;
  int g0 = ui_zone_group(&e->zs, old);
  int g1 = ui_zone_group(&e->zs, id);
  struct lay_s l;

  if (id == old)
    {
      return;
    }

  e->zs.hot = id;
  layout(w, &l);
  if (g0 == ZG_TOOL || g1 == ZG_TOOL)
    {
      toolbar_draw(w, &l);
    }

  if (g0 == ZG_CARD || g1 == ZG_CARD)
    {
      card_draw(w, &l);
    }
}

static void edit_mouse(struct desk_win_s *w, int x, int cy, uint8_t buttons)
{
  struct edit_s *e = w->priv;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(e->buttons & NX_MOUSE_LEFTBUTTON);
  bool release = !(buttons & NX_MOUSE_LEFTBUTTON) &&
                 (e->buttons & NX_MOUSE_LEFTBUTTON);
  struct ui_zone_s *z;
  struct lay_s l;
  int y = cy + TITLE_H;
  int pl;
  int pc;

  e->buttons = buttons;
  layout(w, &l);

  if (x < 0 && cy < 0 && buttons == 0)
    {
      set_hot(w, 0);
      e->zs.pressed = 0;
      e->drag = D_NONE;
      return;
    }

  if (e->drag == D_THUMB)
    {
      if (buttons & NX_MOUSE_LEFTBUTTON)
        {
          int total = e->nl + l.full - 1;
          int th = l.th * l.full / total;

          th = th < 24 ? 24 : th;
          if (l.th > th)
            {
              scroll_to(w, &l, (y - e->grab - l.ty) * (e->nl - 1) /
                        (l.th - th));
              refresh(w, false);
            }
        }
      else
        {
          e->drag = D_NONE;
          scrollbar_draw(w, &l);
          ops_draw((NXWINDOW)w->hwnd, &g_o);
        }

      return;
    }

  if (e->drag == D_TEXT)
    {
      if (buttons & NX_MOUSE_LEFTBUTTON)
        {
          int oldtop = e->top;

          /* Above or below the text: it scrolls */

          if (y < l.ty && e->top > 0)
            {
              scroll_to(w, &l, e->top - 1);
            }
          else if (y >= l.ty + l.th && e->top < e->nl - l.full)
            {
              scroll_to(w, &l, e->top + 1);
            }

          pos_at(w, &l, x, y, &pl, &pc);
          if (pl != e->cl || pc != e->cc || oldtop != e->top)
            {
              move(w, pl, pc, true);
              e->want = vcol(&e->ln[e->cl], e->cc);
              refresh(w, true);
            }
        }
      else
        {
          e->drag = D_NONE;
        }

      return;
    }

  z = ui_zone_at(&e->zs, x, y);
  if (e->prompt != P_NONE && z != NULL && z->group != ZG_CARD)
    {
      z = NULL;
    }

  if (!(buttons & NX_MOUSE_LEFTBUTTON) || press)
    {
      set_hot(w, z != NULL && z->enabled ? z->id : 0);
    }

  if (press)
    {
      if (z != NULL)
        {
          if (z->enabled)
            {
              e->zs.pressed = z->id;
              set_hot(w, 0);
              set_hot(w, z->id);
            }
        }
      else if (e->prompt != P_NONE)
        {
          return;
        }
      else if (x >= l.xr && y >= l.ty && y < l.ty + l.th)
        {
          int total = e->nl + l.full - 1;
          int th;
          int ty;

          if (e->nl <= l.full)
            {
              return;
            }

          th = l.th * l.full / total;
          th = th < 24 ? 24 : th;
          ty = l.ty + (l.th - th) * e->top / (e->nl - 1);
          if (y >= ty && y < ty + th)
            {
              e->drag = D_THUMB;
              e->grab = y - ty;
              scrollbar_draw(w, &l);
              ops_draw((NXWINDOW)w->hwnd, &g_o);
            }
          else
            {
              scroll_to(w, &l, e->top + (y < ty ? -l.full : l.full));
              refresh(w, false);
            }
        }
      else if (y >= l.ty && y < l.ty + l.th)
        {
          uint32_t now = desk_ms();

          pos_at(w, &l, x, y, &pl, &pc);
          e->clicks = pl == e->clickl && pc == e->clickc &&
                      now - e->clickms < DBLCLICK_MS ? e->clicks + 1 : 1;
          e->clickms = now;
          e->clickl = pl;
          e->clickc = pc;
          move(w, pl, pc, false);
          e->want = vcol(&e->ln[e->cl], e->cc);
          if (e->clicks == 2)
            {
              select_word(e);
            }
          else if (e->clicks >= 3)
            {
              e->sel = true;
              e->al = pl;
              e->ac = 0;
              if (pl < e->nl - 1)
                {
                  e->cl = pl + 1;
                  e->cc = 0;
                }
              else
                {
                  e->cc = e->ln[pl].len;
                }

              dirty(e, pl, e->cl);
            }
          else
            {
              e->drag = D_TEXT;
            }

          refresh(w, true);
        }
    }

  if (release && e->zs.pressed != 0)
    {
      int id = e->zs.pressed;

      e->zs.pressed = 0;
      if (z != NULL && z->id == id)
        {
          set_hot(w, 0);
          switch (id)
            {
              case Z_NEW:
                desk_editor_open(NULL);
                return;

              case Z_SAVE:
                save(w);
                return;

              case Z_UNDO:
              case Z_REDO:
                ed_undo(w, id == Z_REDO);
                refresh(w, true);
                break;

              default:
                answer(w, id);
                return;
            }

          set_hot(w, id);
        }
      else
        {
          set_hot(w, z != NULL && z->enabled ? z->id : 0);
          toolbar_draw(w, &l);
          card_draw(w, &l);
        }
    }
}

/* The cursor blinks; it goes when the window is not active */

static void edit_tick(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;
  bool blink = w->active && (!e->blink || desk_frozen()) &&
               e->prompt == P_NONE;

  if (blink != e->blink)
    {
      e->blink = blink;
      dirty(e, e->cl, e->cl);
      refresh(w, false);
    }
}

static void edit_resize(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;
  struct lay_s l;

  layout(w, &l);
  if (e->top > e->nl - 1)
    {
      e->top = e->nl - 1;
    }

  edit_draw(w);
}

static bool edit_canclose(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;

  if (e == NULL || !e->modified)
    {
      return true;
    }

  if (e->prompt != P_CLOSE)
    {
      desk_win_activate(w);
      ask(w, P_CLOSE);
    }

  return false;
}

static void edit_close(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;

  if (e == NULL)
    {
      return;
    }

  ln_free(e);
  undo_free(e->undo, e->nundo);
  undo_free(e->redo, e->nredo);
  e->nundo = 0;
  e->nredo = 0;
  free(e->cls);
  e->cls = NULL;
}

static void edit_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  int i;

  icon_tile(o, x, y, s, RGB(0xff, 0xd2, 0x5a), RGB(0xf0, 0x96, 0x1e), a);
  ops_rrect(o, x + s / 5, y + s / 6, s * 3 / 5, s * 2 / 3, LUT_R3, 3,
            C_WHITE, a);
  for (i = 0; i < 4; i++)
    {
      ops_blend(o, x + s / 5 + 5, y + s / 6 + 7 + i * s / 8,
                s * 3 / 5 - 10 - (i == 3 ? s / 6 : 0), 2,
                RGB(0x80, 0x80, 0x88), a);
    }
}

static struct desk_win_s *edit_open(void)
{
  const char *path = g_openpath;
  struct desk_win_s *w;
  struct edit_s *e;
  int ret = 0;

  e = calloc(1, sizeof(*e));
  if (e == NULL)
    {
      return NULL;
    }

  if (path != NULL)
    {
      ret = ed_load(e, path);
      if (ret == -ENOENT)
        {
          /* A new file of that name */

          ln_free(e);
          strlcpy(e->path, path, sizeof(e->path));
          e->lang = lang_of(path);
          ret = 0;
        }
      else if (ret < 0)
        {
          ln_free(e);
          snprintf(e->msg, sizeof(e->msg), "Cannot open %s: %s", path,
                   strerror(-ret));
          e->msgerr = true;
        }
    }

  if (e->nl == 0 && ln_make(e, 0, 1) < 0)
    {
      free(e);
      return NULL;
    }

  w = desk_win_open(APP_EDITOR, "Text Editor", EDIT_W, EDIT_H);
  if (w == NULL)
    {
      ln_free(e);
      free(e);
      return NULL;
    }

  w->priv  = e;
  e->blink = true;
  e->dlo   = 1;
  e->dhi   = 0;
  snprintf(e->title, sizeof(e->title), "%s", base_name(e));
  snprintf(w->title, sizeof(w->title), "%s", base_name(e));
  edit_draw(w);
  return w;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int desk_editor_open(const char *path)
{
  struct desk_win_s *w;

  /* Open already: that window comes forward */

  for (w = g_desk.wins; path != NULL && w != NULL; w = w->next)
    {
      struct edit_s *e = w->priv;

      if (w->app == APP_EDITOR && !w->closing && e != NULL &&
          strcmp(e->path, path) == 0)
        {
          desk_win_restore(w);
          return 0;
        }
    }

  g_openpath = path;
  desk_launch(APP_EDITOR);
  g_openpath = NULL;
  return 0;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_editor =
{
  .name     = "Text Editor",
  .icon     = edit_icon,
  .open     = edit_open,
  .draw     = edit_draw,
  .resize   = edit_resize,
  .tick     = edit_tick,
  .mouse    = edit_mouse,
  .key      = edit_key,
  .close    = edit_close,
  .canclose = edit_canclose,
};
