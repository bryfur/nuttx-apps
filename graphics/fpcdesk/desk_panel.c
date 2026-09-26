/****************************************************************************
 * apps/graphics/fpcdesk/desk_panel.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's own windows:
 *
 *   - the wallpaper: a window in the desktop layer whose pixels are drawn
 *     once, by the GPU (a gradient and large soft glows);
 *   - the top bar: a layer window over a dark translucent backdrop (the
 *     compositor blends it over whatever is below, windows included):
 *     "Activities", the active application, the clock, the load;
 *   - the dock: a layer window with a light translucent backdrop, round
 *     corners and a shadow: the applications, a dot under the running
 *     ones, the one under the pointer lifted with its name above it;
 *   - the launcher: a layer window over the whole screen, fading in: the
 *     time, and the applications as large tiles.
 *
 * Layer windows have no pixels: they are display lists, redrawn by
 * replacing the list, which costs only the operations on the link.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nuttx/nx/nxbe.h>

#include "desk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LAUNCHER_STEPS  6
#define TILE            96
#define TILE_ICON       64
#define TILE_GAP        28
#define ACT_X           10
#define ACT_W           84

/****************************************************************************
 * Private Data
 ****************************************************************************/

extern const struct nx_callback_s g_desk_partcb;

static struct desk_ops_s g_ops;
static int g_launcher_alpha;       /* 0..255 while fading in */
static int g_launcher_hover = -1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static NXWINDOW panel_open(uint8_t part, uint8_t flags, int x, int y,
                           int w, int h)
{
  struct nxgl_point_s pos;
  struct nxgl_size_s size;
  NXWINDOW hwnd;

  hwnd = nx_openwindow(g_desk.hnx, flags | NXBE_WINDOW_HIDDEN,
                       &g_desk_partcb, (void *)(uintptr_t)part);
  if (hwnd == NULL)
    {
      return NULL;
    }

  size.w = w;
  size.h = h;
  pos.x  = x;
  pos.y  = y;
  nx_setsize(hwnd, &size);
  nx_setposition(hwnd, &pos);
  return hwnd;
}

static void panel_effects(NXWINDOW hwnd, uint8_t layer, uint8_t flags,
                          uint8_t backdrop, uint16_t tint, uint8_t shadow)
{
  struct nx_effects_s fx;

  memset(&fx, 0, sizeof(fx));
  fx.flags    = flags;
  fx.layer    = layer;
  fx.opacity  = 255;
  fx.backdrop = backdrop;
  fx.tint     = tint;
  fx.shadow   = shadow;
  fx.shadowdy = 6;
  nx_seteffects(hwnd, &fx);
}

/* The apps in the dock: the launcher first, then the others */

static int dock_count(void)
{
  return APP_COUNT;
}

static int dock_icon_x(int i)
{
  return DOCK_PAD + i * (DOCK_ICON + DOCK_PAD);
}

/****************************************************************************
 * Public Functions: the wallpaper
 ****************************************************************************/

struct wallpaper_s
{
  uint16_t top;
  uint16_t bottom;
  struct
  {
    int16_t x;
    int16_t y;
    uint8_t r;
    uint8_t shift;
    uint16_t c;
    uint8_t alpha;
  } glow[5];
};

static const struct wallpaper_s g_walls[3] =
{
  /* Aurora */

  {
    RGB(0x0c, 0x16, 0x38), RGB(0x2c, 0x10, 0x48),
    {
      { 150, 640, 2, 3, RGB(0xd8, 0x2c, 0x8c), 150 },
      { 860, 170, 2, 3, RGB(0x10, 0xb0, 0xe0), 140 },
      { 610, 560, 1, 3, RGB(0x70, 0x40, 0xf8), 130 },
      { 300, 130, 4, 2, RGB(0x30, 0x70, 0xf0), 120 },
      { 980, 700, 2, 2, RGB(0xff, 0x80, 0x40), 90 },
    }
  },

  /* Sunset */

  {
    RGB(0x18, 0x0c, 0x30), RGB(0x60, 0x18, 0x30),
    {
      { 512, 800, 6, 3, RGB(0xff, 0x70, 0x30), 190 },
      { 180, 520, 2, 3, RGB(0xf0, 0x30, 0x60), 130 },
      { 860, 480, 2, 3, RGB(0xc0, 0x30, 0xa0), 120 },
      { 700, 120, 4, 2, RGB(0x50, 0x30, 0xa0), 110 },
      { 120, 100, 2, 2, RGB(0x20, 0x40, 0x90), 90 },
    }
  },

  /* Graphite */

  {
    RGB(0x24, 0x26, 0x2c), RGB(0x0e, 0x0f, 0x12),
    {
      { 820, 120, 2, 3, RGB(0x50, 0x60, 0x80), 90 },
      { 160, 620, 2, 3, RGB(0x40, 0x48, 0x58), 80 },
      { 520, 420, 2, 3, RGB(0x30, 0x34, 0x40), 60 },
      { 0, 0, 0, 0, 0, 0 },
      { 0, 0, 0, 0, 0, 0 },
    }
  },
};

