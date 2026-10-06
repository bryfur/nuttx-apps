/****************************************************************************
 * apps/graphics/fpcdesk/desk_wasm.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebAssembly apps in the launcher (docs/wasm.md): each directory of
 * CONFIG_FPCWASM_APPDIR (/mnt/sd0/apps) with an app.wasm or app.aot and a
 * manifest whose kind is game or app is a tile after the built-in
 * applications, found again each time the launcher opens (an app copied
 * to the SD card shows without a restart).  Its icon is icon.bmp (24 or
 * 32 bits, drawn on a tile of the manifest's colour), or a tile of that
 * colour with the name's first letters.  Opening it runs the `wasm`
 * program (apps/graphics/fpcwasm) on its directory, as a task of its own
 * that takes the display and the keyboard as the other games do; an app
 * of kind window runs beside the desktop's windows, in windows of its own
 * (fpc_win_*).
 *
 * The manifest: lines of "key value": name, kind (game, app, window,
 * cli), api, icon (default icon.bmp), color (the tile's, RRGGBB).
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/stat.h>

#include <nuttx/lib/builtin.h>

#include "desk.h"
#include "desk_wasm.h"

#ifdef CONFIG_GRAPHICS_FPCWASM

#ifdef CONFIG_FPC_GPU
void fpc_gpu_game_prepare(void);        /* (drivers/gpu: fpc_gpu.h) */
#endif

#define WASM_ICON   64                  /* The launcher's icon size */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wasm_app_s
{
  char dir[NAME_MAX + 1];               /* Its directory's name */
  char name[32];
  char icon[32];                        /* Its icon file */
  uint16_t colour;                      /* The tile's */
  NXWINDOW surf;                        /* The icon's pixels, or NULL */
  volatile bool lost;                   /* ... gone (the GPU's memory) */
  bool noicon;                          /* No icon file to be had */
  bool window;                          /* kind window: a desktop app */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct wasm_app_s g_wa[DESK_WASM_MAX];
static int g_nwa;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool has_file(const char *dir, const char *name)
{
  char path[PATH_MAX];
  struct stat st;

  snprintf(path, sizeof(path), "%s/%s/%s", CONFIG_FPCWASM_APPDIR, dir, name);
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* The manifest; false if the app is not for the launcher */

static bool read_manifest(struct wasm_app_s *a)
{
  char path[PATH_MAX];
  char line[96];
  bool shown = true;
  FILE *fp;

  strlcpy(a->name, a->dir, sizeof(a->name));
  strlcpy(a->icon, "icon.bmp", sizeof(a->icon));
  a->colour = RGB(0x4a, 0x50, 0x68);

  snprintf(path, sizeof(path), "%s/%s/manifest", CONFIG_FPCWASM_APPDIR,
           a->dir);
  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return true;                      /* (a game by default) */
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      char *k = line;
      char *v;
      char *e;

      while (isspace((unsigned char)*k))
        {
          k++;
        }

      for (v = k; *v != '\0' && !isspace((unsigned char)*v); v++);
      if (*v != '\0')
        {
          *v++ = '\0';
        }

      while (isspace((unsigned char)*v))
        {
          v++;
        }

      for (e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); e--);
      *e = '\0';

      if (strcmp(k, "name") == 0 && *v != '\0')
        {
          strlcpy(a->name, v, sizeof(a->name));
        }
      else if (strcmp(k, "kind") == 0)
        {
          shown = strcmp(v, "cli") != 0;
          a->window = strcmp(v, "window") == 0;
        }
      else if (strcmp(k, "icon") == 0 && *v != '\0' && strchr(v, '/') == NULL)
        {
          strlcpy(a->icon, v, sizeof(a->icon));
        }
      else if (strcmp(k, "color") == 0 && strlen(v) == 6)
        {
          unsigned long c = strtoul(v, NULL, 16);

          a->colour = RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
        }
    }

  fclose(fp);
  return shown;
}

static int compare(const void *a, const void *b)
{
  return strcasecmp(((const struct wasm_app_s *)a)->name,
                    ((const struct wasm_app_s *)b)->name);
}

