/****************************************************************************
 * apps/graphics/fpcdesk/desk_files.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Files: the file manager.  A sidebar of places, a tool bar (back, up, the
 * path as buttons, refresh), the folder as a list (icon, name, size,
 * modified; folders first, sorted by a column), and a bar at the bottom:
 * the status and the actions (new folder, rename, delete), or a question.
 *
 * The list scrolls by pixels: a COPY within the window moves what stays,
 * and only the rows that come into view are drawn (cut to the list with
 * ui_clip()).  The pointer or the selection moving redraws just the rows
 * concerned.  Once a second the folder's names are read (without stat()):
 * if they changed, the folder is loaded again.
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "desk_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FILES_W        700
#define FILES_H        440

#define TB_H           44               /* Tool bar */
#define SIDE_W         150              /* Places */
#define HDR_H          28               /* Column names */
#define ROW_H          28
#define BAR_H          42               /* Status and actions */
#define SB_W           12               /* Scroll bar */
#define PLACE_H        30

#define MAX_PLACES     6
#define MAX_CRUMBS     16
#define HIST_N         16
#define DBLCLICK_MS    400

#define C_SIDE         RGB(0x2a, 0x2a, 0x2e)
#define C_PATHBAR      RGB(0x1c, 0x1c, 0x20)
#define C_FOLDER       RGB(0x5a, 0xa8, 0xf0)
#define C_PAGE         RGB(0xdc, 0xdc, 0xe2)
#define C_EXEC         RGB(0x3d, 0xc0, 0x70)
#define C_THUMB        RGB(0x80, 0x80, 0x88)

/* Kinds of entry */

#define K_DIR          0
#define K_FILE         1
#define K_TEXT         2
#define K_EXEC         3

/* Sort columns */

#define COL_NAME       0
#define COL_SIZE       1
#define COL_DATE       2

/* Areas that react to the pointer, by group (redrawn together) */

#define ZG_TOOL        0
#define ZG_SIDE        1
#define ZG_HEAD        2
#define ZG_BAR         3

#define Z_NONE         0
#define Z_BACK         1
#define Z_UP           2
#define Z_REFRESH      3
#define Z_NEWDIR       4
#define Z_RENAME       5
#define Z_DELETE       6
#define Z_OK           7
#define Z_CANCEL       8
#define Z_COL          16                /* + column */
#define Z_CRUMB        32                /* + crumb */
#define Z_PLACE        64                /* + place */

/* Questions in the bottom bar */

#define P_NONE         0
#define P_NEWDIR       1
#define P_RENAME       2
#define P_DELETE       3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct fent_s
{
  char *name;
  off_t size;
  time_t mtime;
  uint8_t kind;
};

struct place_s
{
  const char *name;
  char path[48];
};

struct crumb_s
{
  int16_t w;
  int16_t len;                           /* Its path: this much of ours */
  char label[40];
};

/* Where things are, in frame coordinates */

struct lay_s
{
  int cw;
  int y0;                                /* Top of the client area */
  int bottom;                            /* Below it */
  int lx;                                /* The rows */
  int ly;
  int lw;
  int lh;
  int hy;                                /* Column names */
  int sbx;                               /* Scroll bar */
  int by;                                /* Bottom bar */
  int namex;                             /* Name text */
  int namew;
  int sizer;                             /* Right edge of sizes, 0: none */
  int datex;                             /* Dates, 0: none */
};

struct files_s
{
  char path[PATH_MAX];
  char tmp[PATH_MAX];
  struct fent_s *ent;
  int n;
  int cap;
  uint32_t sig;                          /* The names, hashed */
  int nfiles;
  int ndirs;

  int sel;                               /* -1: none */
  int hover;                             /* Row under the pointer, -1 */
  int top;                               /* Scrolled, pixels */
  int sortcol;
  bool sortdesc;

  char *hist[HIST_N];                    /* Back */
  int nhist;

  struct place_s place[MAX_PLACES];
  int nplaces;
  struct crumb_s crumb[MAX_CRUMBS];
  int ncrumbs;
  int crumb0;                            /* First shown, the rest elided */
  struct ui_zones_s zs;

  uint8_t buttons;
  bool dragthumb;
  int grab;                              /* Pointer in the thumb */
  uint32_t clickms;
  int clickrow;

  char msg[128];
  bool msgerr;