void desk_wallpaper_draw(void)
{
  const struct wallpaper_s *w = &g_walls[g_desk.wallstyle % 3];
  int i;

  ops_reset(&g_ops);
  ops_vgrad(&g_ops, 0, 0, DESK_W, DESK_H, w->top, w->bottom, 255);
  for (i = 0; i < 5; i++)
    {
      if (w->glow[i].alpha > 0)
        {
          /* Placed for 1024 x 768, wherever that is on this display */

          ops_glow(&g_ops, w->glow[i].x * DESK_W / 1024,
                   w->glow[i].y * DESK_H / 768, w->glow[i].r,
                   w->glow[i].shift, w->glow[i].c, w->glow[i].alpha);
        }
    }

  ops_draw(g_desk.wallpaper, &g_ops);
}

/* Small previews of the wallpapers (Settings) */

void desk_wallpaper_preview(struct desk_ops_s *o, int style, int x, int y,
                            int w, int h)
{
  const struct wallpaper_s *wp = &g_walls[style % 3];
  int i;

  ops_vgrad(o, x, y, w, h, wp->top, wp->bottom, 255);
  for (i = 0; i < 5; i++)
    {
      if (wp->glow[i].alpha > 0)
        {
          int gx = x + wp->glow[i].x * w / 1024;
          int gy = y + wp->glow[i].y * h / 768;
          int shift = wp->glow[i].shift > 3 ? wp->glow[i].shift - 3 : 0;
          int reach = (NX_BLEND_LUT_N - 1) << shift;
          struct nx_gpuop_s *op = ops_add(o);

          /* The glow clipped to the preview */

          op->op     = NX_GPUOP_BLEND;
          op->x      = gx - reach < x ? x : gx - reach;
          op->y      = gy - reach < y ? y : gy - reach;
          op->w      = (gx + reach > x + w - 1 ? x + w - 1 : gx + reach) -
                       op->x + 1;
          op->h      = (gy + reach > y + h - 1 ? y + h - 1 : gy + reach) -
                       op->y + 1;
          op->colour = wp->glow[i].c;
          op->alpha  = wp->glow[i].alpha;
          op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_LUT |
                       NX_BLEND_DITHER;
          op->slot   = LUT_GLOW;
          op->shift  = shift;
          op->p[0]   = gx;
          op->p[1]   = gy;
          op->p[2]   = gx;
          op->p[3]   = gy;
        }
    }
}

/****************************************************************************
 * Public Functions: the top bar
 ****************************************************************************/