static uint32_t le32(const uint8_t *p)
{
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

/* icon.bmp (uncompressed, 24 or 32 bits, any size: sampled to 64 x 64,
 * transparent pixels on the tile's colour) into a surface of its own
 */

static void icon_load(struct wasm_app_s *a)
{
  char path[PATH_MAX];
  uint8_t hdr[54];
  uint16_t *px = NULL;
  uint8_t *row = NULL;
  uint32_t off;
  int32_t w;
  int32_t h;
  int bpp;
  size_t stride;
  bool topdown;
  int fd;
  int y;
  int x;

  a->noicon = true;
  snprintf(path, sizeof(path), "%s/%s/%s", CONFIG_FPCWASM_APPDIR, a->dir,
           a->icon);
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return;
    }

  if (read(fd, hdr, sizeof(hdr)) != sizeof(hdr) || hdr[0] != 'B' ||
      hdr[1] != 'M' || le32(hdr + 30) != 0)
    {
      goto out;
    }

  off = le32(hdr + 10);
  w   = (int32_t)le32(hdr + 18);
  h   = (int32_t)le32(hdr + 22);
  bpp = hdr[28];
  topdown = h < 0;
  h = topdown ? -h : h;
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024 || (bpp != 24 && bpp != 32))
    {
      goto out;
    }

  stride = (((size_t)w * bpp / 8) + 3) & ~(size_t)3;
  px  = malloc(WASM_ICON * WASM_ICON * 2);
  row = malloc(stride);
  if (px == NULL || row == NULL)
    {
      goto out;
    }

  for (y = 0; y < WASM_ICON; y++)
    {
      int sy = y * h / WASM_ICON;
      off_t pos = off + stride * (topdown ? sy : h - 1 - sy);

      if (lseek(fd, pos, SEEK_SET) != pos ||
          read(fd, row, stride) != (ssize_t)stride)
        {
          goto out;
        }

      for (x = 0; x < WASM_ICON; x++)
        {
          const uint8_t *p = row + (x * w / WASM_ICON) * (bpp / 8);

          px[y * WASM_ICON + x] = bpp == 32 && p[3] < 128 ? a->colour :
                                  RGB(p[2], p[1], p[0]);
        }
    }

  if (a->surf == NULL)
    {
      a->surf = desk_surface_open(WASM_ICON, WASM_ICON, &a->lost);
    }

  if (a->surf != NULL)
    {
      a->lost = false;
      desk_surface_put(a->surf, 0, 0, WASM_ICON, WASM_ICON, px,
                       WASM_ICON * 2);
      a->noicon = false;
    }

out:
  free(row);
  free(px);
  close(fd);
}

