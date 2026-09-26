/****************************************************************************
 * apps/graphics/fpcdesk/desk_apps.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The applications: the table of them, the icon tile they share, and the
 * small ones (the launcher's entry, Settings, About).  The others have a
 * file each: desk_term.c, desk_files.c, desk_edit.c, desk_monitor.c.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/boardctl.h>

#include <nuttx/nx/nxfonts.h>

#include "nshlib/nshlib.h"
#include "desk.h"

static struct desk_ops_s g_o;

/****************************************************************************
 * Public Functions: icons
 ****************************************************************************/

/* A round tile with a top-to-bottom gradient, a hairline of light at the
 * top: the base of every icon
 */

void icon_tile(struct desk_ops_s *o, int x, int y, int s,
                      uint16_t top, uint16_t bottom, uint8_t a)
{
  ops_rrect_vgrad(o, x, y, s, s, LUT_R12, 12, top, bottom, a);
  ops_blend(o, x + 10, y, s - 20, 1, C_WHITE, (uint8_t)(a * 60 / 255));
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void icon_apps(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  int i;
  int j;

  icon_tile(o, x, y, s, RGB(0x6a, 0x70, 0x80), RGB(0x3a, 0x3e, 0x4a), a);
  for (j = 0; j < 3; j++)
    {
      for (i = 0; i < 3; i++)
        {
          int cx = x + s / 2 + (i - 1) * s / 4;
          int cy = y + s / 2 + (j - 1) * s / 4;

          ops_text_centre(o, &g_desk_font_title, cx,
                          cy - text_height(&g_desk_font_title) / 2 - 1,
                          DESK_CH_BULLET, C_WHITE, a);
        }
    }
}

static void icon_settings(struct desk_ops_s *o, int x, int y, int s,
                          uint8_t a)
{
  static const uint8_t knob[3] =
  {
    30, 68, 45
  };

  int i;

  icon_tile(o, x, y, s, RGB(0xa4, 0xaa, 0xb2), RGB(0x58, 0x5c, 0x64), a);
  for (i = 0; i < 3; i++)
    {
      int ly = y + s * (i + 1) / 4;
      int kx = x + s / 6 + (s * 2 / 3) * knob[i] / 100;

      ops_blend(o, x + s / 6, ly - 1, s * 2 / 3, 2, C_WHITE,
                (uint8_t)(a * 200 / 255));
      ops_disc(o, kx, ly, LUT_R6, 6, C_WHITE, a);
      ops_disc(o, kx, ly, LUT_R6, 6, RGB(0x30, 0x30, 0x38),
               (uint8_t)(a / 5));
    }
}

static void icon_about(struct desk_ops_s *o, int x, int y, int s,
                       uint8_t a)
{
  const struct desk_font_s *f = s >= 64 ? &g_desk_font_title :
                                          &g_desk_font_uib;

  icon_tile(o, x, y, s, RGB(0x4a, 0x8c, 0xff), RGB(0x1c, 0x48, 0xc8), a);
  ops_text_centre(o, f, x + s / 2, y + s / 2 - text_height(f) / 2, "FPC",
                  C_WHITE, a);
}

static const char * const g_wallnames[3] =
{
  "Aurora", "Sunset", "Graphite"
};

#define SWATCH_W   132
#define SWATCH_H   88

static void settings_draw(struct desk_win_s *w)
{
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  int y = TITLE_H;
  int i;

  ops_reset(&g_o);
  ops_fill(&g_o, 0, y, w->csize.w, w->csize.h, C_WINDOW_BG);
  ops_text(&g_o, &g_desk_font_title, 20, y + 16, "Wallpaper", C_TEXT, 255);
  ops_text(&g_o, &g_desk_font_ui, 20, y + 44,
           "Drawn by the GPU: a gradient and soft glows", C_TEXT_DIM, 255);
  ops_draw(hwnd, &g_o);

  for (i = 0; i < 3; i++)
    {
      int x = 20 + i * (SWATCH_W + 16);
      int sy = y + 78;

      if (g_desk.wallstyle == i)
        {
          ops_rrect(&g_o, x - 4, sy - 4, SWATCH_W + 8, SWATCH_H + 8, LUT_R12,
                    12, C_ACCENT, 255);
        }

      desk_wallpaper_preview(&g_o, i, x, sy, SWATCH_W, SWATCH_H);
      ops_text_centre(&g_o, g_desk.wallstyle == i ? &g_desk_font_uib :
                      &g_desk_font_ui, x + SWATCH_W / 2, sy + SWATCH_H + 10,
                      g_wallnames[i], g_desk.wallstyle == i ? C_TEXT :
                      C_TEXT_DIM, 255);
      ops_draw(hwnd, &g_o);
    }
}

static void settings_mouse(struct desk_win_s *w, int x, int y,
                           uint8_t buttons)
{
  static uint8_t last;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) && !(last & NX_MOUSE_LEFTBUTTON);

  last = buttons;
  if (!press)
    {
      return;
    }

  int i;

  for (i = 0; i < 3; i++)
    {
      int sx = 20 + i * (SWATCH_W + 16);

      if (x >= sx && x < sx + SWATCH_W && y >= 78 && y < 78 + SWATCH_H + 26 &&
          g_desk.wallstyle != i)
        {
          g_desk.wallstyle = i;
          desk_wallpaper_draw();
          settings_draw(w);
        }
    }
}

