/****************************************************************************
 * apps/graphics/fpcdesk/desk_apps.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The applications: their icons (drawn by the GPU, no bitmaps) and their
 * windows.
 *
 *   Terminal   NSH in an NxTerm window
 *   Monitor    CPU load (with its last minute), memory, uptime
 *   Settings   the wallpaper
 *   About      the machine
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

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TERM_COLS     80
#define TERM_ROWS     24
#define TERM_MARGIN   8               /* Below the last row: the round corners */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct desk_ops_s g_o;

/****************************************************************************
 * Private Functions: icons
 ****************************************************************************/

/* A round tile with a top-to-bottom gradient, a hairline of light at the
 * top: the base of every icon
 */

static void icon_tile(struct desk_ops_s *o, int x, int y, int s,
                      uint16_t top, uint16_t bottom, uint8_t a)
{
  ops_rrect_vgrad(o, x, y, s, s, LUT_R12, 12, top, bottom, a);
  ops_blend(o, x + 10, y, s - 20, 1, C_WHITE, (uint8_t)(a * 60 / 255));
}

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

static void icon_terminal(struct desk_ops_s *o, int x, int y, int s,
                          uint8_t a)
{
  const struct desk_font_s *f = s >= 64 ? &g_desk_font_title :
                                          &g_desk_font_uib;

  icon_tile(o, x, y, s, RGB(0x46, 0x46, 0x4c), RGB(0x16, 0x16, 0x1a), a);
  ops_blend(o, x + 6, y + s / 5, s - 12, 1, C_WHITE, (uint8_t)(a / 6));
  ops_text(o, f, x + s / 6, y + s / 2 - text_height(f) / 2 + 2, ">_",
           RGB(0x3d, 0xdc, 0x84), a);
}