  int prompt;
  bool deltree;                          /* Delete: a folder with contents */
  struct ui_field_s field;
  struct ui_keys_s keys;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct desk_ops_s g_o;
static int g_sortcol;
static bool g_sortdesc;

static const char * const g_textext[] =
{
  "txt", "c", "h", "cc", "cpp", "hpp", "cxx", "md", "sh", "py", "cfg",
  "conf", "ini", "log", "json", "xml", "html", "htm", "css", "js", "csv",
  "mk", "rc", "s", "asm", "lua", "rs", "go", "java", "toml", "yaml", "yml",
  "ld", "patch", "diff", "v", "sv", "tcl", "cst", "sdc", "rst", "nsh", NULL
};

static const char * const g_textname[] =
{
  "Makefile", "Kconfig", "README", "LICENSE", "NOTICE", "defconfig",
  "Make.defs", NULL
};

static const char * const g_execext[] =
{
  "elf", "bin", "uf2", "hex", "nxf", NULL
};

/****************************************************************************
 * Private Functions: paths and folders
 ****************************************************************************/

static void path_join(char *out, const char *dir, const char *name)
{
  snprintf(out, PATH_MAX, "%s%s%s", dir,
           dir[0] != '\0' && dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}

/* The parent of path, in place; returns the name that was the last (in
 * path's buffer, after the parent), or NULL at the root
 */

static const char *path_up(char *path)
{
  char *s = strrchr(path, '/');

  if (s == NULL || strcmp(path, "/") == 0)
    {
      return NULL;
    }

  if (s == path)
    {
      memmove(path + 2, path + 1, strlen(path + 1) + 1);
      path[1] = '\0';
      return path + 2;
    }

  *s = '\0';
  return s + 1;
}

static bool is_dir(const char *path)
{
  struct stat st;

  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool in_list(const char *s, const char * const *list, bool nocase)
{
  int i;

  for (i = 0; list[i] != NULL; i++)
    {
      if ((nocase ? strcasecmp(s, list[i]) : strcmp(s, list[i])) == 0)
        {
          return true;
        }
    }

  return false;
}

static uint8_t kind_of(const char *name, const struct stat *st)
{
  const char *ext = strrchr(name, '.');

  if (S_ISDIR(st->st_mode))
    {
      return K_DIR;
    }

  if (in_list(name, g_textname, false) ||
      (ext != NULL && in_list(ext + 1, g_textext, true)))
    {
      return K_TEXT;
    }

  if ((ext != NULL && in_list(ext + 1, g_execext, true)) ||
      (st->st_mode & S_IXUSR) != 0)
    {
      return K_EXEC;
    }

  return K_FILE;
}

static uint32_t hash(const char *s)
{
  uint32_t h = 2166136261u;

  while (*s != '\0')
    {
      h = (h ^ (uint8_t)*s++) * 16777619u;
    }

  return h;
}

/* Names as people sort them: case aside, numbers by value (doc2 before
 * doc10)
 */

static int natcmp(const char *a, const char *b)
{
  while (*a != '\0' && *b != '\0')
    {
      if (isdigit((uint8_t)*a) && isdigit((uint8_t)*b))
        {
          const char *a0;
          const char *b0;
          int na;
          int nb;

          while (*a == '0' && isdigit((uint8_t)a[1]))
            {
              a++;
            }

          while (*b == '0' && isdigit((uint8_t)b[1]))
            {
              b++;
            }

          for (a0 = a; isdigit((uint8_t)*a); a++)
            {
            }

          for (b0 = b; isdigit((uint8_t)*b); b++)
            {
            }

          na = a - a0;
          nb = b - b0;
          if (na != nb)
            {
              return na - nb;
            }

          while (a0 < a)
            {
              if (*a0 != *b0)
                {
                  return *a0 - *b0;
                }

              a0++;
              b0++;
            }
        }
      else
        {
          int d = tolower((uint8_t)*a) - tolower((uint8_t)*b);

          if (d != 0)
            {
              return d;
            }

          a++;
          b++;
        }
    }

  return (uint8_t)*a - (uint8_t)*b;
}

static int ent_cmp(const void *a, const void *b)
{
  const struct fent_s *x = a;
  const struct fent_s *y = b;
  int r = 0;

  if ((x->kind == K_DIR) != (y->kind == K_DIR))
    {
      return x->kind == K_DIR ? -1 : 1;
    }

  switch (g_sortcol)
    {
      case COL_SIZE:                     /* (Folders: by name) */
        if (x->kind != K_DIR)
          {
            r = x->size < y->size ? -1 : x->size > y->size;
          }
        break;

      case COL_DATE:
        r = x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
        break;

      default:
        break;
    }

  if (r == 0)
    {
      r = natcmp(x->name, y->name);
    }

  if (r == 0)
    {
      r = strcmp(x->name, y->name);
    }

  return g_sortdesc ? -r : r;
}

static void ents_free(struct files_s *f)
{
  int i;

  for (i = 0; i < f->n; i++)
    {
      free(f->ent[i].name);
    }

  free(f->ent);
  f->ent = NULL;
  f->n = 0;
  f->cap = 0;
}

static void ents_sort(struct files_s *f)
{
  g_sortcol  = f->sortcol;
  g_sortdesc = f->sortdesc;
  if (f->n > 1)
    {
      qsort(f->ent, f->n, sizeof(f->ent[0]), ent_cmp);
    }
}

static int ent_find(struct files_s *f, const char *name)
{
  int i;

  for (i = 0; name != NULL && i < f->n; i++)
    {
      if (strcmp(f->ent[i].name, name) == 0)
        {
          return i;
        }
    }

  return -1;
}

/* The names in a folder, hashed in any order: has it changed? */

static uint32_t dir_sig(const char *path, int *count)
{
  struct dirent *de;
  uint32_t h = 0;
  DIR *d;
  int n = 0;

  d = opendir(path);
  if (d == NULL)
    {
      *count = -1;
      return 0;
    }

  while ((de = readdir(d)) != NULL)
    {
      if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
        {
          h += hash(de->d_name);
          n++;
        }
    }

  closedir(d);
  *count = n;
  return h;
}

/* Load a folder: returns 0 or -errno (and then nothing changed) */

static int dir_load(struct files_s *f, const char *path)
{
  struct fent_s *ent = NULL;
  struct dirent *de;
  uint32_t h = 0;
  int nfiles = 0;
  int ndirs = 0;
  int cap = 0;
  int n = 0;
  DIR *d;

  d = opendir(path);
  if (d == NULL)
    {
      return -errno;
    }

  while ((de = readdir(d)) != NULL)
    {
      struct stat st;
      struct fent_s *e;

      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
        {
          continue;
        }

      if (n == cap)
        {
          int ncap = cap ? cap * 2 : 32;
          struct fent_s *ne = realloc(ent, ncap * sizeof(*ent));

          if (ne == NULL)
            {
              break;                             /* What fits */
            }

          ent = ne;
          cap = ncap;
        }

      e = &ent[n];
      e->name = strdup(de->d_name);
      if (e->name == NULL)
        {
          break;
        }

      path_join(f->tmp, path, de->d_name);
      if (stat(f->tmp, &st) < 0)
        {
          memset(&st, 0, sizeof(st));
          st.st_mode = de->d_type == DT_DIR ? S_IFDIR : S_IFREG;
        }

      e->size  = st.st_size;
      e->mtime = st.st_mtime;
      e->kind  = kind_of(e->name, &st);
      h += hash(de->d_name);
      if (e->kind == K_DIR)
        {
          ndirs++;
        }
      else
        {
          nfiles++;
        }

      n++;
    }

  closedir(d);
  ents_free(f);
  f->ent    = ent;
  f->n      = n;
  f->cap    = cap;
  f->sig    = h;
  f->nfiles = nfiles;
  f->ndirs  = ndirs;
  ents_sort(f);
  return 0;
}

/****************************************************************************
 * Private Functions: layout
 ****************************************************************************/

static void layout(struct desk_win_s *w, struct lay_s *l)
{
  l->cw     = w->csize.w;
  l->y0     = TITLE_H;
  l->bottom = TITLE_H + w->csize.h;
  l->hy     = l->y0 + TB_H;
  l->lx     = SIDE_W + 1;
  l->ly     = l->hy + HDR_H;
  l->sbx    = l->cw - SB_W;
  l->lw     = l->sbx - l->lx;
  l->by     = l->bottom - BAR_H;
  l->lh     = l->by - l->ly;
  if (l->lh < 0)
    {
      l->lh = 0;
    }

  l->namex = l->lx + 40;
  l->sizer = 0;
  l->datex = 0;
  if (l->lw >= 470)
    {
      l->datex = l->lx + l->lw - 136;
      l->sizer = l->datex - 24;
      l->namew = l->sizer - 80 - l->namex;
    }
  else if (l->lw >= 300)
    {
      l->sizer = l->lx + l->lw - 14;
      l->namew = l->sizer - 80 - l->namex;
    }
  else
    {
      l->namew = l->lx + l->lw - 10 - l->namex;
    }
}

static int content_h(struct files_s *f)
{
  return f->n * ROW_H;
}

static int max_top(struct files_s *f, struct lay_s *l)
{
  int m = content_h(f) - l->lh;

  return m > 0 ? m : 0;
}

/* The scroll bar's thumb: its top and height; false if there is none */

static bool thumb(struct files_s *f, struct lay_s *l, int *ty, int *th)
{
  int total = content_h(f);

  if (total <= l->lh || l->lh <= 0)
    {
      return false;
    }

  *th = l->lh * l->lh / total;
  *th = *th < 24 ? 24 : *th;
  *ty = l->ly + (l->lh - *th) * f->top / max_top(f, l);
  return true;
}

/****************************************************************************
 * Private Functions: icons
 ****************************************************************************/

static void icon_folder(struct desk_ops_s *o, int x, int y, uint16_t c)
{
  ops_rrect(o, x, y + 2, 9, 6, LUT_R3, 3, colour_mix(c, C_BLACK, 50), 255);
  ops_rrect(o, x, y + 4, 18, 13, LUT_R3, 3, c, 255);
  ops_blend(o, x + 2, y + 6, 14, 1, C_WHITE, 70);
}

static void icon_page(struct desk_ops_s *o, int x, int y, int kind)
{
  int i;

  if (kind == K_EXEC)
    {
      ops_rrect(o, x, y + 1, 18, 16, LUT_R3, 3, C_EXEC, 255);
      ops_blend(o, x + 4, y + 5, 2, 2, C_WHITE, 240);
      ops_blend(o, x + 6, y + 7, 2, 2, C_WHITE, 240);
      ops_blend(o, x + 4, y + 9, 2, 2, C_WHITE, 240);
      ops_blend(o, x + 9, y + 11, 5, 2, C_WHITE, 240);
      return;
    }

  ops_rrect(o, x + 2, y, 14, 18, LUT_R3, 3, C_PAGE, 255);
  ops_blend(o, x + 11, y, 5, 5, C_BLACK, 70);
  if (kind == K_TEXT)
    {
      for (i = 0; i < 4; i++)
        {
          ops_blend(o, x + 5, y + 6 + i * 3, i == 3 ? 5 : 8, 1,
                    RGB(0x60, 0x60, 0x68), 255);
        }
    }
}

/* A place's symbol, 16 x 16, in c on bg */

static void icon_place(struct desk_ops_s *o, const char *path, int x, int y,
                       uint16_t c, uint16_t bg)
{
  if (strcmp(path, "/") == 0)
    {
      ops_rrect(o, x, y + 1, 16, 11, LUT_R3, 3, c, 255);
      ops_fill(o, x + 2, y + 3, 12, 7, bg);
      ops_fill(o, x + 7, y + 12, 2, 2, c);
      ops_fill(o, x + 4, y + 14, 8, 1, c);
    }
  else if (strcmp(path, "/mnt/sd0") == 0)
    {
      ops_rrect(o, x + 2, y, 12, 16, LUT_R3, 3, c, 255);
      ops_fill(o, x + 10, y, 4, 4, bg);
      ops_fill(o, x + 5, y + 2, 1, 3, bg);
      ops_fill(o, x + 8, y + 2, 1, 3, bg);
    }
  else if (strcmp(path, "/tmp") == 0)
    {
      icon_folder(o, x - 1, y - 1, c);
    }
  else
    {
      ui_triangle(o, x + 8, y + 4, 7, 0, c, 255);
      ops_fill(o, x + 3, y + 8, 10, 7, c);
      ops_fill(o, x + 7, y + 11, 3, 4, bg);
    }
}

/****************************************************************************
 * Private Functions: drawing
 ****************************************************************************/

static void fmt_size(off_t s, char *buf, int n)
{
  if (s < 1000)
    {
      snprintf(buf, n, "%d bytes", (int)s);
    }
  else if (s < 1000000)
    {
      snprintf(buf, n, "%d.%d kB", (int)(s / 1000), (int)(s % 1000 / 100));
    }
  else if (s < 1000000000)
    {
      snprintf(buf, n, "%d.%d MB", (int)(s / 1000000),
               (int)(s % 1000000 / 100000));
    }
  else
    {
      snprintf(buf, n, "%d.%d GB", (int)(s / 1000000000),
               (int)(s % 1000000000 / 100000000));
    }
}

static void fmt_date(time_t t, char *buf, int n)
{
  struct tm tm;
  struct tm now;
  time_t tn = time(NULL);

  buf[0] = '\0';
  if (t <= 0 || localtime_r(&t, &tm) == NULL ||
      localtime_r(&tn, &now) == NULL)
    {
      return;
    }

  if (tm.tm_year == now.tm_year && tm.tm_yday == now.tm_yday)
    {
      strftime(buf, n, "Today %H:%M", &tm);
    }
  else if (tm.tm_year == now.tm_year)
    {
      strftime(buf, n, "%d %b %H:%M", &tm);
    }
  else
    {
      strftime(buf, n, "%d %b %Y", &tm);
    }
}

/* Row i (past the last: empty), cut to [cy0, cy1) of the list */

static void row_draw(struct desk_win_s *w, struct lay_s *l, int i, int cy0,
                     int cy1)
{
  struct files_s *f = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  const struct desk_font_s *font = &g_desk_font_ui;
  int y = l->ly + i * ROW_H - f->top;
  int ty = y + (ROW_H - text_height(font)) / 2;
  char buf[96];
  int start;

  if (cy0 < l->ly)
    {
      cy0 = l->ly;
    }

  if (cy1 > l->ly + l->lh)
    {
      cy1 = l->ly + l->lh;
    }

  if (y >= cy1 || y + ROW_H <= cy0)
    {
      return;
    }

  ui_flush(hwnd, &g_o, 160);
  start = g_o.n;
  ops_fill(&g_o, l->lx, y, l->lw, ROW_H, C_WINDOW_BG);
  if (i < f->n)
    {
      struct fent_s *e = &f->ent[i];
      bool sel = i == f->sel;
      uint16_t fg = sel ? C_WHITE : C_TEXT;
      uint16_t dim = sel ? RGB(0xdc, 0xe8, 0xff) : C_TEXT_DIM;

      if (sel)
        {
          ops_rrect(&g_o, l->lx + 4, y + 1, l->lw - 8, ROW_H - 2, LUT_R6, 6,
                    C_ACCENT, 255);
        }
      else if (i == f->hover)
        {
          ops_rrect(&g_o, l->lx + 4, y + 1, l->lw - 8, ROW_H - 2, LUT_R6, 6,
                    C_WHITE, 16);
        }

      if (e->kind == K_DIR)
        {
          icon_folder(&g_o, l->lx + 12, y + 5, C_FOLDER);
        }
      else
        {
          icon_page(&g_o, l->lx + 12, y + 5, e->kind);
        }

      ui_fit(font, e->name, l->namew, buf, sizeof(buf));
      ops_text(&g_o, font, l->namex, ty, buf, fg, 255);
      if (l->sizer > 0 && e->kind != K_DIR)
        {
          fmt_size(e->size, buf, sizeof(buf));
          ops_text(&g_o, font, l->sizer - text_width(font, buf), ty, buf,
                   dim, 255);
        }

      if (l->datex > 0)
        {
          fmt_date(e->mtime, buf, sizeof(buf));
          ops_text(&g_o, font, l->datex, ty, buf, dim, 255);
        }
    }

  ui_clip(&g_o, start, l->lx, cy0, l->lx + l->lw, cy1);
}

static void scrollbar_draw(struct desk_win_s *w, struct lay_s *l)
{
  struct files_s *f = w->priv;
  int ty;
  int th;

  ui_flush((NXWINDOW)w->hwnd, &g_o, 8);
  ops_fill(&g_o, l->sbx, l->ly, SB_W, l->lh, C_WINDOW_BG);
  if (thumb(f, l, &ty, &th))
    {
      ops_rrect(&g_o, l->sbx + 3, ty + 2, 6, th - 4, LUT_R3, 3, C_THUMB,
                f->dragthumb ? 255 : 170);
    }
}

/* The list between y0 and y1 (frame coordinates) */

static void list_paint(struct desk_win_s *w, struct lay_s *l, int y0,
                       int y1)
{
  struct files_s *f = w->priv;
  int i;
  int last;

  if (y0 < l->ly)
    {
      y0 = l->ly;
    }

  if (y1 > l->ly + l->lh)
    {
      y1 = l->ly + l->lh;
    }

  if (y1 <= y0)
    {
      return;
    }

  last = (y1 - 1 - l->ly + f->top) / ROW_H;
  for (i = (y0 - l->ly + f->top) / ROW_H; i <= last; i++)
    {
      row_draw(w, l, i, y0, y1);
    }

  if (f->n == 0)
    {
      int start = g_o.n;

      ops_text_centre(&g_o, &g_desk_font_ui, l->lx + l->lw / 2,
                      l->ly + l->lh / 2 - 20, "Folder is empty", C_TEXT_DIM,
                      255);
      ui_clip(&g_o, start, l->lx, y0, l->lx + l->lw, y1);
    }
}

static void list_draw(struct desk_win_s *w, struct lay_s *l)
{
  list_paint(w, l, l->ly, l->ly + l->lh);
  scrollbar_draw(w, l);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
}

/* Scroll the list to top: what stays is copied, the rest drawn */

static void list_scroll(struct desk_win_s *w, struct lay_s *l, int top)
{
  struct files_s *f = w->priv;
  struct nx_gpuop_s *op;
  int d;

  if (top > max_top(f, l))
    {
      top = max_top(f, l);
    }

  if (top < 0)
    {
      top = 0;
    }

  d = top - f->top;
  if (d == 0)
    {
      return;
    }

  f->top = top;
  if (d >= l->lh || -d >= l->lh)
    {
      list_paint(w, l, l->ly, l->ly + l->lh);
    }
  else
    {
      ui_flush((NXWINDOW)w->hwnd, &g_o, 1);
      op = ops_add(&g_o);
      op->op = NX_GPUOP_COPY;
      op->x  = l->lx;
      op->w  = l->lw;
      op->h  = l->lh - (d > 0 ? d : -d);
      op->sx = l->lx;
      if (d > 0)
        {
          op->y  = l->ly;
          op->sy = l->ly + d;
          list_paint(w, l, l->ly + l->lh - d, l->ly + l->lh);
        }
      else
        {
          op->y  = l->ly - d;
          op->sy = l->ly;
          list_paint(w, l, l->ly, l->ly - d);
        }
    }

  scrollbar_draw(w, l);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
}

static void row_redraw(struct desk_win_s *w, struct lay_s *l, int i)
{
  if (i >= 0)
    {
      row_draw(w, l, i, l->ly, l->ly + l->lh);
    }
}

/* The path as buttons: the place it is in, then a button a folder; the
 * first ones are left out (an ellipsis) if they do not fit in avail
 */

static void crumbs_build(struct files_s *f, int avail)
{
  const struct desk_font_s *font = &g_desk_font_ui;
  const char *p = f->path;
  size_t blen = 0;
  int base = -1;
  int total;
  int n = 0;
  int i;
  int k;

  for (k = 0; k < f->nplaces; k++)
    {
      size_t len = strlen(f->place[k].path);

      if (strcmp(f->place[k].path, "/") != 0 &&
          strncmp(p, f->place[k].path, len) == 0 &&
          (p[len] == '\0' || p[len] == '/') && len > blen)
        {
          base = k;
          blen = len;
        }
    }

  strlcpy(f->crumb[0].label, base >= 0 ? f->place[base].name : "Computer",
          sizeof(f->crumb[0].label));
  f->crumb[0].len = base >= 0 ? blen : 1;
  n = 1;

  i = blen;
  for (; ; )
    {
      int j;

      while (p[i] == '/')
        {
          i++;
        }

      if (p[i] == '\0')
        {
          break;
        }

      for (j = i; p[j] != '\0' && p[j] != '/'; j++)
        {
        }

      if (n == MAX_CRUMBS)
        {
          memmove(&f->crumb[1], &f->crumb[2],
                  (MAX_CRUMBS - 2) * sizeof(f->crumb[0]));
          n--;
        }

      k = j - i < (int)sizeof(f->crumb[n].label) - 1 ? j - i :
          (int)sizeof(f->crumb[n].label) - 1;
      memcpy(f->crumb[n].label, p + i, k);
      f->crumb[n].label[k] = '\0';
      f->crumb[n].len = j;
      n++;
      i = j;
    }

  f->ncrumbs = n;

  /* Widths; leave out the first ones if they do not fit */

  total = -14;
  for (k = 0; k < n; k++)
    {
      const struct desk_font_s *cf = k == n - 1 ? &g_desk_font_uib : font;
      char buf[40];

      ui_fit(cf, f->crumb[k].label, 180, buf, sizeof(buf));
      strlcpy(f->crumb[k].label, buf, sizeof(f->crumb[k].label));
      f->crumb[k].w = text_width(cf, buf) + 16;
      total += f->crumb[k].w + 14;
    }

  f->crumb0 = 0;
  while (total > avail && f->crumb0 < n - 1)
    {
      total -= f->crumb[f->crumb0].w + 14;
      if (f->crumb0 == 0)
        {
          total += text_width(font, DESK_CH_ELLIPSIS) + 16 + 14;
        }

      f->crumb0++;
    }
}

static void toolbar_draw(struct desk_win_s *w, struct lay_s *l)
{
  struct files_s *f = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  const struct desk_font_s *font = &g_desk_font_ui;
  int y = l->y0 + 7;
  int ty = y + (30 - text_height(font)) / 2;
  int px = 88;
  int pw = l->cw - px - 52;
  uint16_t rbg;
  int x;
  int k;
  bool can;

  ui_zones_clear(&f->zs, ZG_TOOL);
  ui_flush(hwnd, &g_o, 220);
  ops_fill(&g_o, 0, l->y0, l->cw, TB_H, C_UI_BAR);
  ops_fill(&g_o, 0, l->y0 + TB_H - 1, l->cw, 1, C_UI_LINE);

  /* Back, up */

  can = f->nhist > 0;
  ui_button(&g_o, 10, y, 32, 30, NULL, ui_look(&f->zs, Z_BACK, can));
  ops_fill(&g_o, 21, y + 14, 11, 2, can ? C_TEXT : C_TEXT_DIM);
  ui_triangle(&g_o, 19, y + 15, 5, 2, can ? C_TEXT : C_TEXT_DIM, 255);
  ui_zone_add(&f->zs, ZG_TOOL, Z_BACK, 10, y, 32, 30, can);

  can = strcmp(f->path, "/") != 0;
  ui_button(&g_o, 46, y, 32, 30, NULL, ui_look(&f->zs, Z_UP, can));
  ops_fill(&g_o, 61, y + 12, 2, 11, can ? C_TEXT : C_TEXT_DIM);
  ui_triangle(&g_o, 62, y + 10, 5, 0, can ? C_TEXT : C_TEXT_DIM, 255);
  ui_zone_add(&f->zs, ZG_TOOL, Z_UP, 46, y, 32, 30, can);

  /* The path */

  ops_rrect(&g_o, px, y, pw, 30, LUT_R6, 6, C_PATHBAR, 255);
  crumbs_build(f, pw - 8);
  x = px + 4;
  if (f->crumb0 > 0)
    {
      int cw = text_width(font, DESK_CH_ELLIPSIS) + 16;

      if (f->zs.hot == Z_CRUMB + f->crumb0 - 1)
        {
          ops_rrect(&g_o, x, y + 3, cw, 24, LUT_R6, 6, C_UI_HOVER, 255);
        }

      ops_text(&g_o, font, x + 8, ty, DESK_CH_ELLIPSIS, C_TEXT_DIM, 255);
      ui_zone_add(&f->zs, ZG_TOOL, Z_CRUMB + f->crumb0 - 1, x, y, cw, 30,
                  true);
      x += cw;
      ops_text(&g_o, font, x + 4, ty, "/", C_TEXT_DIM, 160);
      x += 14;
    }

  for (k = f->crumb0; k < f->ncrumbs; k++)
    {
      struct crumb_s *c = &f->crumb[k];
      bool last = k == f->ncrumbs - 1;

      if (f->zs.hot == Z_CRUMB + k && !last)
        {
          ops_rrect(&g_o, x, y + 3, c->w, 24, LUT_R6, 6, C_UI_HOVER, 255);
        }

      ops_text(&g_o, last ? &g_desk_font_uib : font, x + 8, ty, c->label,
               last ? C_TEXT : C_TEXT_DIM, 255);
      ui_zone_add(&f->zs, ZG_TOOL, Z_CRUMB + k, x, y, c->w, 30, !last);
      x += c->w;
      if (!last)
        {
          ops_text(&g_o, font, x + 4, ty, "/", C_TEXT_DIM, 160);
          x += 14;
        }
    }

  /* Refresh: a ring with a gap and an arrowhead */

  x = l->cw - 42;
  k = ui_look(&f->zs, Z_REFRESH, true);
  rbg = k == UI_PRESSED ? C_UI_PRESSED : k == UI_HOVER ? C_UI_HOVER :
        C_UI_BUTTON;
  ui_button(&g_o, x, y, 32, 30, NULL, k);
  ops_disc(&g_o, x + 16, y + 15, LUT_R6, 6, C_TEXT, 255);
  ops_disc(&g_o, x + 16, y + 15, LUT_R6, 4, rbg, 255);
  ops_fill(&g_o, x + 17, y + 8, 6, 7, rbg);
  ui_triangle(&g_o, x + 18, y + 11, 4, 3, C_TEXT, 255);
  ui_zone_add(&f->zs, ZG_TOOL, Z_REFRESH, x, y, 32, 30, true);
  ops_draw(hwnd, &g_o);
}

static void sidebar_draw(struct desk_win_s *w, struct lay_s *l)
{
  struct files_s *f = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  int y = l->y0 + TB_H;
  int k;

  ui_zones_clear(&f->zs, ZG_SIDE);
  ui_flush(hwnd, &g_o, 220);
  ops_fill(&g_o, 0, y, SIDE_W, l->bottom - y, C_SIDE);
  ops_fill(&g_o, SIDE_W, y, 1, l->bottom - y, C_UI_LINE);
  ops_text(&g_o, &g_desk_font_small, 16, y + 12, "Places", C_TEXT_DIM, 255);
  y += 32;
  for (k = 0; k < f->nplaces; k++)
    {
      bool here = strcmp(f->path, f->place[k].path) == 0;
      uint16_t c = here ? C_WHITE : C_TEXT;
      uint16_t bg = C_SIDE;

      if (y + PLACE_H > l->bottom - 8)
        {
          break;
        }

      if (here)
        {
          bg = C_ACCENT;
          ops_rrect(&g_o, 8, y, SIDE_W - 16, PLACE_H - 2, LUT_R6, 6, bg,
                    255);
        }
      else if (f->zs.hot == Z_PLACE + k)
        {
          bg = C_UI_HOVER;
          ops_rrect(&g_o, 8, y, SIDE_W - 16, PLACE_H - 2, LUT_R6, 6, bg,
                    255);
        }

      icon_place(&g_o, f->place[k].path, 18, y + 6, c, bg);
      ops_text(&g_o, &g_desk_font_ui, 42, y + 6, f->place[k].name, c, 255);
      ui_zone_add(&f->zs, ZG_SIDE, Z_PLACE + k, 8, y, SIDE_W - 16,
                  PLACE_H - 2, true);
      y += PLACE_H;
    }

  ops_draw(hwnd, &g_o);
}

static void header_draw(struct desk_win_s *w, struct lay_s *l)
{
  static const char * const names[3] =
  {
    "Name", "Size", "Modified"
  };

  struct files_s *f = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  const struct desk_font_s *font = &g_desk_font_small;
  int ty = l->hy + (HDR_H - text_height(font)) / 2;
  int c;

  ui_zones_clear(&f->zs, ZG_HEAD);
  ui_flush(hwnd, &g_o, 60);
  ops_fill(&g_o, l->lx, l->hy, l->cw - l->lx, HDR_H, C_WINDOW_BG);
  ops_fill(&g_o, l->lx + 8, l->hy + HDR_H - 1, l->cw - l->lx - 16, 1,
           C_UI_BAR);
  for (c = 0; c < 3; c++)
    {
      int x;
      int zx;
      int zw;
      int tw = text_width(font, names[c]);
      bool on = f->sortcol == c;
      uint16_t col = on || f->zs.hot == Z_COL + c ? C_TEXT : C_TEXT_DIM;

      if (c == COL_NAME)
        {
          x  = l->namex;
          zx = l->lx;
          zw = l->namex + l->namew - l->lx;
        }
      else if (c == COL_SIZE && l->sizer > 0)
        {
          x  = l->sizer - tw;
          zx = l->sizer - 80;
          zw = 80 + 8;
        }
      else if (c == COL_DATE && l->datex > 0)
        {
          x  = l->datex;
          zx = l->datex - 8;
          zw = l->lx + l->lw - zx;
        }
      else
        {
          continue;
        }

      ops_text(&g_o, font, x, ty, names[c], col, 255);
      if (on)
        {
          int ax = c == COL_SIZE ? x - 10 : x + tw + 10;

          ui_triangle(&g_o, ax, l->hy + HDR_H / 2, 4, f->sortdesc ? 1 : 0,
                      col, 255);
        }

      ui_zone_add(&f->zs, ZG_HEAD, Z_COL + c, zx, l->hy, zw, HDR_H, true);
    }

  ops_draw(hwnd, &g_o);
}

static void bar_text(struct files_s *f, char *buf, int n)
{
  char size[24];

  if (f->msg[0] != '\0')
    {
      strlcpy(buf, f->msg, n);
    }
  else if (f->sel >= 0 && f->sel < f->n)
    {
      struct fent_s *e = &f->ent[f->sel];

      if (e->kind == K_DIR)
        {
          snprintf(buf, n, "\"%s\" selected", e->name);
        }
      else
        {
          fmt_size(e->size, size, sizeof(size));
          snprintf(buf, n, "\"%s\" selected (%s)", e->name, size);
        }
    }
  else
    {
      snprintf(buf, n, "%d folder%s, %d file%s", f->ndirs,
               f->ndirs == 1 ? "" : "s", f->nfiles,
               f->nfiles == 1 ? "" : "s");
    }
}

/* A button of the bottom bar, right-aligned at x; returns the next x */

static int bar_button(struct files_s *f, int x, int y, int id,
                      const char *label, bool enabled, int accent)
{
  int bw = ui_button_width(label);
  int lk = ui_look(&f->zs, id, enabled);

  if (accent != UI_NORMAL && lk != UI_DISABLED && lk != UI_PRESSED)
    {
      lk = accent;
    }

  x -= bw;
  ui_button(&g_o, x, y, bw, 28, label, lk);
  ui_zone_add(&f->zs, ZG_BAR, id, x, y, bw, 28, enabled);
  return x - 8;
}

static void bar_draw(struct desk_win_s *w, struct lay_s *l)
{
  struct files_s *f = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  const struct desk_font_s *font = &g_desk_font_ui;
  int x0 = l->lx;
  int y = l->by + (BAR_H - 28) / 2;
  int ty = l->by + (BAR_H - text_height(font)) / 2;
  int x = l->cw - 12;
  char buf[160];
  char fit[160];
  bool sel = f->sel >= 0 && f->sel < f->n;

  ui_zones_clear(&f->zs, ZG_BAR);
  ops_draw(hwnd, &g_o);
  ops_fill(&g_o, x0, l->by, l->cw - x0, BAR_H, C_UI_BAR);
  ops_fill(&g_o, x0, l->by, l->cw - x0, 1, C_UI_LINE);

  switch (f->prompt)
    {
      case P_NEWDIR:
      case P_RENAME:
        {
          const char *label = f->prompt == P_NEWDIR ? "Folder name" :
                              "Rename to";
          int lw = text_width(font, label);

          x = bar_button(f, x, y, Z_OK, f->prompt == P_NEWDIR ? "Create" :
                         "Rename", f->field.len > 0, UI_DEFAULT);
          x = bar_button(f, x, y, Z_CANCEL, "Cancel", true, UI_NORMAL);
          ops_text(&g_o, font, x0 + 14, ty, label, C_TEXT_DIM, 255);
          ui_field_draw(&g_o, &f->field, x0 + 24 + lw, y, x - x0 - 24 - lw,
                        28, true);
        }
        break;

      case P_DELETE:
        x = bar_button(f, x, y, Z_OK, "Delete", true, UI_DANGER);
        x = bar_button(f, x, y, Z_CANCEL, "Cancel", true, UI_NORMAL);
        if (sel)
          {
            snprintf(buf, sizeof(buf), f->deltree ?
                     "Delete \"%s\" and all it holds?" : "Delete \"%s\"?",
                     f->ent[f->sel].name);
            ui_fit(font, buf, x - x0 - 20, fit, sizeof(fit));
            ops_text(&g_o, font, x0 + 14, ty, fit, C_TEXT, 255);
          }
        break;

      default:
        x = bar_button(f, x, y, Z_DELETE, "Delete", sel, UI_NORMAL);
        x = bar_button(f, x, y, Z_RENAME, "Rename", sel, UI_NORMAL);
        x = bar_button(f, x, y, Z_NEWDIR, "New Folder", true, UI_NORMAL);
        bar_text(f, buf, sizeof(buf));
        ui_fit(font, buf, x - x0 - 20, fit, sizeof(fit));
        ops_text(&g_o, font, x0 + 14, ty, fit,
                 f->msg[0] != '\0' && f->msgerr ? C_UI_ERROR : C_TEXT_DIM,
                 255);
        break;
    }

  ops_draw(hwnd, &g_o);
}

static void group_draw(struct desk_win_s *w, int group)
{
  struct lay_s l;

  layout(w, &l);
  switch (group)
    {
      case ZG_TOOL:
        toolbar_draw(w, &l);
        break;

      case ZG_SIDE:
        sidebar_draw(w, &l);
        break;

      case ZG_HEAD:
        header_draw(w, &l);
        break;

      case ZG_BAR:
        bar_draw(w, &l);
        break;

      default:
        break;
    }
}

static void files_draw(struct desk_win_s *w)
{
  struct files_s *f = w->priv;
  struct lay_s l;

  if (f == NULL)
    {
      return;
    }

  layout(w, &l);
  if (f->top > max_top(f, &l))
    {
      f->top = max_top(f, &l);
    }

  ops_reset(&g_o);
  toolbar_draw(w, &l);
  sidebar_draw(w, &l);
  header_draw(w, &l);
  list_draw(w, &l);
  bar_draw(w, &l);
}

/****************************************************************************
 * Private Functions: actions
 ****************************************************************************/

static void set_msg(struct files_s *f, bool err, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(f->msg, sizeof(f->msg), fmt, ap);
  va_end(ap);
  f->msgerr = err;
}

/* The title: the folder's name (the last of the path's buttons) */

static void set_title(struct desk_win_s *w)
{
  struct files_s *f = w->priv;

  if (f->ncrumbs > 0)
    {
      snprintf(w->title, sizeof(w->title), "%s",
               f->crumb[f->ncrumbs - 1].label);
      desk_win_title(w);
    }
}

/* Go to a folder (push: remember where we were, for Back); select the
 * entry named select if there is one
 */

static int files_go(struct desk_win_s *w, const char *path, bool push,
                    const char *select)
{
  struct files_s *f = w->priv;
  char *old = strdup(f->path);
  int ret;

  if (old == NULL)
    {
      return -ENOMEM;
    }

  ret = dir_load(f, path);
  if (ret < 0)
    {
      set_msg(f, true, "Cannot open \"%s\": %s", path, strerror(-ret));
      group_draw(w, ZG_BAR);
      free(old);
      return ret;
    }

  if (push && strcmp(old, path) != 0 && old[0] != '\0')
    {
      if (f->nhist == HIST_N)
        {
          free(f->hist[0]);
          memmove(&f->hist[0], &f->hist[1], (HIST_N - 1) * sizeof(char *));
          f->nhist--;
        }

      f->hist[f->nhist++] = old;
      old = NULL;
    }

  free(old);
  if (path != f->path)
    {
      strlcpy(f->path, path, sizeof(f->path));
    }

  f->sel    = ent_find(f, select);
  f->hover  = -1;
  f->top    = 0;
  f->msg[0] = '\0';
  f->prompt = P_NONE;
  if (f->sel >= 0)
    {
      struct lay_s l;

      layout(w, &l);
      f->top = f->sel * ROW_H - l.lh / 2;
      f->top = f->top < 0 ? 0 : f->top > max_top(f, &l) ? max_top(f, &l) :
               f->top;
    }

  files_draw(w);
  set_title(w);
  return 0;
}

/* Load the folder again, keeping the selection and the scroll */

static void files_reload(struct desk_win_s *w)
{
  struct files_s *f = w->priv;
  char *name = NULL;
  int ret;

  if (f->sel >= 0 && f->sel < f->n)
    {
      name = strdup(f->ent[f->sel].name);
    }

  ret = dir_load(f, f->path);
  if (ret < 0)
    {
      /* Gone (a card taken out?): up until something opens */

      char *p = strdup(f->path);

      free(name);
      if (p == NULL)
        {
          return;
        }

      while (path_up(p) != NULL && !is_dir(p))
        {
        }

      files_go(w, p, false, NULL);
      set_msg(f, true, "The folder has gone");
      group_draw(w, ZG_BAR);
      free(p);
      return;
    }

  f->sel = ent_find(f, name);
  f->hover = -1;
  free(name);
  files_draw(w);
}

static void ensure_visible(struct desk_win_s *w, int i)
{
  struct files_s *f = w->priv;
  struct lay_s l;
  int y;

  layout(w, &l);
  y = i * ROW_H;
  if (y < f->top)
    {
      list_scroll(w, &l, y);
    }
  else if (y + ROW_H > f->top + l.lh)
    {
      list_scroll(w, &l, y + ROW_H - l.lh);
    }
}

static void select_row(struct desk_win_s *w, int i)
{
  struct files_s *f = w->priv;
  struct lay_s l;
  int old = f->sel;

  if (i >= f->n)
    {
      i = f->n - 1;
    }

  if (i < -1)
    {
      i = -1;
    }

  layout(w, &l);
  if (i != old || f->msg[0] != '\0')
    {
      f->sel = i;
      f->msg[0] = '\0';
      row_redraw(w, &l, old);
      row_redraw(w, &l, i);
      ops_draw((NXWINDOW)w->hwnd, &g_o);
      if (f->prompt == P_NONE)
        {
          bar_draw(w, &l);
        }
    }

  if (i >= 0)
    {
      ensure_visible(w, i);
    }
}

/* Text: its start has no NUL and few control characters */

static bool is_text(const char *path)
{
  uint8_t buf[256];
  ssize_t n;
  int fd = open(path, O_RDONLY);
  int ctl = 0;
  int i;

  if (fd < 0)
    {
      return false;
    }

  n = read(fd, buf, sizeof(buf));
  close(fd);
  for (i = 0; i < n; i++)
    {
      if (buf[i] == '\0')
        {
          return false;
        }

      ctl += buf[i] < 0x20 && strchr("\t\n\r\f\v\x1b", buf[i]) == NULL;
    }

  return n >= 0 && ctl * 32 <= n;
}

static void files_open_sel(struct desk_win_s *w)
{
  struct files_s *f = w->priv;
  struct fent_s *e;

  if (f->sel < 0 || f->sel >= f->n)
    {
      return;
    }

  e = &f->ent[f->sel];
  path_join(f->tmp, f->path, e->name);
  if (e->kind == K_DIR)
    {
      char *p = strdup(f->tmp);

      if (p != NULL)
        {
          files_go(w, p, true, NULL);
          free(p);
        }
    }
  else if (e->kind == K_TEXT || is_text(f->tmp))
    {
      desk_editor_open(f->tmp);
    }
  else
    {
      set_msg(f, true, "No application opens \"%s\"", e->name);
      group_draw(w, ZG_BAR);
    }
}

static void files_up(struct desk_win_s *w)
{
  struct files_s *f = w->priv;
  const char *name;
  char *child;
  char *p;

  strlcpy(f->tmp, f->path, sizeof(f->tmp));
  name = path_up(f->tmp);
  if (name == NULL)
    {
      return;
    }

  child = strdup(name);
  p = strdup(f->tmp);
  if (child != NULL && p != NULL)
    {
      files_go(w, p, true, child);
    }

  free(p);
  free(child);
}

static void files_back(struct desk_win_s *w)
{
  struct files_s *f = w->priv;
  char *p;

  if (f->nhist == 0)
    {
      return;
    }

  p = f->hist[--f->nhist];
  if (files_go(w, p, false, NULL) < 0)
    {
      group_draw(w, ZG_TOOL);
    }

  free(p);
}

/* Remove a folder and all it holds (depth first, depth-limited); path is
 * a PATH_MAX buffer, used for the entries and given back as it was
 */

static int remove_tree(char *path, int depth)
{
  struct dirent *de;
  size_t len = strlen(path);
  int ret = 0;
  DIR *d;

  if (depth > 16)
    {
      return -ELOOP;
    }

  d = opendir(path);
  if (d == NULL)
    {
      return -errno;
    }

  while (ret == 0 && (de = readdir(d)) != NULL)
    {
      struct stat st;

      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
        {
          continue;
        }

      if (len + 1 + strlen(de->d_name) >= PATH_MAX)
        {
          ret = -ENAMETOOLONG;
          break;
        }

      path[len] = '/';
      strcpy(path + len + 1, de->d_name);
      if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        {
          ret = remove_tree(path, depth + 1);
        }
      else if (unlink(path) < 0)
        {
          ret = -errno;
        }

      path[len] = '\0';
      if (ret == 0)
        {
          /* The folder changed under readdir(): start again */

          closedir(d);
          d = opendir(path);
          if (d == NULL)
            {
              return -errno;
            }
        }
    }

  closedir(d);
  if (ret == 0 && rmdir(path) < 0)
    {
      ret = -errno;
    }

  return ret;
}

static bool dir_empty(const char *path)
{
  struct dirent *de;
  bool empty = true;
  DIR *d = opendir(path);

  if (d == NULL)
    {
      return true;
    }

  while ((de = readdir(d)) != NULL)
    {
      if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
        {
          empty = false;
          break;
        }
    }

  closedir(d);
  return empty;
}

static void prompt_start(struct desk_win_s *w, int prompt)
{
  struct files_s *f = w->priv;
  bool sel = f->sel >= 0 && f->sel < f->n;

  if (prompt != P_NEWDIR && !sel)
    {
      return;
    }

  f->prompt = prompt;
  f->msg[0] = '\0';
  f->zs.hot    = Z_NONE;
  switch (prompt)
    {
      case P_NEWDIR:
        ui_field_set(&f->field, "New Folder");
        break;

      case P_RENAME:
        ui_field_set(&f->field, f->ent[f->sel].name);
        break;

      case P_DELETE:
        path_join(f->tmp, f->path, f->ent[f->sel].name);
        f->deltree = f->ent[f->sel].kind == K_DIR && !dir_empty(f->tmp);
        break;

      default:
        break;
    }

  group_draw(w, ZG_BAR);
}

static void prompt_end(struct desk_win_s *w, bool ok)
{
  struct files_s *f = w->priv;
  char *name = NULL;
  int ret = 0;

  if (!ok)
    {
      f->prompt = P_NONE;
      group_draw(w, ZG_BAR);
      return;
    }

  switch (f->prompt)
    {
      case P_NEWDIR:
        if (f->field.len == 0 || strchr(f->field.text, '/') != NULL)
          {
            return;
          }

        path_join(f->tmp, f->path, f->field.text);
        if (mkdir(f->tmp, 0777) < 0)
          {
            ret = -errno;
            set_msg(f, true, "Cannot make \"%s\": %s", f->field.text,
                    strerror(errno));
          }
        else
          {
            name = strdup(f->field.text);
          }
        break;

      case P_RENAME:
        {
          char *to;

          if (f->field.len == 0 || strchr(f->field.text, '/') != NULL ||
              f->sel < 0 || f->sel >= f->n)
            {
              return;
            }

          if (strcmp(f->field.text, f->ent[f->sel].name) == 0)
            {
              break;
            }

          to = malloc(PATH_MAX);
          if (to == NULL)
            {
              return;
            }

          path_join(f->tmp, f->path, f->ent[f->sel].name);
          path_join(to, f->path, f->field.text);

          /* (FAT's names ignore case: renaming to another case is fine) */

          if (access(to, F_OK) == 0 &&
              strcasecmp(f->field.text, f->ent[f->sel].name) != 0)
            {
              ret = -EEXIST;
              set_msg(f, true, "\"%s\" already exists", f->field.text);
            }
          else if (rename(f->tmp, to) < 0)
            {
              ret = -errno;
              set_msg(f, true, "Cannot rename: %s", strerror(errno));
            }
          else
            {
              name = strdup(f->field.text);
            }

          free(to);
        }
        break;

      case P_DELETE:
        if (f->sel < 0 || f->sel >= f->n)
          {
            break;
          }

        path_join(f->tmp, f->path, f->ent[f->sel].name);
        if (f->ent[f->sel].kind == K_DIR)
          {
            ret = remove_tree(f->tmp, 0);
          }
        else if (unlink(f->tmp) < 0)
          {
            ret = -errno;
          }

        if (ret < 0)
          {
            set_msg(f, true, "Cannot delete \"%s\": %s",
                    f->ent[f->sel].name, strerror(-ret));
          }
        else
          {
            /* Select what follows */

            int next = f->sel + 1 < f->n ? f->sel + 1 : f->sel - 1;

            name = next >= 0 ? strdup(f->ent[next].name) : NULL;
          }
        break;

      default:
        break;
    }

  f->prompt = P_NONE;
  if (ret < 0)
    {
      group_draw(w, ZG_BAR);
      return;
    }

  /* The folder changed: load it, select the result */

  if (dir_load(f, f->path) == 0)
    {
      f->sel = ent_find(f, name);
      f->hover = -1;
    }

  free(name);
  files_draw(w);
  if (f->sel >= 0)
    {
      ensure_visible(w, f->sel);
    }
}

static void sort_by(struct desk_win_s *w, int col)
{
  struct files_s *f = w->priv;
  char *name = f->sel >= 0 ? strdup(f->ent[f->sel].name) : NULL;

  if (f->sortcol == col)
    {
      f->sortdesc = !f->sortdesc;
    }
  else
    {
      f->sortcol  = col;
      f->sortdesc = false;
    }

  ents_sort(f);
  f->sel = ent_find(f, name);
  f->hover = -1;
  free(name);
  files_draw(w);
}

static void zone_action(struct desk_win_s *w, int id)
{
  struct files_s *f = w->priv;

  if (id >= Z_PLACE)
    {
      char *p = strdup(f->place[id - Z_PLACE].path);

      if (p != NULL)
        {
          files_go(w, p, true, NULL);
          free(p);
        }
    }
  else if (id >= Z_CRUMB)
    {
      int k = id - Z_CRUMB;
      char *p = malloc(PATH_MAX);
      char *child = k + 1 < f->ncrumbs ? strdup(f->crumb[k + 1].label) :
                    NULL;

      if (p != NULL)
        {
          snprintf(p, PATH_MAX, "%.*s", f->crumb[k].len, f->path);
          files_go(w, p, true, child);
        }

      free(child);
      free(p);
    }
  else if (id >= Z_COL)
    {
      sort_by(w, id - Z_COL);
    }
  else
    {
      switch (id)
        {
          case Z_BACK:
            files_back(w);
            break;

          case Z_UP:
            files_up(w);
            break;

          case Z_REFRESH:
            files_reload(w);
            break;

          case Z_NEWDIR:
            prompt_start(w, P_NEWDIR);
            break;

          case Z_RENAME:
            prompt_start(w, P_RENAME);
            break;

          case Z_DELETE:
            prompt_start(w, P_DELETE);
            break;

          case Z_OK:
            prompt_end(w, true);
            break;

          case Z_CANCEL:
            prompt_end(w, false);
            break;

          default:
            break;
        }
    }
}

/****************************************************************************
 * Private Functions: events
 ****************************************************************************/

static void set_hot(struct desk_win_s *w, int id)
{
  struct files_s *f = w->priv;
  int old = f->zs.hot;
  int g0 = ui_zone_group(&f->zs, old);
  int g1 = ui_zone_group(&f->zs, id);

  if (id == old)
    {
      return;
    }

  f->zs.hot = id;
  if (g0 >= 0)
    {
      group_draw(w, g0);
    }

  if (g1 >= 0 && g1 != g0)
    {
      group_draw(w, g1);
    }
}

static void set_hover(struct desk_win_s *w, struct lay_s *l, int row)
{
  struct files_s *f = w->priv;
  int old = f->hover;

  if (row == old)
    {
      return;
    }

  f->hover = row;
  row_redraw(w, l, old);
  row_redraw(w, l, row);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
}

static void files_mouse(struct desk_win_s *w, int x, int cy,
                        uint8_t buttons)
{
  struct files_s *f = w->priv;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(f->buttons & NX_MOUSE_LEFTBUTTON);
  bool release = !(buttons & NX_MOUSE_LEFTBUTTON) &&
                 (f->buttons & NX_MOUSE_LEFTBUTTON);
  struct ui_zone_s *z;
  struct lay_s l;
  int y = cy + TITLE_H;
  int row = -1;
  int ty;
  int th;

  f->buttons = buttons;
  layout(w, &l);

  /* The pointer has left */

  if (x < 0 && cy < 0 && buttons == 0)
    {
      set_hot(w, Z_NONE);
      set_hover(w, &l, -1);
      f->zs.pressed = Z_NONE;
      f->dragthumb = false;
      return;
    }

  /* Dragging the scroll bar's thumb */

  if (f->dragthumb)
    {
      if ((buttons & NX_MOUSE_LEFTBUTTON) && thumb(f, &l, &ty, &th) &&
          l.lh > th)
        {
          list_scroll(w, &l, (y - f->grab - l.ly) * max_top(f, &l) /
                      (l.lh - th));
        }
      else
        {
          f->dragthumb = false;
          scrollbar_draw(w, &l);
          ops_draw((NXWINDOW)w->hwnd, &g_o);
        }

      return;
    }

  z = ui_zone_at(&f->zs, x, y);
  if (x >= l.lx && x < l.lx + l.lw && y >= l.ly && y < l.ly + l.lh)
    {
      row = (y - l.ly + f->top) / ROW_H;
      if (row >= f->n)
        {
          row = -1;
        }
    }

  if (!(buttons & NX_MOUSE_LEFTBUTTON) || press)
    {
      set_hot(w, z != NULL && z->enabled ? z->id : Z_NONE);
      set_hover(w, &l, row);
    }

  if (press)
    {
      if (z != NULL)
        {
          if (z->enabled)
            {
              f->zs.pressed = z->id;
              group_draw(w, z->group);
            }
        }
      else if (x >= l.sbx && y >= l.ly && y < l.ly + l.lh)
        {
          if (!thumb(f, &l, &ty, &th))
            {
              return;
            }

          if (y >= ty && y < ty + th)
            {
              f->dragthumb = true;
              f->grab = y - ty;
              scrollbar_draw(w, &l);
              ops_draw((NXWINDOW)w->hwnd, &g_o);
            }
          else
            {
              list_scroll(w, &l, f->top + (y < ty ? -l.lh : l.lh));
            }
        }
      else if (x >= l.lx && x < l.lx + l.lw && y >= l.ly &&
               y < l.ly + l.lh && f->prompt == P_NONE)
        {
          uint32_t now = desk_ms();

          select_row(w, row);
          if (row >= 0 && row == f->clickrow &&
              now - f->clickms < DBLCLICK_MS)
            {
              f->clickrow = -1;
              files_open_sel(w);
              return;
            }

          f->clickrow = row;
          f->clickms  = now;
        }
    }

  if (release && f->zs.pressed != Z_NONE)
    {
      int id = f->zs.pressed;
      int g = ui_zone_group(&f->zs, id);

      f->zs.pressed = Z_NONE;
      if (z != NULL && z->id == id)
        {
          zone_action(w, id);
        }
      else if (g >= 0)
        {
          group_draw(w, g);
        }
    }
}

static void files_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct files_s *f = w->priv;
  int keys[16];
  int nk = ui_keys(&f->keys, ch, n, keys, 16);
  int i;

  for (i = 0; i < nk; i++)
    {
      int k = keys[i];
      struct lay_s l;
      int page;

      layout(w, &l);
      page = l.lh / ROW_H - 1;
      page = page < 1 ? 1 : page;

      if (f->prompt != P_NONE)
        {
          if (k == '\r' || k == '\n')
            {
              prompt_end(w, true);
            }
          else if (k == KEY_ESC)
            {
              prompt_end(w, false);
            }
          else if (f->prompt != P_DELETE && ui_field_key(&f->field, k))
            {
              group_draw(w, ZG_BAR);
            }

          continue;
        }

      switch (k & ~KEY_SHIFT)
        {
          case KEY_UP:
            select_row(w, f->sel < 0 ? f->n - 1 : f->sel > 0 ? f->sel - 1 :
                       0);
            break;

          case KEY_DOWN:
            select_row(w, f->sel + 1 < f->n ? f->sel + 1 : f->sel);
            break;

          case KEY_PGUP:
            select_row(w, f->sel - page < 0 ? 0 : f->sel - page);
            break;

          case KEY_PGDN:
            select_row(w, f->sel + page);
            break;

          case KEY_HOME:
            select_row(w, 0);
            break;

          case KEY_END:
            select_row(w, f->n - 1);
            break;

          case '\r':
          case '\n':
            files_open_sel(w);
            break;

          case 0x7f:
          case 0x08:
          case KEY_LEFT | KEY_CTRL:
            files_up(w);
            break;

          case KEY_DEL:
            prompt_start(w, P_DELETE);
            break;

          case KEY_F2:
            prompt_start(w, P_RENAME);
            break;

          case KEY_F5:
          case KEY_CTL('r'):
            files_reload(w);
            break;

          case KEY_CTL('n'):
            prompt_start(w, P_NEWDIR);
            break;

          case KEY_CTL('c'):
            if (f->sel >= 0)
              {
                path_join(f->tmp, f->path, f->ent[f->sel].name);
                desk_clip_set(f->tmp, strlen(f->tmp));
                set_msg(f, false, "Copied the path of \"%s\"",
                        f->ent[f->sel].name);
                group_draw(w, ZG_BAR);
              }
            break;

          case KEY_ESC:
            select_row(w, -1);
            break;

          default:

            /* A letter: the next name that starts with it */

            if (k > ' ' && k < 0x7f && f->n > 0)
              {
                int j;

                for (j = 1; j <= f->n; j++)
                  {
                    int e = (f->sel + j + f->n) % f->n;

                    if (tolower((uint8_t)f->ent[e].name[0]) == tolower(k))
                      {
                        select_row(w, e);
                        break;
                      }
                  }
              }
            break;
        }
    }
}

static void files_tick(struct desk_win_s *w)
{
  struct files_s *f = w->priv;
  uint32_t sig;
  int count;

  if (f == NULL || f->prompt != P_NONE || f->dragthumb || f->buttons != 0)
    {
      return;
    }

  sig = dir_sig(f->path, &count);
  if (count != f->n || sig != f->sig)
    {
      files_reload(w);
    }
}

static void place_add(struct files_s *f, const char *name, const char *path)
{
  int k;

  if (f->nplaces == MAX_PLACES || path == NULL || !is_dir(path) ||
      strlen(path) >= sizeof(f->place[0].path))
    {
      return;
    }

  for (k = 0; k < f->nplaces; k++)
    {
      if (strcmp(f->place[k].path, path) == 0)
        {
          return;
        }
    }

  f->place[f->nplaces].name = name;
  strlcpy(f->place[f->nplaces].path, path, sizeof(f->place[0].path));
  f->nplaces++;
}

static void files_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  icon_tile(o, x, y, s, RGB(0x5a, 0xb4, 0xff), RGB(0x1e, 0x6e, 0xd8), a);
  ops_rrect(o, x + s / 5, y + s / 3, s * 3 / 5, s * 2 / 5, LUT_R3, 3,
            C_WHITE, (uint8_t)(a * 230 / 255));
  ops_rrect(o, x + s / 5, y + s / 4, s / 4, s / 8, LUT_R3, 3, C_WHITE,
            (uint8_t)(a * 230 / 255));
}

static struct desk_win_s *files_open(void)
{
  struct desk_win_s *w;
  struct files_s *f;
  const char *home = getenv("HOME");