static struct desk_win_s *settings_open(void)
{
  struct desk_win_s *w = desk_win_open(APP_SETTINGS, "Settings", 468, 230);

  if (w != NULL)
    {
      settings_draw(w);
    }

  return w;
}

static void about_draw(struct desk_win_s *w)
{
  static const char * const rows[][2] =
  {
    { "Processor", "RP2350, 2 x Cortex-M33" },
    { "Memory",    "520 KB SRAM, 8 MB PSRAM" },
    { "Graphics",  "Tang Nano 20K, 1024 x 768" },
    { "Display",   "Composited: GPU protocol v4" },
    { "System",    "NuttX, NX" },
  };

  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  int cw = w->csize.w;
  int y = TITLE_H;
  int i;

  ops_reset(&g_o);
  ops_fill(&g_o, 0, y, cw, w->csize.h, C_WINDOW_BG);
  icon_about(&g_o, cw / 2 - 36, y + 22, 72, 255);
  ops_text_centre(&g_o, &g_desk_font_title, cw / 2, y + 106, "FPC",
                  C_TEXT, 255);
  ops_text_centre(&g_o, &g_desk_font_ui, cw / 2, y + 134,
                  "A small computer", C_TEXT_DIM, 255);
  ops_draw(hwnd, &g_o);

  for (i = 0; i < 5; i++)
    {
      int ry = y + 170 + i * 24;

      ops_text(&g_o, &g_desk_font_ui, cw / 2 - 12 -
               text_width(&g_desk_font_ui, rows[i][0]), ry, rows[i][0],
               C_TEXT_DIM, 255);
      ops_text(&g_o, &g_desk_font_ui, cw / 2 + 4, ry, rows[i][1], C_TEXT,
               255);
      ops_draw(hwnd, &g_o);
    }
}

static struct desk_win_s *about_open(void)
{
  struct desk_win_s *w = desk_win_open(APP_ABOUT, "About", 400, 300);

  if (w != NULL)
    {
      about_draw(w);
    }

  return w;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

static const struct desk_app_s g_app_launcher =
{
  .name = "Applications",
  .icon = icon_apps,
};

static const struct desk_app_s g_app_settings =
{
  .name   = "Settings",
  .icon   = icon_settings,
  .open   = settings_open,
  .draw   = settings_draw,
  .mouse  = settings_mouse,
  .single = true,
};

static const struct desk_app_s g_app_about =
{
  .name   = "About",
  .icon   = icon_about,
  .open   = about_open,
  .draw   = about_draw,
  .single = true,
};

const struct desk_app_s * const g_apps[APP_COUNT] =
{
  [APP_LAUNCHER] = &g_app_launcher,
  [APP_TERMINAL] = &g_app_terminal,
  [APP_FILES]    = &g_app_files,
  [APP_EDITOR]   = &g_app_editor,
  [APP_MONITOR]  = &g_app_monitor,
  [APP_SETTINGS] = &g_app_settings,
  [APP_ABOUT]    = &g_app_about,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* The load, once a second: /proc/cpuload, /proc/meminfo (all heaps) */

void desk_load_sample(void)
{
  char line[128];
  FILE *f;
  int load = 0;

  f = fopen("/proc/cpuload", "r");
  if (f != NULL)
    {
      if (fgets(line, sizeof(line), f) != NULL)
        {
          load = atoi(line);
        }

      fclose(f);
    }

  g_desk.cpuload = load;
  memmove(g_desk.cpuhist, g_desk.cpuhist + 1, sizeof(g_desk.cpuhist) - 1);
  g_desk.cpuhist[sizeof(g_desk.cpuhist) - 1] = (uint8_t)load;

  f = fopen("/proc/meminfo", "r");
  if (f != NULL)
    {
      unsigned long total = 0;
      unsigned long used = 0;

      while (fgets(line, sizeof(line), f) != NULL)
        {
          unsigned long a;
          unsigned long u;

          if (sscanf(line, "%lu %lu", &a, &u) == 2)
            {
              total += a;
              used  += u;
            }
        }

      fclose(f);
      g_desk.memtotal = total / 1024;
      g_desk.memused  = used / 1024;
    }
}
