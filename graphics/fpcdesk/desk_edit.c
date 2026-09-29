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
 *
 * The tool bar, the scroll bar, the status bar and the questions (save
 * before closing?, Save As: the kit's file dialog) are the widget kit's
 * (desk_kit.h); the text is a custom widget of it that takes every key,
 * drawn here.
 *
 * Python files: colours (keywords, builtins, strings, triple-quoted ones
 * across lines, comments, decorators, numbers), Enter after a ':' indents
 * four more, Tab puts spaces to the next four, Backspace in an indent takes
 * four back.
 *
 * Another application can have an editor with more (struct edit_ext_s,
 * desk.h: the Python IDE, desk_ide.c): buttons at the tool bar's start,
 * widgets below the text, their events and keys, a word when a file has
 * been saved.
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

#include "desk_kit.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define EDIT_W         680
#define EDIT_H         460

#define NWG            40                /* The kit's widgets (with an
                                          * extension's) */
#define LH             20                /* A line */
#define CW             9                 /* A character (Plex Mono 15) */
#define TABW           4
#define MAX_FILE       (4 * 1024 * 1024)
#define UNDO_MAX       200
#define DBLCLICK_MS    400

#define C_EDIT_BG      THEMED(RGB(0x1c, 0x1c, 0x20), RGB(0xff, 0xff, 0xff))
#define C_LINENO       THEMED(RGB(0x62, 0x62, 0x6a), RGB(0xa8, 0xa8, 0xb0))
#define C_CARET        THEMED(RGB(0xf2, 0xf2, 0xf4), RGB(0x1d, 0x1d, 0x1f))

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
#define LANG_PYTHON    3

#define PY_INDENT      4

/* The widgets */

#define E_NEW          1
#define E_SAVE         2
#define E_UNDO         3
#define E_REDO         4
#define E_PATH         5
#define E_TEXT         6
#define E_SCROLL       7
#define E_LEFT         8                 /* The status bar */
#define E_RIGHT        9

/* Questions, in dialogs (their ids) */

#define P_NONE         0
#define P_CLOSE        1                 /* Unsaved changes: save first? */
#define P_SAVEAS       2                 /* A file name */

/* Mouse drags */

#define D_NONE         0
#define D_TEXT         1

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
  uint32_t kind;                         /* The extension's (0: none) */
  const struct edit_ext_s *ext;          /* An application's additions */
  void *extpriv;
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
  char title[40];
  uint8_t *cls;                          /* Syntax classes of a line */
  int clscap;

  bool closeafter;                       /* Save, then close */
  bool lastcr;

  /* The tool bar, the scroll bar, the status bar, the dialogs: the
   * widget kit's; the text is a custom widget of it, drawn here
   */

  struct kit_s kit;
  struct kit_widget_s wg[NWG];
  char lefttext[96];                     /* The status bar's texts */
  char right[64];
  char pathlabel[PATH_MAX];

  uint8_t buttons;
  int drag;
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
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct desk_ops_s g_o;
static const char *g_openpath;

/* The syntax colours: dark theme, light theme (the first is the text's) */

static const uint16_t g_syncol[2][7] =
{
  {
    0,
    RGB(0xff, 0x8f, 0x70),               /* Keywords: orange */
    RGB(0x70, 0xc4, 0xff),               /* Types: blue */
    RGB(0xa4, 0xde, 0x7c),               /* Strings: green */
    RGB(0x7c, 0x82, 0x8c),               /* Comments: grey */
    RGB(0xd4, 0xa4, 0xff),               /* Preprocessor, $vars: purple */
    RGB(0xff, 0xd0, 0x60),               /* Numbers: yellow */
  },
  {
    0,
    RGB(0xc8, 0x3c, 0x1c),
    RGB(0x0b, 0x64, 0xc0),
    RGB(0x24, 0x80, 0x2c),
    RGB(0x8a, 0x8f, 0x98),
    RGB(0x8a, 0x3a, 0xb8),
    RGB(0xa8, 0x62, 0x00),
  },
};

#define SYNCOL(k)      ((k) == 0 ? C_TEXT : g_syncol[g_theme.dark ? 0 : 1][k])

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

static const char * const g_pykw[] =
{
  "False", "None", "True", "and", "as", "assert", "async", "await",
  "break", "class", "continue", "def", "del", "elif", "else", "except",
  "finally", "for", "from", "global", "if", "import", "in", "is", "lambda",
  "nonlocal", "not", "or", "pass", "raise", "return", "try", "while",
  "with", "yield", NULL
};

static const char * const g_pybi[] =
{
  "self", "print", "len", "range", "int", "str", "float", "bool", "list",
  "dict", "tuple", "set", "bytes", "bytearray", "object", "type", "open",
  "input", "isinstance", "enumerate", "zip", "map", "filter", "sorted",
  "min", "max", "sum", "abs", "round", "super", "iter", "next", "hex",
  "ord", "chr", "repr", "hasattr", "getattr", "setattr", "Exception",
  "ValueError", "TypeError", "KeyError", "IndexError", "OSError",
  "RuntimeError", "StopIteration", "KeyboardInterrupt", NULL
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

/* Python: st is the triple quote a line starts in (1 \'\'\', 2 """, 0 none);
 * the one the next starts in is returned
 */

static int syn_python(struct edit_s *e, const char *s, int n, int st,
                      uint8_t *cls)
{
  int i = 0;
  int j;
  bool first = true;

#define PMARK(a, b, c) do { if (cls) memset(cls + (a), (c), (b) - (a)); } \
                       while (0)

  while (i < n)
    {
      uint8_t c = s[i];

      if (st)
        {
          char q = st == 1 ? '\'' : '"';

          for (j = i; j + 2 < n && !(s[j] == q && s[j + 1] == q &&
                                     s[j + 2] == q); j++)
            {
              j += s[j] == '\\';
            }

          if (j + 2 < n)
            {
              PMARK(i, j + 3, S_STRING);
              i  = j + 3;
              st = 0;
            }
          else
            {
              PMARK(i, n, S_STRING);
              i = n;
            }

          continue;
        }

      if (c == ' ' || c == '\t')
        {
          i++;
          continue;
        }

      if (c == '#')
        {
          PMARK(i, n, S_COMMENT);
          break;
        }

      if (c == '@' && first)
        {
          for (j = i + 1; j < n && (is_word(s[j]) || s[j] == '.'); j++)
            {
            }

          PMARK(i, j, S_PREPROC);
          i = j;
          first = false;
          continue;
        }

      first = false;

      /* Strings, with a prefix (r, b, f, u and pairs of them) */

      j = i;
      while (j < n && j - i < 2 && strchr("rRbBfFuUtT", s[j]) != NULL)
        {
          j++;
        }

      if (j < n && (s[j] == '"' || s[j] == '\'') &&
          (j == i || i == 0 || !is_word(s[i - 1])))
        {
          char q = s[j];
          int k;

          if (j + 2 < n && s[j + 1] == q && s[j + 2] == q)
            {
              st = q == '\'' ? 1 : 2;
              for (k = j + 3; k + 2 < n && !(s[k] == q && s[k + 1] == q &&
                                             s[k + 2] == q); k++)
                {
                  k += s[k] == '\\';
                }

              if (k + 2 < n)
                {
                  PMARK(i, k + 3, S_STRING);
                  i  = k + 3;
                  st = 0;
                }
              else
                {
                  PMARK(i, n, S_STRING);
                  i = n;
                }

              continue;
            }

          for (k = j + 1; k < n && s[k] != q; k++)
            {
              k += s[k] == '\\';
            }

          k += k < n;
          PMARK(i, k > n ? n : k, S_STRING);
          i = k;
          continue;
        }

      if (isdigit(c) && (i == 0 || !is_word(s[i - 1])))
        {
          for (j = i; j < n && (is_word(s[j]) || s[j] == '.'); j++)
            {
            }

          PMARK(i, j, S_NUMBER);
          i = j;
          continue;
        }

      if (isalpha(c) || c == '_')
        {
          for (j = i; j < n && is_word(s[j]); j++)
            {
            }

          if (in_words(s + i, j - i, g_pykw))
            {
              PMARK(i, j, S_KEYWORD);
            }
          else if (in_words(s + i, j - i, g_pybi))
            {
              PMARK(i, j, S_TYPE);
            }

          i = j;
          continue;
        }

      i++;
    }

#undef PMARK

  return st;
}

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

  if (e->lang == LANG_PYTHON)
    {
      return syn_python(e, s, n, st, cls);
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

  if (e->lang != LANG_C && e->lang != LANG_PYTHON)
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

  if (ext != NULL && strcasecmp(ext, ".py") == 0)
    {
      return LANG_PYTHON;
    }

  if ((ext != NULL && (strcasecmp(ext, ".sh") == 0 ||
      strcasecmp(ext, ".mk") == 0 ||
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

  struct kit_widget_s *text = kit_get(&e->kit, E_TEXT);

  /* The text area is the kit's custom widget (after kit_layout()) */

  l->cw   = w->csize.w;
  l->ty   = text->y;
  l->th   = text->h;
  l->rows = (l->th + LH - 1) / LH;
  l->full = l->th / LH;
  l->full = l->full < 1 ? 1 : l->full;
  l->gw   = (d < 3 ? 3 : d) * CW + 20;
  l->x0   = l->gw + 6;
  l->xr   = text->x + text->w;
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
                            (v1 - v0) * CW, LH, C_ACCENT, THEMED(120, 70));
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
                       SYNCOL(e->cls[k]) : C_TEXT, 255);
            }

          v++;
        }

      /* The cursor */

      if (i == e->cl && e->blink && w->active &&
          !kit_dialog_open(&e->kit))
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

/* The scroll bar, the status bar and the tool bar: what they show, into
 * the kit's widgets (drawn at the kit's next flush, if they changed)
 */

static void scrollbar_sync(struct desk_win_s *w, struct lay_s *l)
{
  struct edit_s *e = w->priv;

  /* The last line can scroll to the top */

  kit_scroll_set(&e->kit, E_SCROLL, e->top, l->full, e->nl + l->full - 1);
}

static const char *lang_name(int lang)
{
  return lang == LANG_C ? "C" : lang == LANG_SHELL ? "Shell" :
         lang == LANG_PYTHON ? "Python" : "Plain Text";
}

static void status_sync(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;
  char right[64];
  char left[96];

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

  if (strcmp(left, e->lefttext) != 0)
    {
      strlcpy(e->lefttext, left, sizeof(e->lefttext));
      kit_invalidate(&e->kit, E_LEFT);
    }

  kit_set_style(&e->kit, E_LEFT, e->msg[0] != '\0' && e->msgerr ?
                KIT_TEXT_ERROR : KIT_TEXT_DIM);
  if (strcmp(right, e->right) != 0)
    {
      strlcpy(e->right, right, sizeof(e->right));
      kit_set_text(&e->kit, E_RIGHT, e->right);
    }
}

static void toolbar_sync(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;
  const char *path = e->path[0] != '\0' ? e->path : "Not saved yet";

  kit_set_style(&e->kit, E_SAVE, e->modified ? KIT_BTN_DEFAULT :
                KIT_BTN_NORMAL);
  kit_enable(&e->kit, E_UNDO, e->nundo > 0);
  kit_enable(&e->kit, E_REDO, e->nredo > 0);
  if (strcmp(path, e->pathlabel) != 0)
    {
      strlcpy(e->pathlabel, path, sizeof(e->pathlabel));
      kit_invalidate(&e->kit, E_PATH);
    }
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
  scrollbar_sync(w, &l);
  status_sync(w);
  toolbar_sync(w);
  e->full = true;
  kit_redraw(&e->kit);                   /* The text: edit_paint() */
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

/* The lines marked, or all of them; the band of the text area drawn,
 * [*y0, *y1) (nothing: *y0 == *y1)
 */

static void lines_draw(struct desk_win_s *w, struct lay_s *l, int *y0,
                       int *y1)
{
  struct edit_s *e = w->priv;
  int i;

  *y0 = l->ty;
  *y1 = l->ty;
  if (e->full)
    {
      text_draw(w, l);
      *y1 = l->ty + l->th;
    }
  else if (e->dlo <= e->dhi)
    {
      int a = e->dlo < e->top ? e->top : e->dlo;
      int b = e->dhi > e->top + l->rows ? e->top + l->rows : e->dhi;

      for (i = a; i <= b; i++)
        {
          line_draw(w, l, i);
        }

      if (a <= b)
        {
          *y0 = l->ty + (a - e->top) * LH;
          *y1 = l->ty + (b - e->top + 1) * LH;
          *y1 = *y1 < l->ty + l->th ? *y1 : l->ty + l->th;
        }
    }

  e->full = false;
  e->dlo = 1;
  e->dhi = 0;
}

static void refresh(struct desk_win_s *w, bool follow)
{
  struct edit_s *e = w->priv;
  struct lay_s l;
  int top = e->top;
  int y0;
  int y1;
  int v;

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

  lines_draw(w, &l, &y0, &y1);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
  if (e->top != top)
    {
      y0 = l.ty;                         /* Scrolled: all of it moved */
      y1 = l.ty + l.th;
    }

  /* The kit's widgets; anything of it over the text drawn (a tooltip, a
   * dialog) again: only there, where it was drawn over (its round corners
   * and shadows blend: over themselves they would come out darker)
   */

  scrollbar_sync(w, &l);
  status_sync(w);
  toolbar_sync(w);
  kit_flush(&e->kit);
  if (y1 > y0)
    {
      kit_touched(&e->kit, 0, y0, l.xr, y1 - y0);
    }

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
      toolbar_sync(w);
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
      toolbar_sync(w);
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
  toolbar_sync(w);
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
  toolbar_sync(w);
  if (e->closeafter)
    {
      e->modified = false;
      desk_win_close(w);
      return;
    }

  if (e->ext != NULL && e->ext->saved != NULL)
    {
      e->ext->saved(w);
    }
}

static void ask(struct desk_win_s *w, int prompt)
{
  static const char * const saving[3] =
  {
    "Save", "Cancel", "Don't Save"
  };

  struct edit_s *e = w->priv;
  char title[96];

  if (prompt == P_CLOSE)
    {
      snprintf(title, sizeof(title), "Save changes to \"%s\"?",
               base_name(e));
      kit_dialog(&e->kit, P_CLOSE, KIT_DLG_CONFIRM, title,
                 "If you don't save, the changes are lost.", saving, 3);
    }
  else
    {
      const char *home = getenv("HOME");
      char *p = malloc(PATH_MAX);

      if (p == NULL)
        {
          return;
        }

      /* In the home directory ($HOME, else CONFIG_LIBC_HOMEDIR); without
       * one, on the microSD, or in /tmp
       */

#ifdef CONFIG_LIBC_HOMEDIR
      if (home == NULL)
        {
          home = CONFIG_LIBC_HOMEDIR;
        }
#endif

      if (home == NULL || strcmp(home, "/") == 0 ||
          access(home, F_OK) != 0)
        {
          home = access("/mnt/sd0", F_OK) == 0 ? "/mnt/sd0" :
                 access("/tmp", F_OK) == 0 ? "/tmp" : "";
        }

      snprintf(p, PATH_MAX, "%s/%s", home, e->lang == LANG_PYTHON ?
               "untitled.py" : "Untitled.txt");
      kit_dialog(&e->kit, P_SAVEAS, KIT_DLG_SAVE, "Save As", p, NULL, 0);
      free(p);
    }

  e->blink = false;                      /* The cursor goes */
  dirty(e, e->cl, e->cl);
  refresh(w, false);
}

/* The dialog's answer (the dialog has gone: the text under it is drawn
 * again by edit_paint())
 */

static void answer(struct desk_win_s *w, int prompt, int ans,
                   const char *path)
{
  struct edit_s *e = w->priv;

  if (prompt == P_SAVEAS)
    {
      if (ans == KIT_ANS_OK && path[0] != '\0')
        {
          do_save(w, path);
        }
      else
        {
          e->closeafter = false;
        }
    }
  else if (ans == KIT_ANS_ALT)
    {
      e->modified = false;
      desk_win_close(w);                 /* Don't Save */
      return;
    }
  else if (ans == KIT_ANS_OK)
    {
      e->closeafter = true;
      if (e->path[0] == '\0')
        {
          ask(w, P_SAVEAS);
          return;
        }

      do_save(w, e->path);
    }

  if (!w->closing)
    {
      e->blink = true;
      dirty(e, e->cl, e->cl);
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

/* Python: whether a line (to column n) opens a block (ends in ':', not
 * in a string or a comment)
 */

static bool py_opens(const char *s, int n)
{
  char q = 0;
  int last = -1;
  int i;

  for (i = 0; i < n; i++)
    {
      char c = s[i];

      if (q != 0)
        {
          if (c == '\\')
            {
              i++;
            }
          else if (c == q)
            {
              q = 0;
            }
        }
      else if (c == '#')
        {
          break;
        }
      else if (c == '"' || c == '\'')
        {
          q = c;
          last = i;
        }
      else if (c != ' ' && c != '\t')
        {
          last = i;
        }
    }

  return last >= 0 && s[last] == ':';
}

/* The cursor is in the line's indent (spaces only before it) */

static bool py_indent_only(struct edit_s *e)
{
  struct line_s *ln = &e->ln[e->cl];
  int i;

  if (e->sel)
    {
      return false;
    }

  for (i = 0; i < e->cc; i++)
    {
      if (ln->s[i] != ' ')
        {
          return false;
        }
    }

  return true;
}

/* New: another window of the editor's, or of the extension's */

static void new_document(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;

  if (e->ext != NULL && e->ext->newdoc != NULL)
    {
      e->ext->newdoc(w);
    }
  else
    {
      desk_editor_open(NULL);
    }
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

  if (e->ext != NULL && e->ext->key != NULL && e->ext->key(w, k))
    {
      return;
    }

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

        if (e->cc > 0 && e->lang == LANG_PYTHON && py_indent_only(e))
          {
            /* In the indent: back to the one before */

            int to = (e->cc - 1) / PY_INDENT * PY_INDENT;

            ed_delete(w, e->cl, to, e->cl, e->cc, true);
          }
        else if (e->cc > 0)
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

          /* Python: a block starts after a ':' (the line's last, before
           * any comment): four more
           */

          if (e->lang == LANG_PYTHON && py_opens(ln->s, e->cc))
            {
              char *t = realloc(s, n + PY_INDENT);

              if (t != NULL)
                {
                  s = t;
                  memset(s + n, ' ', PY_INDENT);
                  n += PY_INDENT;
                }
            }

          ed_insert(w, s, n, false);
          free(s);
        }
        break;

      case '\t':
        if (e->lang == LANG_PYTHON)
          {
            /* Spaces to the next indent */

            int v = vcol(&e->ln[e->cl], e->cc);

            delete_sel(w);
            ed_insert(w, "    ", PY_INDENT - v % PY_INDENT, true);
          }
        else
          {
            ed_insert(w, "\t", 1, true);
          }
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
        new_document(w);
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

  if (e != NULL)
    {
      kit_key(&e->kit, ch, n);           /* The text's: KIT_EV_KEY */
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

/* The pointer on the text (the kit hands it over while a button pressed
 * there is held): the cursor goes there, a drag selects, two clicks
 * select a word, three a line
 */

static void text_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct edit_s *e = w->priv;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(e->buttons & NX_MOUSE_LEFTBUTTON);
  struct lay_s l;
  int pl;
  int pc;

  e->buttons = buttons;
  layout(w, &l);

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

  if (press && y >= l.ty && y < l.ty + l.th)
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

static void edit_event(struct kit_s *k, struct kit_widget_s *wg, int ev)
{
  struct desk_win_s *w = k->w;
  struct edit_s *e = w->priv;
  int id = wg != NULL ? wg->id : 0;

  /* An extension's widgets, menus and dialogs */

  if (e->ext != NULL && e->ext->event != NULL &&
      (id >= EDIT_EXT_ID || ev == KIT_EV_MENU ||
       (ev == KIT_EV_DIALOG && k->key >= EDIT_EXT_ID)))
    {
      e->ext->event(w, k, wg, ev);
      return;
    }

  switch (ev)
    {
      case KIT_EV_CLICK:
        switch (id)
          {
            case E_NEW:
              new_document(w);
              break;

            case E_SAVE:
              save(w);
              break;

            case E_UNDO:
            case E_REDO:
              ed_undo(w, id == E_REDO);
              refresh(w, true);
              break;

            default:
              break;
          }
        break;

      case KIT_EV_CHANGE:
        if (id == E_SCROLL)
          {
            struct lay_s l;

            layout(w, &l);
            scroll_to(w, &l, wg->value);
            refresh(w, false);
          }
        break;

      case KIT_EV_MOUSE:
        if (id == E_TEXT)
          {
            text_mouse(w, k->mx, k->my, k->mbuttons);
          }
        break;

      case KIT_EV_KEY:
        if (id == E_TEXT && !kit_dialog_open(k))
          {
            edit_keyone(w, k->key);
            if (!w->closing && !kit_dialog_open(k))
              {
                refresh(w, true);
              }
          }

        k->key = 0;
        break;

      case KIT_EV_DIALOG:
        answer(w, k->key, k->arg, k->answer);
        break;

      default:
        break;
    }
}

/* The kit's draw function: the text, where the kit draws what is under
 * it again (a dialog gone, an expose); its own operations, cut to the
 * text area
 */

static void edit_paint(struct kit_s *k, struct kit_widget_s *wg, int x0,
                       int y0, int x1, int y1)
{
  struct desk_win_s *w = k->w;
  struct edit_s *e = w->priv;
  struct lay_s l;
  int a;
  int b;

  if (wg->id >= EDIT_EXT_ID)
    {
      if (e->ext != NULL && e->ext->paint != NULL)
        {
          e->ext->paint(w, k, wg, x0, y0, x1, y1);
        }

      return;
    }

  layout(w, &l);
  if (!e->full)
    {
      dirty(e, e->top + (y0 - l.ty) / LH, e->top + (y1 - 1 - l.ty) / LH);
    }

  lines_draw(w, &l, &a, &b);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
}

static void edit_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct edit_s *e = w->priv;

  if (e != NULL)
    {
      kit_mouse(&e->kit, x, y, buttons);
    }
}

/* The cursor blinks; it goes when the window is not active */

static void edit_tick(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;
  bool blink = w->active && (!e->blink || desk_frozen()) &&
               !kit_dialog_open(&e->kit);

  if (blink != e->blink)
    {
      e->blink = blink;
      dirty(e, e->cl, e->cl);
      refresh(w, false);
    }

  kit_tick(&e->kit);
}

static void edit_resize(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;
  struct lay_s l;

  kit_layout(&e->kit);
  kit_resize(&e->kit);                   /* A dialog stays in the middle */
  layout(w, &l);
  if (e->top > e->nl - 1)
    {
      e->top = e->nl - 1;
    }

  edit_draw(w);
}

static void edit_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  struct edit_s *e = w->priv;

  if (e != NULL)
    {
      kit_expose(&e->kit, x, y, cw, ch);
    }
}

static void edit_focus(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;

  if (e != NULL)
    {
      e->blink = w->active;
      dirty(e, e->cl, e->cl);
      kit_activate(&e->kit);
      refresh(w, false);
    }
}

static bool edit_canclose(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;

  if (e == NULL || !e->modified)
    {
      return true;
    }

  if (!kit_dialog_open(&e->kit))
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

  if (e->ext != NULL && e->ext->close != NULL)
    {
      e->ext->close(w);
    }

  ln_free(e);
  undo_free(e->undo, e->nundo);
  undo_free(e->redo, e->nredo);
  e->nundo = 0;
  e->nredo = 0;
  free(e->cls);
  e->cls = NULL;
  kit_dialog_close(&e->kit);
}

/* The window's widgets: a tool bar, the text and its scroll bar, a status
 * bar
 */

static void edit_build(struct desk_win_s *w, struct edit_s *e)
{
  struct kit_s *k = &e->kit;
  struct kit_widget_s *wg;

  kit_init(k, w, e->wg, NWG, edit_event);
  k->priv = e;
  k->draw = edit_paint;

  kit_col(k, KIT_BOX_WINDOW);
    kit_row(k, KIT_BOX_BAR);
      if (e->ext != NULL && e->ext->toolbar != NULL)
        {
          e->ext->toolbar(w, k);
        }

      kit_button(k, E_NEW, "New", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_tip(k, "A new document (Ctrl-N)");
      kit_button(k, E_SAVE, "Save", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_tip(k, "Ctrl-S");
      kit_spacer(k, 0);
      kit_size(k, 4, 0);
      kit_button(k, E_UNDO, "Undo", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_tip(k, "Ctrl-Z");
      kit_button(k, E_REDO, "Redo", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_tip(k, "Ctrl-Y");
      kit_spacer(k, 0);
      kit_size(k, 4, 0);
      kit_label(k, E_PATH, e->pathlabel, KIT_TEXT_DIM);
      kit_grow(k, 1);
    kit_end(k);

    wg = kit_row(k, KIT_BOX_NONE);
    wg->gap = 0;
    kit_grow(k, 1);
      kit_custom(k, E_TEXT)->flags |= KIT_F_KEYS;
      kit_grow(k, 1);
      kit_scroll(k, E_SCROLL);
    kit_end(k);

    if (e->ext != NULL && e->ext->pane != NULL)
      {
        e->ext->pane(w, k);
      }

    kit_row(k, KIT_BOX_STATUS)->ph = 26;
      kit_label(k, E_LEFT, e->lefttext, KIT_TEXT_DIM);
      kit_grow(k, 1);
      kit_label(k, E_RIGHT, e->right, KIT_TEXT_DIM);
    kit_end(k);
  kit_end(k);

  kit_focus(k, E_TEXT);
  kit_layout(k);
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

static struct desk_win_s *edit_open_with(const char *path,
                                         const struct edit_ext_s *ext,
                                         void *priv)
{
  struct desk_win_s *w;
  struct edit_s *e;
  int ret = 0;

  e = calloc(1, sizeof(*e));
  if (e == NULL)
    {
      return NULL;
    }

  if (ext != NULL)
    {
      e->kind    = ext->kind;
      e->ext     = ext;
      e->extpriv = priv;
      e->lang    = ext->python ? LANG_PYTHON : LANG_PLAIN;
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

  w = desk_win_open(ext != NULL ? ext->app : APP_EDITOR, "Text Editor",
                    ext != NULL ? ext->w : EDIT_W,
                    ext != NULL ? ext->h : EDIT_H);
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
  edit_build(w, e);
  edit_draw(w);
  return w;
}

static struct desk_win_s *edit_open(void)
{
  return edit_open_with(g_openpath, NULL, NULL);
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

/* An editor with more, for another application (desk.h) */

struct desk_win_s *desk_edit_open_ext(const char *path,
                                      const struct edit_ext_s *ext,
                                      void *priv)
{
  return edit_open_with(path, ext, priv);
}

struct kit_s *desk_edit_kit(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;

  return &e->kit;
}

void *desk_edit_extpriv(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;

  return e != NULL ? e->extpriv : NULL;
}

const char *desk_edit_path(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;

  return e != NULL ? e->path : "";
}

bool desk_edit_modified(struct desk_win_s *w)
{
  struct edit_s *e = w->priv;

  return e != NULL && e->modified;
}

void desk_edit_save(struct desk_win_s *w)
{
  save(w);
}

/* Another file in the window (the text so far is let go of) */

int desk_edit_load(struct desk_win_s *w, const char *path)
{
  struct edit_s *e = w->priv;
  int ret;

  ln_free(e);
  undo_free(e->undo, e->nundo);
  undo_free(e->redo, e->nredo);
  e->nundo    = 0;
  e->nredo    = 0;
  e->savedid  = 0;
  e->modified = false;
  e->sel      = false;
  e->cl       = 0;
  e->cc       = 0;
  e->top      = 0;
  e->left     = 0;
  e->path[0]  = '\0';
  e->msg[0]   = '\0';
  ret = ed_load(e, path);
  if (ret < 0)
    {
      ln_free(e);
      snprintf(e->msg, sizeof(e->msg), "Cannot open %s: %s", path,
               strerror(-ret));
      e->msgerr = true;
    }

  if (e->nl == 0 && ln_make(e, 0, 1) < 0)
    {
      return -ENOMEM;
    }

  edit_draw(w);
  set_title(w);
  return ret;
}

void desk_edit_goto(struct desk_win_s *w, int line)
{
  struct edit_s *e = w->priv;
  struct line_s *ln;
  int c = 0;

  line = line < 1 ? 1 : line > e->nl ? e->nl : line;
  ln = &e->ln[line - 1];
  while (c < ln->len && (ln->s[c] == ' ' || ln->s[c] == '\t'))
    {
      c++;
    }

  sel_none(e);
  move(w, line - 1, c, false);
  e->want = vcol(ln, c);

  /* The line in the middle of the view, if it was not in it */

  if (line - 1 < e->top || line - 1 >= e->top + 8)
    {
      struct lay_s l;

      layout(w, &l);
      if (line - 1 < e->top || line - 1 >= e->top + l.full)
        {
          int top = line - 1 - l.full / 2;

          scroll_to(w, &l, top < 0 ? 0 : top);
        }
    }

  kit_focus(&e->kit, E_TEXT);
  refresh(w, true);
}

void desk_edit_message(struct desk_win_s *w, bool err, const char *msg)
{
  struct edit_s *e = w->priv;

  strlcpy(e->msg, msg, sizeof(e->msg));
  e->msgerr = err;
  refresh(w, false);
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
  .expose   = edit_expose,
  .resize   = edit_resize,
  .tick     = edit_tick,
  .mouse    = edit_mouse,
  .key      = edit_key,
  .focus    = edit_focus,
  .close    = edit_close,
  .canclose = edit_canclose,
};
