/****************************************************************************
 * apps/graphics/fpcdesk/desk_monitor.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Monitor: CPU load (with its last minute), memory, uptime.
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

static void monitor_icon(struct desk_ops_s *o, int x, int y, int s,
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


const struct desk_app_s g_app_monitor =
{
  .name   = "Monitor",
  .icon   = monitor_icon,
  .open   = monitor_open,
  .draw   = monitor_draw,
  .tick   = monitor_draw,
  .single = true,
};