void desk_topbar_draw(void)
{
  char buf[48];
  time_t now = time(NULL);
  struct tm tm;
  const struct desk_font_s *f = &g_desk_font_uib;
  int ty;
  int x;

  if (g_desk.topbar == NULL)
    {
      return;
    }

  localtime_r(&now, &tm);
  ty = (TOPBAR_H - text_height(f)) / 2;

  ops_reset(&g_ops);

  /* Activities, a pill when the pointer is on it */

  if (g_desk.topbarhover || g_desk.launcher != NULL)
    {
      ops_rrect(&g_ops, ACT_X - 6, 4, ACT_W, TOPBAR_H - 8, LUT_R6, 6,
                C_WHITE, g_desk.launcher != NULL ? 70 : 45);
    }

  x = ACT_X + ops_text(&g_ops, f, ACT_X, ty, "Activities", C_WHITE, 255);

  /* The active application */

  if (g_desk.active != NULL)
    {
      ops_text(&g_ops, &g_desk_font_ui, x + 24, ty,
               g_apps[g_desk.active->app]->name, C_WHITE, 200);
    }

  /* The clock */

  strftime(buf, sizeof(buf), "%a %b %e  %H:%M", &tm);
  ops_text_centre(&g_ops, f, DESK_W / 2, ty, buf, C_WHITE, 255);

  /* The load, right-aligned: a small meter and a figure */

  snprintf(buf, sizeof(buf), "CPU %d%%", g_desk.cpuload);
  x = DESK_W - 14 - text_width(&g_desk_font_ui, buf);
  ops_text(&g_ops, &g_desk_font_ui, x, ty, buf, C_WHITE, 220);
  ops_rrect(&g_ops, x - 50, 11, 40, 6, LUT_R6, 3, C_WHITE, 50);
  ops_rrect(&g_ops, x - 50, 11,
            6 + 34 * (g_desk.cpuload > 100 ? 100 : g_desk.cpuload) / 100,
            6, LUT_R6, 3, g_desk.cpuload > 80 ? C_CLOSE : C_MAXIMIZE, 255);

  if (g_desk.memtotal > 0)
    {
      snprintf(buf, sizeof(buf), "%u.%u MB", g_desk.memused / 1024,
               (g_desk.memused % 1024) * 10 / 1024);
      x -= 70 + text_width(&g_desk_font_ui, buf);
      ops_text(&g_ops, &g_desk_font_ui, x, ty, buf, C_WHITE, 170);
    }

  ops_setlist(g_desk.topbar, &g_ops);
}

void desk_topbar_mouse(int x, int y, uint8_t buttons)
{
  bool over = x >= ACT_X - 6 && x < ACT_X - 6 + ACT_W && y >= 0 &&
              y < TOPBAR_H;
  static uint8_t last;

  if (over != g_desk.topbarhover)
    {
      g_desk.topbarhover = over;
      desk_topbar_draw();
    }

  if (over && (buttons & NX_MOUSE_LEFTBUTTON) &&
      !(last & NX_MOUSE_LEFTBUTTON))
    {
      desk_launcher_toggle();
    }

  last = buttons;
}

/****************************************************************************
 * Public Functions: the dock
 ****************************************************************************/

static void tip_show(int app)
{
  const char *name;
  struct nxgl_point_s pos;
  struct nxgl_size_s size;
  int w;

  if (app < 0)
    {
      nx_setvisibility(g_desk.tip, true);
      return;
    }

  name   = g_apps[app]->name;
  w      = text_width(&g_desk_font_ui, name) + 20;
  size.w = w;
  size.h = 24;
  pos.x  = g_desk.dockx + dock_icon_x(app) + DOCK_ICON / 2 - w / 2;
  pos.y  = DESK_H - DOCK_MARGIN - DOCK_H - 30;

  ops_reset(&g_ops);
  ops_rrect(&g_ops, 0, 0, w, 24, LUT_R6, 6, RGB(0x20, 0x20, 0x24), 225);
  ops_text_centre(&g_ops, &g_desk_font_ui, w / 2,
                  (24 - text_height(&g_desk_font_ui)) / 2, name,
                  C_WHITE, 255);

  nx_setsize(g_desk.tip, &size);
  nx_setposition(g_desk.tip, &pos);
  ops_setlist(g_desk.tip, &g_ops);
  nx_setvisibility(g_desk.tip, false);
}

void desk_dock_draw(void)
{
  struct desk_win_s *w;
  int i;

  if (g_desk.dock == NULL)
    {
      return;
    }

  ops_reset(&g_ops);

  /* A hairline of light along the top of the glass */

  ops_blend(&g_ops, 12, 0, g_desk.dockw - 24, 1, C_WHITE, 70);

  for (i = 0; i < dock_count(); i++)
    {
      int x = dock_icon_x(i);
      int y = DOCK_PAD - (g_desk.dockhover == i ? 4 : 0);
      bool running = false;

      g_apps[i]->icon(&g_ops, x, y, DOCK_ICON, 255);

      for (w = g_desk.wins; w != NULL; w = w->next)
        {
          running |= w->app == i && !w->closing;
        }

      if (running)
        {
          ops_text_centre(&g_ops, &g_desk_font_title, x + DOCK_ICON / 2,
                          DOCK_H - 18, DESK_CH_BULLET, C_WHITE, 220);
        }
    }

  ops_setlist(g_desk.dock, &g_ops);
}