static void icon_monitor(struct desk_ops_s *o, int x, int y, int s,
                         uint8_t a)
{
  static const uint8_t bars[4] =
  {
    40, 70, 55, 85
  };

  int bw = s / 9;
  int i;

  icon_tile(o, x, y, s, RGB(0x22, 0xd0, 0xb0), RGB(0x08, 0x76, 0x88), a);
  for (i = 0; i < 4; i++)
    {
      int h = (s * 6 / 10) * bars[i] / 100;
      int bx = x + s / 5 + i * (bw + bw / 2 + 1);

      ops_blend(o, bx, y + s * 4 / 5 - h, bw, h, C_WHITE, (uint8_t)(a * 235
                / 255));
    }

  ops_blend(o, x + s / 6, y + s * 4 / 5, s * 2 / 3, 1, C_WHITE,
            (uint8_t)(a * 150 / 255));
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

/****************************************************************************
 * Private Functions: Terminal
 ****************************************************************************/

static int term_task(int argc, char *argv[])
{
  int fd;

  if (argc < 2)
    {
      return EXIT_FAILURE;
    }

  fd = open(argv[1], O_RDWR);
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  fflush(stdout);
  fflush(stderr);
  dup2(fd, 0);
  dup2(fd, 1);
  dup2(fd, 2);
  if (fd > 2)
    {
      close(fd);
    }

  printf("FPC  " DESK_CH_MIDDOT "  NuttX shell\n\n");
  return nsh_consolemain(0, NULL);
}

static struct desk_win_s *term_open(void)
{
  static bool nshinit;
  static int minor;
  struct boardioc_nxterm_create_s create;
  const struct nx_font_s *fs;
  struct desk_win_s *w;
  NXHANDLE hfont;
  char path[24];
  char *argv[2];
  int cw = 8;
  int ch = 13;

  hfont = nxf_getfonthandle(NXFONT_DEFAULT);
  if (hfont != NULL && (fs = nxf_getfontset(hfont)) != NULL)
    {
      cw = fs->mxwidth;
      ch = fs->mxheight + CONFIG_NXTERM_LINESEPARATION;
    }

  w = desk_win_open(APP_TERMINAL, "Terminal", TERM_COLS * cw + 12,
                    TERM_ROWS * ch + TERM_MARGIN);
  if (w == NULL)
    {
      return NULL;
    }

  ops_reset(&g_o);
  ops_fill(&g_o, 0, TITLE_H, w->csize.w, w->csize.h, C_TERM_BG);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
  desk_win_sync(w);

  if (!nshinit)
    {
      nsh_initialize();
      nshinit = true;
    }

  memset(&create, 0, sizeof(create));
  create.hwnd           = w->hwnd;
  create.wndo.wcolor[0] = C_TERM_BG;
  create.wndo.fcolor[0] = C_TERM_FG;
  create.wndo.wsize     = w->csize;
  create.wndo.fontid    = NXFONT_DEFAULT;
  create.type           = BOARDIOC_XTERM_FRAMED;
  create.minor          = minor;
  if (boardctl(BOARDIOC_NXTERM, (uintptr_t)&create) < 0)
    {
      fprintf(stderr, "desk: no terminal: %d\n", errno);
      return w;
    }

  w->nxterm = create.nxterm;
  w->minor  = minor++;

  snprintf(path, sizeof(path), "/dev/nxterm%d", w->minor);
  argv[0] = path;
  argv[1] = NULL;
  w->pid  = task_create("Terminal", CONFIG_FPCDESK_TERM_PRIORITY,
                        CONFIG_FPCDESK_TERM_STACKSIZE, term_task, argv);
  return w;
}

static void term_close(struct desk_win_s *w)
{
  char path[24];

  if (w->pid >= 0)
    {
      task_delete(w->pid);
      w->pid = -1;
    }

  if (w->nxterm != NULL)
    {
      snprintf(path, sizeof(path), "/dev/nxterm%d", w->minor);
      unlink(path);
      w->nxterm = NULL;
    }
}

/****************************************************************************
 * Private Functions: Monitor
 ****************************************************************************/

static int win_count(void)
{
  struct desk_win_s *t;
  int n = 0;

  for (t = g_desk.wins; t != NULL; t = t->next)
    {
      n += !t->closing;
    }

  return n;
}

/* A card: a slightly lighter round rectangle with a caption */

static void card(int x, int y, int w, int h, const char *caption)
{
  ops_rrect(&g_o, x, y, w, h, LUT_R12, 12, C_CARD, 255);
  ops_text(&g_o, &g_desk_font_small, x + 14, y + 10, caption, C_TEXT_DIM,
           255);
}

static void monitor_draw(struct desk_win_s *w)
{
  char buf[40];
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  int cw = w->csize.w;
  int y = TITLE_H;
  int gx;
  int gw;
  int i;
  unsigned pct;

  ops_reset(&g_o);
  ops_fill(&g_o, 0, y, cw, w->csize.h, C_WINDOW_BG);

  /* Processor: the figure, and the last minute as bars */

  card(12, y + 12, cw - 24, 150, "PROCESSOR");
  snprintf(buf, sizeof(buf), "%d%%", g_desk.cpuload);
  ops_text(&g_o, &g_desk_font_big, 26, y + 26, buf, C_TEXT, 255);
  ops_text(&g_o, &g_desk_font_ui, 30, y + 88, "2 x Cortex-M33", C_TEXT_DIM,
           255);
  ops_draw(hwnd, &g_o);

  gx = 170;
  gw = cw - 24 - gx + 12 - 14;
  ops_blend(&g_o, gx, y + 40, gw, 108, C_BLACK, 70);
  for (i = 1; i < 4; i++)
    {
      ops_blend(&g_o, gx, y + 40 + i * 27, gw, 1, C_WHITE, 18);
    }

  for (i = 0; i < 60; i++)
    {
      int v = g_desk.cpuhist[i];
      int h = v * 104 / 100;
      int bx = gx + 2 + i * (gw - 4) / 60;
      int bw = (gw - 4) / 60 - 1;

      if (h > 0)
        {
          ops_vgrad(&g_o, bx, y + 146 - h, bw < 1 ? 1 : bw, h,
                    RGB(0x30, 0xe0, 0xc0), RGB(0x0a, 0x84, 0xff), 255);
        }
    }

  ops_draw(hwnd, &g_o);

  /* Memory: a meter */

  card(12, y + 174, cw - 24, 74, "MEMORY");
  pct = g_desk.memtotal ? g_desk.memused * 100 / g_desk.memtotal : 0;
  snprintf(buf, sizeof(buf), "%u KB of %u KB", g_desk.memused,
           g_desk.memtotal);
  ops_text(&g_o, &g_desk_font_ui, 26, y + 198, buf, C_TEXT, 255);
  ops_rrect(&g_o, 26, y + 222, cw - 52, 12, LUT_R6, 6, RGB(0x44, 0x44, 0x4a),
            255);
  ops_rrect(&g_o, 26, y + 222, 12 + (cw - 64) * pct / 100, 12, LUT_R6, 6,
            C_ACCENT, 255);

  /* System */

  card(12, y + 260, cw - 24, 56, "SYSTEM");
  snprintf(buf, sizeof(buf), "Up %lu min  " DESK_CH_MIDDOT
           "  %d windows", (unsigned long)(desk_ms() / 60000), win_count());
  ops_text(&g_o, &g_desk_font_ui, 26, y + 284, buf, C_TEXT, 255);
  ops_draw(hwnd, &g_o);
}

static struct desk_win_s *monitor_open(void)
{
  struct desk_win_s *w = desk_win_open(APP_MONITOR, "Monitor", 440, 330);

  if (w != NULL)
    {
      monitor_draw(w);
    }

  return w;
}

/****************************************************************************
 * Private Functions: Settings
 ****************************************************************************/

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

static void settings_click(struct desk_win_s *w, int x, int y)
{
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

/****************************************************************************
 * Private Functions: About
 ****************************************************************************/

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

const struct desk_app_s g_apps[APP_COUNT] =
{
  { "Applications", icon_apps,     NULL,          NULL,          NULL,
    NULL,           NULL,          false },
  { "Terminal",     icon_terminal, term_open,     NULL,          NULL,
    NULL,           term_close,    false },
  { "Monitor",      icon_monitor,  monitor_open,  monitor_draw,  monitor_draw,
    NULL,           NULL,          true },
  { "Settings",     icon_settings, settings_open, settings_draw, NULL,
    settings_click, NULL,          true },
  { "About",        icon_about,    about_open,    about_draw,    NULL,
    NULL,           NULL,          true },
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
