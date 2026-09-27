/****************************************************************************
 * apps/graphics/fpcdesk/desk_files.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Files: the file manager, built with the widget kit (desk_kit.h).  A
 * sidebar of places, a tool bar (back, up, the path as buttons, refresh),
 * the folder as a table (icon, name, size, modified; folders first,
 * sorted by a column: click it), and a status bar with the actions (new
 * folder, rename, delete: each asks in a dialog).  The right button on a
 * row is a menu of what can be done with it.
 *
 * The table is the kit's list: it scrolls by pixels with a COPY within
 * the window and draws only the rows that come into view; the pointer or
 * the selection moving draws just the rows concerned.  Once a second the
 * folder's names are read (without stat()): if they changed, the folder
 * is loaded again.
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

#include "desk_kit.h"
#include "desk_image.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FILES_W        700
#define FILES_H        440

#define SIDE_W         150              /* Places */
#define BAR_H          42               /* Status and actions */
#define NWG            24               /* The kit's widgets */

#define MAX_PLACES     6
#define MAX_CRUMBS     16
#define HIST_N         16

#define C_FOLDER       RGB(0x5a, 0xa8, 0xf0)
#define C_PAGE         THEMED(RGB(0xdc, 0xdc, 0xe2), RGB(0xc8, 0xcc, 0xd4))
#define C_EXEC         RGB(0x3d, 0xc0, 0x70)

/* Kinds of entry */

#define K_DIR          0
#define K_FILE         1
#define K_TEXT         2
#define K_EXEC         3
#define K_IMAGE        4

/* Sort columns */

#define COL_NAME       0
#define COL_SIZE       1
#define COL_DATE       2

/* The widgets */

#define F_BACK         1
#define F_UP           2
#define F_PATH         3
#define F_REFRESH      4
#define F_PLACES       5
#define F_LIST         6
#define F_STATUS       7
#define F_NEWDIR       8
#define F_RENAME       9
#define F_DELETE       10

/* Questions (dialogs, their ids) and the row's menu */

#define P_NONE         0
#define P_NEWDIR       1
#define P_RENAME       2
#define P_DELETE       3

#define M_ROW          1
#define M_OPEN         0                /* Its items */
#define M_RENAME       1
#define M_DELETE       2
#define M_COPYPATH     4
#define M_NEWDIR       5

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
  int16_t len;                           /* Its path: this much of ours */
  char label[40];
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
  int sortcol;
  bool sortdesc;

  char *hist[HIST_N];                    /* Back */
  int nhist;

  struct place_s place[MAX_PLACES];
  int nplaces;
  struct crumb_s crumb[MAX_CRUMBS];
  const char *crumbp[MAX_CRUMBS];        /* Their labels, for the kit */
  int ncrumbs;

  /* The window: the kit's widgets, the table and the places */

  struct kit_s kit;
  struct kit_widget_s wg[NWG];
  struct kit_list_s list;
  struct kit_list_s places;
  char status[160];

  char msg[128];
  bool msgerr;
  bool deltree;                          /* Delete: a folder with contents */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct kit_col_s g_cols[3] =
{
  { "Name",     0,   0,   0 },
  { "Size",     104, 300, KIT_COL_RIGHT | KIT_COL_DIM },
  { "Modified", 148, 470, KIT_COL_DIM },
};