static void forget(void)
{
  int i;

  for (i = 0; i < g_nwa; i++)
    {
      if (g_wa[i].surf != NULL)
        {
          desk_surface_close(g_wa[i].surf);
        }
    }

  memset(g_wa, 0, sizeof(g_wa));
  g_nwa = 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* The apps installed now (the launcher opens) */

void desk_wasm_scan(void)
{
  struct wasm_app_s found[DESK_WASM_MAX];
  struct dirent *e;
  int n = 0;
  int i;
  DIR *d;

  d = opendir(CONFIG_FPCWASM_APPDIR);
  if (d != NULL)
    {
      while ((e = readdir(d)) != NULL && n < DESK_WASM_MAX)
        {
          struct wasm_app_s *a = &found[n];

          if (e->d_name[0] == '.' || strlen(e->d_name) > NAME_MAX ||
              (!has_file(e->d_name, "app.wasm") &&
               !has_file(e->d_name, "app.aot")))
            {
              continue;
            }

          memset(a, 0, sizeof(*a));
          strlcpy(a->dir, e->d_name, sizeof(a->dir));
          if (read_manifest(a))
            {
              n++;
            }
        }

      closedir(d);
    }

  qsort(found, n, sizeof(found[0]), compare);

  /* The same as before: keep the icons made */

  if (n == g_nwa)
    {
      for (i = 0; i < n; i++)
        {
          if (strcmp(found[i].dir, g_wa[i].dir) != 0 ||
              strcmp(found[i].name, g_wa[i].name) != 0 ||
              strcmp(found[i].icon, g_wa[i].icon) != 0 ||
              found[i].colour != g_wa[i].colour ||
              found[i].window != g_wa[i].window)
            {
              break;
            }
        }

      if (i == n)
        {
          return;
        }
    }

  forget();
  memcpy(g_wa, found, sizeof(found[0]) * n);
  g_nwa = n;
}

int desk_wasm_count(void)
{
  return g_nwa;
}

const char *desk_wasm_name(int i)
{
  return i >= 0 && i < g_nwa ? g_wa[i].name : "";
}

/* Its icon at (x, y), s pixels: the file's, or a tile with its initials */

void desk_wasm_icon(struct desk_ops_s *o, int i, int x, int y, int s,
                    uint8_t a)
{
  struct wasm_app_s *w;
  struct nx_gpuop_s *op;
  char init[3];
  const char *n;
  int k = 0;

  if (i < 0 || i >= g_nwa)
    {
      return;
    }

  w = &g_wa[i];
  if (s == WASM_ICON && !w->noicon && (w->surf == NULL || w->lost))
    {
      icon_load(w);
    }

  if (s == WASM_ICON && w->surf != NULL && !w->noicon && !w->lost)
    {
      op         = ops_add(o);
      op->op     = NX_GPUOP_BLEND;
      op->x      = x;
      op->y      = y;
      op->w      = s;
      op->h      = s;
      op->src    = w->surf;
      op->sx     = 0;
      op->sy     = 0;
      op->alpha  = a;
      op->mode   = NX_BLEND_SRC_RGB | NX_BLEND_MOD_LUT;
      op->slot   = LUT_R12;
      op->p[0]   = x + 12;
      op->p[1]   = y + 12;
      op->p[2]   = x + s - 1 - 12;
      op->p[3]   = y + s - 1 - 12;
      return;
    }

  /* The name's first letters (of its first two words) */

  for (n = w->name; *n != '\0' && k < 2; n++)
    {
      if (isalnum((unsigned char)*n) &&
          (n == w->name || !isalnum((unsigned char)n[-1])))
        {
          init[k++] = (char)toupper((unsigned char)*n);
        }
    }

  init[k] = '\0';
  icon_tile(o, x, y, s, w->colour,
            (uint16_t)((w->colour >> 1) & 0x7bef), a);
  ops_text_centre(o, s >= 64 ? &g_desk_font_title : &g_desk_font_uib,
                  x + s / 2, y + s / 2 -
                  text_height(s >= 64 ? &g_desk_font_title :
                              &g_desk_font_uib) / 2 - 1,
                  init, C_WHITE, a);
}

/* Run it: the wasm program on its directory */

bool desk_wasm_launch(int i)
{
  char dir[PATH_MAX];
  FAR char *argv[4];
  const struct builtin_s *b;
  int index;
  pid_t pid;
  int ret;

  if (i < 0 || i >= g_nwa)
    {
      return false;
    }

#ifdef CONFIG_FPC_GPU
  if (!g_wa[i].window)
    {
      fpc_gpu_game_prepare();           /* (a game: the display its own) */
    }
#endif

  snprintf(dir, sizeof(dir), "%s/%s", CONFIG_FPCWASM_APPDIR, g_wa[i].dir);
  argv[0] = "wasm";
  argv[1] = "run";
  argv[2] = dir;
  argv[3] = NULL;

  index = builtin_isavail("wasm");
  b     = index >= 0 ? builtin_for_index(index) : NULL;
  if (b != NULL)
    {
      pid = task_create(g_wa[i].name, b->priority, b->stacksize, b->main,
                        argv + 1);
      ret = pid < 0 ? errno : 0;
    }
  else
    {
      ret = posix_spawnp(&pid, "wasm", NULL, NULL, argv, NULL);
    }

  if (ret != 0)
    {
      syslog(LOG_WARNING, "desk: %s: wasm did not start: %d\n",
             g_wa[i].name, ret);
      return false;
    }

  syslog(LOG_INFO, "desk: %s (%d): wasm run %s\n", g_wa[i].name, pid, dir);
  return true;
}

#endif /* CONFIG_GRAPHICS_FPCWASM */