void desk_dock_mouse(int x, int y, uint8_t buttons)
{
  static uint8_t last;
  int hover = -1;
  int i;

  if (y >= 0 && y < DOCK_H)
    {
      for (i = 0; i < dock_count(); i++)
        {
          if (x >= dock_icon_x(i) - DOCK_PAD / 2 &&
              x < dock_icon_x(i) + DOCK_ICON + DOCK_PAD / 2)
            {
              hover = i;
            }
        }
    }

  if (hover != g_desk.dockhover)
    {
      g_desk.dockhover = hover;
      desk_dock_draw();
      tip_show(hover);
    }

  if (hover >= 0 && (buttons & NX_MOUSE_LEFTBUTTON) &&
      !(last & NX_MOUSE_LEFTBUTTON))
    {
      struct desk_win_s *w = desk_app_window(hover);

      /* A running app comes forward (or back from the dock); else starts */

      if (w != NULL && hover != APP_LAUNCHER &&
          (w->minimized || w != g_desk.active || g_apps[hover]->single))
        {
          if (g_desk.launcher != NULL)
            {
              desk_launcher_toggle();
            }

          desk_win_restore(w);
        }
      else
        {
          desk_launch(hover);
        }
    }

  last = buttons;
}

/****************************************************************************
 * Public Functions: the launcher
 ****************************************************************************/

static void tile_pos(int i, int *x, int *y)
{
  int n = APP_COUNT - 1;
  int w = n * TILE + (n - 1) * TILE_GAP;

  *x = (DESK_W - w) / 2 + (i - 1) * (TILE + TILE_GAP);
  *y = DESK_H * 43 / 100;
}

void desk_launcher_draw(void)
{
  char buf[48];
  time_t now = time(NULL);
  struct tm tm;
  uint8_t a = (uint8_t)g_launcher_alpha;
  int i;

  if (g_desk.launcher == NULL)
    {
      return;
    }

  localtime_r(&now, &tm);
  ops_reset(&g_ops);
  ops_blend(&g_ops, 0, 0, DESK_W, DESK_H, RGB(0x06, 0x06, 0x10),
            (uint8_t)(a * 150 / 255));

  strftime(buf, sizeof(buf), "%H:%M", &tm);
  ops_text_centre(&g_ops, &g_desk_font_big, DESK_W / 2, DESK_H * 18 / 100,
                  buf, C_WHITE,
                  a);
  strftime(buf, sizeof(buf), "%A, %B %e", &tm);
  ops_text_centre(&g_ops, &g_desk_font_title, DESK_W / 2, DESK_H * 26 / 100,
                  buf,
                  C_WHITE, (uint8_t)(a * 200 / 255));

  for (i = 1; i < APP_COUNT; i++)
    {
      int x;
      int y;

      tile_pos(i, &x, &y);
      if (g_launcher_hover == i)
        {
          ops_rrect(&g_ops, x - 6, y - 8, TILE + 12, TILE + 30, LUT_R12, 12,
                    C_WHITE, (uint8_t)(a * 40 / 255));
        }

      g_apps[i]->icon(&g_ops, x + (TILE - TILE_ICON) / 2, y + 4, TILE_ICON,
                     a);
      ops_text_centre(&g_ops, &g_desk_font_ui, x + TILE / 2,
                      y + TILE_ICON + 12, g_apps[i]->name, C_WHITE, a);
    }

  ops_setlist(g_desk.launcher, &g_ops);
}

void desk_launcher_toggle(void)
{
  if (g_desk.launcher != NULL)
    {
      nx_closewindow(g_desk.launcher);
      g_desk.launcher = NULL;
      g_desk.launcherfade = 0;
    }
  else
    {
      g_desk.launcher = panel_open(PART_LAUNCHER, NXBE_WINDOW_LAYER, 0, 0,
                                   DESK_W, DESK_H);
      if (g_desk.launcher != NULL)
        {
          /* Over the windows, under the top bar and the dock */

          panel_effects(g_desk.launcher, NX_LAYER_PANEL, 0, 0, 0, 0);
          nx_raise(g_desk.topbar);
          nx_raise(g_desk.dock);
          g_launcher_alpha    = 0;
          g_launcher_hover    = -1;
          g_desk.launcherfade = LAUNCHER_STEPS;
          desk_launcher_draw();
          nx_setvisibility(g_desk.launcher, false);
        }
    }

  desk_topbar_draw();
}