static const char * const g_rowmenu[6] =
{
  "Open", "Rename" DESK_CH_ELLIPSIS, "Delete" DESK_CH_ELLIPSIS, "-",
  "Copy Path", "New Folder" DESK_CH_ELLIPSIS
};

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

  if (img_is_image(name))
    {
      return K_IMAGE;
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

  if (kind == K_IMAGE)
    {
      /* A picture: a frame, a sky, a hill */

      ops_rrect(o, x, y + 2, 18, 14, LUT_R3, 3, C_PAGE, 255);
      ops_fill(o, x + 2, y + 4, 14, 10, RGB(0x6c, 0xb4, 0xf0));
      ops_fill(o, x + 11, y + 6, 2, 2, RGB(0xff, 0xd8, 0x50));
      ui_triangle(o, x + 7, y + 11, 4, 0, RGB(0x34, 0xa0, 0x50), 255);
      ops_fill(o, x + 2, y + 13, 14, 1, RGB(0x34, 0xa0, 0x50));
      return;
    }

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
 * Private Functions: what the widgets show
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
  time_t tz = (time_t)g_prefs.tzoff * 60;         /* The desktop's zone */
  time_t tn = desk_time() + tz;

  buf[0] = '\0';
  t += tz;
  if (t <= tz || gmtime_r(&t, &tm) == NULL || gmtime_r(&tn, &now) == NULL)
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

/* The table's rows: an entry's name, size and date; its icon */

static const char *row_text(struct kit_s *k, struct kit_widget_s *wg,
                            int row, int col, char *buf, int size)
{
  struct files_s *f = k->priv;
  struct fent_s *e;

  if (row >= f->n)
    {
      return NULL;
    }

  e = &f->ent[row];
  switch (col)
    {
      case COL_NAME:
        return e->name;

      case COL_SIZE:
        if (e->kind == K_DIR)
          {
            return NULL;
          }

        fmt_size(e->size, buf, size);
        return buf;

      default:
        fmt_date(e->mtime, buf, size);
        return buf;
    }
}

static void row_icon(struct kit_s *k, struct kit_widget_s *wg,
                     struct desk_ops_s *o, int row, int x, int y,
                     uint16_t fg, uint16_t bg)
{
  struct files_s *f = k->priv;

  if (row >= f->n)
    {
      return;
    }

  if (f->ent[row].kind == K_DIR)
    {
      icon_folder(o, x - 1, y - 1, C_FOLDER);
    }
  else
    {
      icon_page(o, x - 1, y - 1, f->ent[row].kind);
    }
}

/* The places: their names and symbols */

static const char *place_text(struct kit_s *k, struct kit_widget_s *wg,
                              int row, int col, char *buf, int size)
{
  struct files_s *f = k->priv;

  return row < f->nplaces ? f->place[row].name : NULL;
}

static void place_icon(struct kit_s *k, struct kit_widget_s *wg,
                       struct desk_ops_s *o, int row, int x, int y,
                       uint16_t fg, uint16_t bg)
{
  struct files_s *f = k->priv;

  if (row < f->nplaces)
    {
      icon_place(o, f->place[row].path, x, y, fg, bg);
    }
}

/* The path as buttons: the place it is in, then a button a folder (the
 * kit leaves out the first ones if they do not fit)
 */

static void crumbs_build(struct files_s *f)
{
  const char *p = f->path;
  size_t blen = 0;
  int base = -1;
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
  for (k = 0; k < n; k++)
    {
      f->crumbp[k] = f->crumb[k].label;
    }
}

/* The status bar's text: a message, the selection, or what is here */

static void status_text(struct files_s *f, char *buf, int n)
{
  char size[24];
  int sel = f->list.sel;

  if (f->msg[0] != '\0')
    {
      strlcpy(buf, f->msg, n);
    }
  else if (sel >= 0 && sel < f->n)
    {
      struct fent_s *e = &f->ent[sel];

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

/* Everything but the table's rows, from the state: the kit draws what
 * changed at its next flush
 */

static void files_sync(struct files_s *f)
{
  struct kit_s *k = &f->kit;
  bool sel = f->list.sel >= 0 && f->list.sel < f->n;
  char buf[160];
  int here = -1;
  int i;

  kit_enable(k, F_BACK, f->nhist > 0);
  kit_enable(k, F_UP, strcmp(f->path, "/") != 0);
  kit_enable(k, F_RENAME, sel);
  kit_enable(k, F_DELETE, sel);

  for (i = 0; i < f->nplaces; i++)
    {
      if (strcmp(f->path, f->place[i].path) == 0)
        {
          here = i;
        }
    }

  if (here != f->places.sel)
    {
      kit_list_select(k, F_PLACES, here);
    }

  status_text(f, buf, sizeof(buf));
  if (strcmp(buf, f->status) != 0)
    {
      strlcpy(f->status, buf, sizeof(f->status));
      kit_invalidate(k, F_STATUS);
    }

  kit_set_style(k, F_STATUS, f->msg[0] != '\0' && f->msgerr ?
                KIT_TEXT_ERROR : KIT_TEXT_DIM);
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

/* The folder's entries are new: the table, the path, the title */

static void files_shown(struct desk_win_s *w, const char *select)
{
  struct files_s *f = w->priv;

  f->list.n   = f->n;
  f->list.sel = ent_find(f, select);
  crumbs_build(f);
  kit_set_items(&f->kit, F_PATH, f->crumbp, f->ncrumbs);
  kit_list_changed(&f->kit, F_LIST);
  if (f->list.sel >= 0)
    {
      /* In the middle, if it can be */

      kit_list_scroll(&f->kit, F_LIST, f->list.sel * KIT_ROW_H -
                      kit_list_visible(&f->kit, F_LIST) * KIT_ROW_H / 2);
    }

  files_sync(f);
  set_title(w);
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
      files_sync(f);
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

  f->list.top = 0;
  f->msg[0]   = '\0';
  kit_dialog_close(&f->kit);
  files_shown(w, select);
  return 0;
}

/* Load the folder again, keeping the selection and the scroll */

static void files_reload(struct desk_win_s *w)
{
  struct files_s *f = w->priv;
  char *name = NULL;
  int ret;

  if (f->list.sel >= 0 && f->list.sel < f->n)
    {
      name = strdup(f->ent[f->list.sel].name);
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
      files_sync(f);
      free(p);
      return;
    }

  f->list.n   = f->n;
  f->list.sel = ent_find(f, name);
  free(name);
  kit_list_changed(&f->kit, F_LIST);
  files_sync(f);
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

  if (f->list.sel < 0 || f->list.sel >= f->n)
    {
      return;
    }

  e = &f->ent[f->list.sel];
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
  else if (e->kind == K_IMAGE)
    {
      desk_viewer_open(f->tmp);
    }
  else if (e->kind == K_TEXT || is_text(f->tmp))
    {
      desk_editor_open(f->tmp);
    }
  else
    {
      set_msg(f, true, "No application opens \"%s\"", e->name);
      files_sync(f);
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
      files_sync(f);
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

/* Ask: a new folder's name, a new name, or whether to delete */

static void ask(struct desk_win_s *w, int prompt)
{
  static const char * const create[2] =
  {
    "Create", "Cancel"
  };

  static const char * const rename[2] =
  {
    "Rename", "Cancel"
  };

  static const char * const delete[2] =
  {
    "Delete", "Cancel"
  };

  struct files_s *f = w->priv;
  struct fent_s *e = f->list.sel >= 0 && f->list.sel < f->n ?
                     &f->ent[f->list.sel] : NULL;
  char title[96];

  if (prompt != P_NEWDIR && e == NULL)
    {
      return;
    }

  f->msg[0] = '\0';
  switch (prompt)
    {
      case P_NEWDIR:
        kit_dialog(&f->kit, P_NEWDIR, KIT_DLG_PROMPT, "New folder",
                   "New Folder", create, 2);
        break;

      case P_RENAME:
        snprintf(title, sizeof(title), "Rename \"%s\"", e->name);
        kit_dialog(&f->kit, P_RENAME, KIT_DLG_PROMPT, title, e->name,
                   rename, 2);
        break;

      default:
        path_join(f->tmp, f->path, e->name);
        f->deltree = e->kind == K_DIR && !dir_empty(f->tmp);
        snprintf(title, sizeof(title), f->deltree ?
                 "Delete \"%s\" and all it holds?" : "Delete \"%s\"?",
                 e->name);
        kit_dialog(&f->kit, P_DELETE, KIT_DLG_CONFIRM | KIT_DLG_DANGER,
                   title, "It cannot be brought back.", delete, 2);
        break;
    }
}

/* The answer: do it (name: what the prompt's field said) */

static void answer(struct desk_win_s *w, int prompt, const char *text)
{
  struct files_s *f = w->priv;
  int sel = f->list.sel;
  char *name = NULL;
  int ret = 0;

  switch (prompt)
    {
      case P_NEWDIR:
        if (text[0] == '\0' || strchr(text, '/') != NULL)
          {
            return;
          }

        path_join(f->tmp, f->path, text);
        if (mkdir(f->tmp, 0777) < 0)
          {
            ret = -errno;
            set_msg(f, true, "Cannot make \"%s\": %s", text,
                    strerror(errno));
          }
        else
          {
            name = strdup(text);
          }
        break;

      case P_RENAME:
        {
          char *to;

          if (text[0] == '\0' || strchr(text, '/') != NULL || sel < 0 ||
              sel >= f->n || strcmp(text, f->ent[sel].name) == 0)
            {
              return;
            }

          to = malloc(PATH_MAX);
          if (to == NULL)
            {
              return;
            }

          path_join(f->tmp, f->path, f->ent[sel].name);
          path_join(to, f->path, text);

          /* (FAT's names ignore case: renaming to another case is fine) */

          if (access(to, F_OK) == 0 &&
              strcasecmp(text, f->ent[sel].name) != 0)
            {
              ret = -EEXIST;
              set_msg(f, true, "\"%s\" already exists", text);
            }
          else if (rename(f->tmp, to) < 0)
            {
              ret = -errno;
              set_msg(f, true, "Cannot rename: %s", strerror(errno));
            }
          else
            {
              name = strdup(text);
            }

          free(to);
        }
        break;

      case P_DELETE:
        if (sel < 0 || sel >= f->n)
          {
            return;
          }

        path_join(f->tmp, f->path, f->ent[sel].name);
        if (f->ent[sel].kind == K_DIR)
          {
            ret = remove_tree(f->tmp, 0);
          }
        else if (unlink(f->tmp) < 0)
          {
            ret = -errno;
          }

        if (ret < 0)
          {
            set_msg(f, true, "Cannot delete \"%s\": %s", f->ent[sel].name,
                    strerror(-ret));
          }
        else
          {
            /* Select what follows */

            int next = sel + 1 < f->n ? sel + 1 : sel - 1;

            name = next >= 0 ? strdup(f->ent[next].name) : NULL;
          }
        break;

      default:
        return;
    }

  if (ret < 0)
    {
      files_sync(f);
      return;
    }

  /* The folder changed: load it, select the result */

  if (dir_load(f, f->path) == 0)
    {
      f->list.n   = f->n;
      f->list.sel = ent_find(f, name);
      kit_list_changed(&f->kit, F_LIST);
      kit_list_show(&f->kit, F_LIST, f->list.sel);
    }

  free(name);
  files_sync(f);
}

/* Sorted by another column (the kit has set the list's): the selection
 * stays selected
 */

static void files_sort(struct files_s *f)
{
  char *name = f->list.sel >= 0 ? strdup(f->ent[f->list.sel].name) : NULL;

  f->sortcol  = f->list.sortcol;
  f->sortdesc = f->list.sortdesc;
  ents_sort(f);
  f->list.sel = ent_find(f, name);
  free(name);
  kit_list_changed(&f->kit, F_LIST);
  kit_list_show(&f->kit, F_LIST, f->list.sel);
}

static void copy_path(struct files_s *f)
{
  if (f->list.sel >= 0 && f->list.sel < f->n)
    {
      path_join(f->tmp, f->path, f->ent[f->list.sel].name);
      desk_clip_set(f->tmp, strlen(f->tmp));
      set_msg(f, false, "Copied the path of \"%s\"",
              f->ent[f->list.sel].name);
    }
}

/****************************************************************************
 * Private Functions: events
 ****************************************************************************/

/* Keys the table did not take */

static void files_keyev(struct desk_win_s *w, int key)
{
  struct files_s *f = w->priv;

  switch (key & ~KEY_SHIFT)
    {
      case 0x7f:
      case 0x08:
      case KEY_LEFT | KEY_CTRL:
        files_up(w);
        break;

      case KEY_DEL:
        ask(w, P_DELETE);
        break;

      case KEY_F2:
        ask(w, P_RENAME);
        break;

      case KEY_F5:
      case KEY_CTL('r'):
        files_reload(w);
        break;

      case KEY_CTL('n'):
        ask(w, P_NEWDIR);
        break;

      case KEY_CTL('c'):
        copy_path(f);
        break;

      case KEY_ESC:
        kit_list_select(&f->kit, F_LIST, -1);
        break;

      default:

        /* A letter: the next name that starts with it */

        if (key > ' ' && key < 0x7f && f->n > 0)
          {
            int j;

            for (j = 1; j <= f->n; j++)
              {
                int e = (f->list.sel + j + f->n) % f->n;

                if (tolower((uint8_t)f->ent[e].name[0]) == tolower(key))
                  {
                    kit_list_select(&f->kit, F_LIST, e);
                    f->msg[0] = '\0';
                    break;
                  }
              }
          }
        break;
    }

  files_sync(f);
}

static void files_event(struct kit_s *k, struct kit_widget_s *wg, int ev)
{
  struct desk_win_s *w = k->w;
  struct files_s *f = k->priv;
  int id = wg != NULL ? wg->id : 0;

  switch (ev)
    {
      case KIT_EV_CLICK:
        switch (id)
          {
            case F_BACK:
              files_back(w);
              break;

            case F_UP:
              files_up(w);
              break;

            case F_REFRESH:
              files_reload(w);
              break;

            case F_NEWDIR:
              ask(w, P_NEWDIR);
              break;

            case F_RENAME:
              ask(w, P_RENAME);
              break;

            case F_DELETE:
              ask(w, P_DELETE);
              break;

            case F_PATH:
              if (k->arg >= 0 && k->arg < f->ncrumbs)
                {
                  char *p = malloc(PATH_MAX);
                  char *child = k->arg + 1 < f->ncrumbs ?
                                strdup(f->crumb[k->arg + 1].label) : NULL;

                  if (p != NULL)
                    {
                      snprintf(p, PATH_MAX, "%.*s", f->crumb[k->arg].len,
                               f->path);
                      files_go(w, p, true, child);
                    }

                  free(child);
                  free(p);
                }
              break;

            default:
              break;
          }
        break;

      case KIT_EV_CHANGE:
        if (id == F_PLACES && f->places.sel >= 0)
          {
            char *p = strdup(f->place[f->places.sel].path);

            if (p != NULL)
              {
                files_go(w, p, true, NULL);
                free(p);
              }

            kit_focus(k, F_LIST);
          }
        else if (id == F_LIST)
          {
            f->msg[0] = '\0';
            files_sync(f);
          }
        break;

      case KIT_EV_ACTIVATE:
        if (id == F_LIST)
          {
            files_open_sel(w);
          }
        break;

      case KIT_EV_SORT:
        files_sort(f);
        break;

      case KIT_EV_MOUSE:

        /* The right button on a row: it is selected, and its menu */

        if (id == F_LIST && (k->mbuttons & NX_MOUSE_RIGHTBUTTON))
          {
            int row = kit_list_at(k, F_LIST, k->mx, k->my);

            if (row >= 0)
              {
                kit_list_select(k, F_LIST, row);
              }

            kit_menu(k, M_ROW, k->mx, k->my, g_rowmenu, 6,
                     row < 0 ? 0x17 : 0);
            files_sync(f);
          }
        break;

      case KIT_EV_MENU:
        switch (k->arg)
          {
            case M_OPEN:
              files_open_sel(w);
              break;

            case M_RENAME:
              ask(w, P_RENAME);
              break;

            case M_DELETE:
              ask(w, P_DELETE);
              break;

            case M_COPYPATH:
              copy_path(f);
              files_sync(f);
              break;

            case M_NEWDIR:
              ask(w, P_NEWDIR);
              break;

            default:
              break;
          }
        break;

      case KIT_EV_DIALOG:
        if (k->arg == KIT_ANS_OK)
          {
            answer(w, k->key, k->answer);
          }
        break;

      case KIT_EV_KEY:
        if (!kit_dialog_open(k))
          {
            files_keyev(w, k->key);
            k->key = 0;
          }
        break;

      default:
        break;
    }
}

static void files_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct files_s *f = w->priv;

  if (f != NULL)
    {
      kit_mouse(&f->kit, x, y, buttons);
    }
}

static void files_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct files_s *f = w->priv;

  if (f != NULL)
    {
      kit_key(&f->kit, ch, n);
    }
}

static void files_tick(struct desk_win_s *w)
{
  struct files_s *f = w->priv;
  uint32_t sig;
  int count;

  if (f == NULL)
    {
      return;
    }

  kit_tick(&f->kit);
  if (kit_dialog_open(&f->kit) || f->kit.down != KIT_NONE)
    {
      return;
    }

  sig = dir_sig(f->path, &count);
  if (count != f->n || sig != f->sig)
    {
      files_reload(w);
      kit_flush(&f->kit);
    }
}

static void files_draw(struct desk_win_s *w)
{
  struct files_s *f = w->priv;

  if (f != NULL)
    {
      files_sync(f);
      kit_redraw(&f->kit);
    }
}

static void files_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  struct files_s *f = w->priv;

  if (f != NULL)
    {
      kit_expose(&f->kit, x, y, cw, ch);
    }
}

static void files_resize(struct desk_win_s *w)
{
  struct files_s *f = w->priv;

  if (f != NULL)
    {
      kit_resize(&f->kit);
    }
}

static void files_focus(struct desk_win_s *w)
{
  struct files_s *f = w->priv;

  if (f != NULL)
    {
      kit_activate(&f->kit);
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

/* The window's widgets */

static void files_build(struct desk_win_s *w, struct files_s *f)
{
  struct kit_s *k = &f->kit;
  struct kit_widget_s *wg;

  kit_init(k, w, f->wg, NWG, files_event);
  k->priv = f;

  f->list.sel     = -1;
  f->list.ncols   = 3;
  f->list.cols    = g_cols;
  f->list.text    = row_text;
  f->list.icon    = row_icon;
  f->list.empty   = "Folder is empty";
  f->places.sel   = -1;
  f->places.n     = f->nplaces;
  f->places.text  = place_text;
  f->places.icon  = place_icon;
  f->places.sortcol = -1;

  kit_col(k, KIT_BOX_WINDOW);
    kit_row(k, KIT_BOX_BAR);
      kit_button(k, F_BACK, NULL, KIT_ICON_BACK, KIT_BTN_NORMAL);
      kit_tip(k, "Back");
      kit_button(k, F_UP, NULL, KIT_ICON_UP, KIT_BTN_NORMAL);
      kit_tip(k, "The folder it is in");
      kit_crumbs(k, F_PATH, f->crumbp, 0);
      kit_grow(k, 1);
      kit_button(k, F_REFRESH, NULL, KIT_ICON_REFRESH, KIT_BTN_NORMAL);
      kit_tip(k, "Read the folder again");
    kit_end(k);

    wg = kit_row(k, KIT_BOX_NONE);
    wg->gap = 0;
    kit_grow(k, 1);
      wg = kit_col(k, KIT_BOX_SIDEBAR);
      wg->gap = 6;
      kit_size(k, SIDE_W, 0);
        kit_label(k, 0, "Places", KIT_TEXT_SMALL | KIT_TEXT_DIM);
        kit_list(k, F_PLACES, &f->places, KIT_LIST_SIDEBAR)->flags |=
          KIT_F_NOFOCUS;
        kit_grow(k, 1);
      kit_end(k);

      wg = kit_col(k, KIT_BOX_NONE);
      wg->gap = 0;
      kit_grow(k, 1);
        kit_list(k, F_LIST, &f->list, KIT_LIST_TABLE);
        kit_grow(k, 1);
        wg = kit_row(k, KIT_BOX_STATUS);
        wg->ph = BAR_H;
        wg->gap = 8;
          kit_label(k, F_STATUS, f->status, KIT_TEXT_DIM);
          kit_grow(k, 1);
          kit_button(k, F_NEWDIR, "New Folder", KIT_ICON_NONE,
                     KIT_BTN_NORMAL);
          kit_button(k, F_RENAME, "Rename", KIT_ICON_NONE, KIT_BTN_NORMAL);
          kit_button(k, F_DELETE, "Delete", KIT_ICON_NONE, KIT_BTN_NORMAL);
        kit_end(k);
      kit_end(k);
    kit_end(k);
  kit_end(k);

  kit_focus(k, F_LIST);
  kit_layout(k);
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

  w->priv = f;

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

  files_build(w, f);
  if (f->nplaces == 0 || files_go(w, f->place[0].path, false, NULL) < 0)
    {
      files_go(w, "/", false, NULL);
    }

  kit_flush(&f->kit);
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
  .name   = "Files",
  .icon   = files_icon,
  .open   = files_open,
  .draw   = files_draw,
  .expose = files_expose,
  .resize = files_resize,
  .tick   = files_tick,
  .mouse  = files_mouse,
  .key    = files_key,
  .focus  = files_focus,
  .close  = files_close,
  .single = true,
};