  f = calloc(1, sizeof(*f));
  if (f == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_FILES, "Files", FILES_W, FILES_H);
  if (w == NULL)
    {
      free(f);
      return NULL;
    }

  w->priv     = f;
  f->sel      = -1;
  f->hover    = -1;
  f->clickrow = -1;

#ifdef CONFIG_LIBC_HOMEDIR
  if (home == NULL)
    {
      home = CONFIG_LIBC_HOMEDIR;
    }
#endif

  place_add(f, "Home", home != NULL && strcmp(home, "/") != 0 ? home :
            NULL);
  place_add(f, "microSD", "/mnt/sd0");
  place_add(f, "Computer", "/");
  place_add(f, "Temporary", "/tmp");

  if (f->nplaces == 0 || files_go(w, f->place[0].path, false, NULL) < 0)
    {
      files_go(w, "/", false, NULL);
    }

  return w;
}

static void files_close(struct desk_win_s *w)
{
  struct files_s *f = w->priv;
  int i;

  if (f == NULL)
    {
      return;
    }

  ents_free(f);
  for (i = 0; i < f->nhist; i++)
    {
      free(f->hist[i]);
    }

  f->nhist = 0;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_files =
{
  .name  = "Files",
  .icon  = files_icon,
  .open  = files_open,
  .draw  = files_draw,
  .tick  = files_tick,
  .mouse = files_mouse,
  .key   = files_key,
  .close = files_close,
  .single = true,
};