void desk_launcher_mouse(int x, int y, uint8_t buttons)
{
  static uint8_t last;
  int hover = -1;
  int i;

  for (i = 1; i < APP_COUNT; i++)
    {
      int tx;
      int ty;

      tile_pos(i, &tx, &ty);
      if (x >= tx && x < tx + TILE && y >= ty && y < ty + TILE + 20)
        {
          hover = i;
        }
    }

  if (hover != g_launcher_hover)
    {
      g_launcher_hover = hover;
      desk_launcher_draw();
    }

  if ((buttons & NX_MOUSE_LEFTBUTTON) == 0 && (last & NX_MOUSE_LEFTBUTTON))
    {
      if (hover >= 0)
        {
          desk_launch(hover);
        }
      else
        {
          desk_launcher_toggle();
        }
    }

  last = buttons;
}

/****************************************************************************
 * Public Functions: all of them
 ****************************************************************************/

int desk_panels_init(void)
{
  int n = dock_count();

  /* The wallpaper, at the bottom */

  g_desk.wallpaper = panel_open(PART_WALLPAPER, NXBE_WINDOW_RAMBACKED, 0, 0,
                                DESK_W, DESK_H);
  if (g_desk.wallpaper == NULL)
    {
      return -1;
    }

  panel_effects(g_desk.wallpaper, NX_LAYER_DESKTOP, 0, 0, 0, 0);
  desk_wallpaper_draw();
  nx_setvisibility(g_desk.wallpaper, false);

  /* The top bar */

  g_desk.topbar = panel_open(PART_TOPBAR, NXBE_WINDOW_LAYER, 0, 0, DESK_W,
                             TOPBAR_H);
  panel_effects(g_desk.topbar, NX_LAYER_PANEL, 0, 150, C_BLACK, 0);
  desk_topbar_draw();
  nx_setvisibility(g_desk.topbar, false);

  /* The dock, centred at the bottom */

  g_desk.dockw = n * DOCK_ICON + (n + 1) * DOCK_PAD;
  g_desk.dockx = (DESK_W - g_desk.dockw) / 2;
  g_desk.dock  = panel_open(PART_DOCK, NXBE_WINDOW_LAYER, g_desk.dockx,
                            DESK_H - DOCK_H - DOCK_MARGIN, g_desk.dockw,
                            DOCK_H);
  panel_effects(g_desk.dock, NX_LAYER_PANEL, NX_FX_ROUND, 60,
                RGB(0xf0, 0xf0, 0xff), 110);
  desk_dock_draw();
  nx_setvisibility(g_desk.dock, false);

  /* The dock's tooltip, shown on hover */

  g_desk.tip = panel_open(PART_TIP, NXBE_WINDOW_LAYER, 0, 0, 10, 10);
  panel_effects(g_desk.tip, NX_LAYER_OVERLAY, 0, 0, 0, 0);
  return 0;
}

/* The display has a new size (a display mode): the wallpaper first (its
 * surface is the largest), then the top bar, the dock and the launcher
 */

void desk_panels_relayout(void)
{
  struct nxgl_point_s pos;
  struct nxgl_size_s size;

  size.w = DESK_W;
  size.h = DESK_H;
  nx_setsize(g_desk.wallpaper, &size);
  desk_wallpaper_draw();

  size.h = TOPBAR_H;
  nx_setsize(g_desk.topbar, &size);
  desk_topbar_draw();

  g_desk.dockx = (DESK_W - g_desk.dockw) / 2;
  pos.x = g_desk.dockx;
  pos.y = DESK_H - DOCK_H - DOCK_MARGIN;
  nx_setposition(g_desk.dock, &pos);
  g_desk.dockhover = -1;
  desk_dock_draw();
  nx_setvisibility(g_desk.tip, true);

  if (g_desk.launcher != NULL)
    {
      size.h = DESK_H;
      nx_setsize(g_desk.launcher, &size);
      desk_launcher_draw();
    }
}

void desk_panels_tick(void)
{
  desk_topbar_draw();
  if (g_desk.launcher != NULL)
    {
      desk_launcher_draw();
    }
}

bool desk_panels_animate(void)
{
  if (g_desk.launcher != NULL && g_desk.launcherfade > 0)
    {
      g_desk.launcherfade--;
      g_launcher_alpha = 255 - g_desk.launcherfade * 255 / LAUNCHER_STEPS;
      desk_launcher_draw();
      return g_desk.launcherfade > 0;
    }

  return false;
}
