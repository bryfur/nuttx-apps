/****************************************************************************
 * apps/graphics/fpcdesk/desk_panel.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's own windows:
 *
 *   - the wallpaper: a window in the desktop layer whose pixels are drawn
 *     once, by the GPU (a gradient and large soft glows);
 *   - the top bar: a layer window over a translucent backdrop (the
 *     compositor blends it over whatever is below, windows included):
 *     "Activities", the active application, the clock, the load;
 *   - the dock: a layer window with a translucent backdrop, round corners
 *     and a shadow, at the bottom, left or right: the launcher, the
 *     favourites, a separator and the other running applications, a dot
 *     under the running ones, the one under the pointer lifted with its
 *     name beside it.  It may hide itself, and come out when the pointer
 *     touches its edge of the display (a thin layer window there);
 *   - the launcher: a layer window over the whole screen, fading in: a
 *     search field and the applications as large tiles, in rows and pages;
 *   - pop-up menus (the dock's and the launcher's, on the right button).
 *
 * Layer windows have no pixels: they are display lists, redrawn by
 * replacing the list, which costs only the operations on the link.  The
 * icons of the dock and the launcher are drawn once into a hidden window,
 * the icon atlas, so that each is one BLEND in a list (with the round
 * tile's shape).
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nuttx/nx/nxbe.h>

#include "desk.h"
#include "desk_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LAUNCHER_STEPS  6
#define DOCK_STEPS      6
#define TILE            96             /* A launcher tile, its icon */
#define TILE_ICON       64
#define TILE_GAP        28
#define TILE_H          (TILE_ICON + 44)
#define SEARCH_W        320
#define SEARCH_H        34
#define ACT_X           10
#define ACT_W           84
#define TIP_H           24
#define MENU_ITEMS      12
#define MENU_ITEM_H     26
#define MENU_SEP_H      9
#define MENU_PAD        6
#define SEP             (-1)           /* The dock's separator */
#define EDGE            2              /* The hidden dock's edge */

/****************************************************************************
 * Private Data
 ****************************************************************************/

extern const struct nx_callback_s g_desk_partcb;

static struct desk_ops_s g_ops;
static struct desk_list_s g_list;      /* The launcher's, longer */

/* The icon atlas: row 0 the launcher's size, row 1 the dock's */

static NXWINDOW g_icons;
static int g_iconrow1;                 /* The dock's icon size drawn there */

/* The dock: its items (apps, SEP) and where each is along it */

static struct
{
  int app;
  int off;
} g_dock[2 * APP_COUNT + 1];

static int g_ndock;
static int g_dicon;                    /* Its icons' size: the setting, or
                                        * less if they do not fit */
static int g_dpad;
static int g_dlen;                     /* Its length */

/* The launcher */

static struct
{
  int alpha;                           /* 0..255 while fading in */
  int hover;                           /* Index in apps[], -1 */
  int sel;                             /* Selected (keys), -1 */
  int page;
  int npages;
  int cols;
  int rows;
  int x0;                              /* The grid's top-left */
  int y0;
  int n;                               /* The apps that match */
  int apps[APP_COUNT];
  uint8_t last;                        /* Buttons */
  struct ui_field_s search;
  struct ui_keys_s keys;
} g_l;

/* A pop-up menu */

static struct
{
  struct desk_menu_item_s item[MENU_ITEMS];
  int n;
  int hover;
  int w;
  int h;
  bool pressed;                        /* A press began in it */
  void (*pick)(int id, void *arg);
  void *arg;
} g_menu;

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
  if ((flags & NX_FX_ROUND) != 0 && g_theme.darkedge)
    {
      fx.flags |= NX_FX_DARKEDGE;         /* Light glass: a dark edge */
      fx.border = 40;
    }

  nx_seteffects(hwnd, &fx);
}

/* To (x, y), then w x h: NX clips a window's size to the display, so
 * where it is must be on it first
 */

static void panel_move(NXWINDOW hwnd, int x, int y, int w, int h)
{
  struct nxgl_point_s pos;
  struct nxgl_size_s size;

  size.w = w;
  size.h = h;
  pos.x  = x;
  pos.y  = y;
  nx_setposition(hwnd, &pos);
  nx_setsize(hwnd, &size);
}

static bool app_running(int app)
{
  struct desk_win_s *w;

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      if (w->app == app && !w->closing)
        {
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Private Functions: the icon atlas
 ****************************************************************************/

static void icons_redraw(NXWINDOW hwnd, const struct nxgl_rect_s *rect,
                         bool more, void *arg)
{
}

static const struct nx_callback_s g_iconscb =
{
  icons_redraw, NULL, NULL, NULL, NULL
};

/* Draw row (0: the launcher's, 1: the dock's) of the atlas at size s */

static void icons_draw(int row, int s)
{
  int y = row == 0 ? 0 : TILE_ICON + 2;
  int i;

  ops_reset(&g_ops);
  ops_fill(&g_ops, 0, y, APP_COUNT * (TILE_ICON + 2), s, C_BLACK);
  ops_draw(g_icons, &g_ops);
  for (i = 0; i < APP_COUNT; i++)
    {
      if (g_apps[i]->icon != NULL)
        {
          g_apps[i]->icon(&g_ops, i * (TILE_ICON + 2), y, s, 255);
          ops_draw(g_icons, &g_ops);
        }
    }

  if (row == 1)
    {
      g_iconrow1 = s;
    }
}

static void icons_init(void)
{
  struct nxgl_point_s pos;
  struct nxgl_size_s size;

  g_icons = nx_openwindow(g_desk.hnx,
                          NXBE_WINDOW_RAMBACKED | NXBE_WINDOW_HIDDEN,
                          &g_iconscb, NULL);
  if (g_icons == NULL)
    {
      return;
    }

  pos.x  = 0;
  pos.y  = 0;
  size.w = APP_COUNT * (TILE_ICON + 2);
  size.h = 2 * TILE_ICON + 2;
  nx_setsize(g_icons, &size);
  nx_setposition(g_icons, &pos);
  icons_draw(0, TILE_ICON);
}

/* An application's icon: from the atlas if it has that size (one BLEND,
 * shaped like the tile), else drawn
 */

static void icon_put(struct desk_ops_s *o, int app, int x, int y, int s,
                     uint8_t a)
{
  struct nx_gpuop_s *op;
  int row = s == TILE_ICON ? 0 : s == g_iconrow1 ? 1 : -1;

  if (g_icons == NULL || row < 0 || g_apps[app]->icon == NULL)
    {
      if (g_apps[app]->icon != NULL)
        {
          g_apps[app]->icon(o, x, y, s, a);
        }

      return;
    }

  op         = ops_add(o);
  op->op     = NX_GPUOP_BLEND;
  op->x      = x;
  op->y      = y;
  op->w      = s;
  op->h      = s;
  op->src    = g_icons;
  op->sx     = app * (TILE_ICON + 2);
  op->sy     = row == 0 ? 0 : TILE_ICON + 2;
  op->alpha  = a;
  op->mode   = NX_BLEND_SRC_RGB | NX_BLEND_MOD_LUT;
  op->slot   = LUT_R12;
  op->p[0]   = x + 12;
  op->p[1]   = y + 12;
  op->p[2]   = x + s - 1 - 12;
  op->p[3]   = y + s - 1 - 12;
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

  if (g_desk.wallpaper == NULL)
    {
      return;
    }

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

/* The clock as Settings have it: 24 or 12-hour, perhaps with seconds and
 * the date
 */

static void clock_text(char *buf, size_t len, const struct tm *tm)
{
  char t[16];
  const char *fmt = g_prefs.clock24 ?
                    (g_prefs.clocksecs ? "%H:%M:%S" : "%H:%M") :
                    (g_prefs.clocksecs ? "%l:%M:%S %p" : "%l:%M %p");

  strftime(t, sizeof(t), fmt, tm);
  if (g_prefs.clockdate)
    {
      char d[16];

      strftime(d, sizeof(d), "%a %b %e", tm);
      snprintf(buf, len, "%s  %s", d, t[0] == ' ' ? t + 1 : t);
    }
  else
    {
      snprintf(buf, len, "%s", t[0] == ' ' ? t + 1 : t);
    }
}

void desk_topbar_draw(void)
{
  char buf[48];
  struct tm tm;
  const struct desk_font_s *f = &g_desk_font_uib;
  uint16_t fg = g_theme.panel_text;
  int ty;
  int x;

  if (g_desk.topbar == NULL)
    {
      return;
    }

  desk_localtime(&tm);
  ty = (TOPBAR_H - text_height(f)) / 2;

  ops_reset(&g_ops);

  /* Activities, a pill when the pointer is on it */

  if (g_desk.topbarhover || g_desk.launcher != NULL)
    {
      ops_rrect(&g_ops, ACT_X - 6, 4, ACT_W, TOPBAR_H - 8, LUT_R6, 6,
                fg, THEMED(g_desk.launcher != NULL ? 70 : 45,
                           g_desk.launcher != NULL ? 40 : 24));
    }

  x = ACT_X + ops_text(&g_ops, f, ACT_X, ty, "Activities", fg, 255);

  /* The active application */

  if (g_desk.active != NULL)
    {
      ops_text(&g_ops, &g_desk_font_ui, x + 24, ty,
               g_apps[g_desk.active->app]->name, fg, 200);
    }

  /* The clock */

  clock_text(buf, sizeof(buf), &tm);
  ops_text_centre(&g_ops, f, DESK_W / 2, ty, buf, fg, 255);

  /* The load, right-aligned: a small meter and a figure */

  snprintf(buf, sizeof(buf), "CPU %d%%", g_desk.cpuload);
  x = DESK_W - 14 - text_width(&g_desk_font_ui, buf);
  ops_text(&g_ops, &g_desk_font_ui, x, ty, buf, fg, 220);
  ops_rrect(&g_ops, x - 50, 11, 40, 6, LUT_R6, 3, fg, THEMED(50, 40));
  ops_rrect(&g_ops, x - 50, 11,
            6 + 34 * (g_desk.cpuload > 100 ? 100 : g_desk.cpuload) / 100,
            6, LUT_R6, 3, g_desk.cpuload > 80 ? C_CLOSE : C_MAXIMIZE, 255);

  if (g_desk.memtotal > 0)
    {
      snprintf(buf, sizeof(buf), "%u.%u MB", g_desk.memused / 1024,
               (g_desk.memused % 1024) * 10 / 1024);
      x -= 70 + text_width(&g_desk_font_ui, buf);
      ops_text(&g_ops, &g_desk_font_ui, x, ty, buf, fg, 170);
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

static bool dock_vertical(void)
{
  return g_prefs.dockpos == DOCK_LEFT || g_prefs.dockpos == DOCK_RIGHT;
}

/* Its length for icons of size s */

static int dock_length(int s)
{
  int pad = s / 5;
  int len = pad;
  int i;

  for (i = 0; i < g_ndock; i++)
    {
      len += (g_dock[i].app == SEP ? 1 : s) + pad;
    }

  return len;
}

/* The items: the launcher, the favourites, then the other running
 * applications after a separator; the size of the icons (smaller if they
 * would not fit), and where the dock is when it is out
 */

static void dock_build(void)
{
  bool listed[APP_COUNT];
  int room;
  int off;
  int i;

  memset(listed, 0, sizeof(listed));
  g_ndock = 0;
  g_dock[g_ndock++].app = APP_LAUNCHER;
  listed[APP_LAUNCHER] = true;

  for (i = 0; i < g_prefs.npinned; i++)
    {
      int app = g_prefs.pinned[i];

      if (app > 0 && app < APP_COUNT && !listed[app])
        {
          g_dock[g_ndock++].app = app;
          listed[app] = true;
        }
    }

  for (i = 1; i < APP_COUNT; i++)
    {
      if (!listed[i] && app_running(i))
        {
          if (g_dock[g_ndock - 1].app != SEP && g_ndock > 1)
            {
              g_dock[g_ndock++].app = SEP;
            }

          g_dock[g_ndock++].app = i;
          listed[i] = true;
        }
    }

  /* The icons' size: the setting, unless the dock would not fit */

  room    = dock_vertical() ? DESK_H - TOPBAR_H - 2 * DOCK_MARGIN :
                              DESK_W - 2 * DOCK_MARGIN;
  g_dicon = DOCK_ICON;
  while (g_dicon > 24 && dock_length(g_dicon) > room)
    {
      g_dicon -= 4;
    }

  g_dpad = g_dicon / 5;
  g_dlen = dock_length(g_dicon);
  off    = g_dpad;
  for (i = 0; i < g_ndock; i++)
    {
      g_dock[i].off = off;
      off += (g_dock[i].app == SEP ? 1 : g_dicon) + g_dpad;
    }

  /* Where it is when out */

  switch (g_prefs.dockpos)
    {
      case DOCK_LEFT:
      case DOCK_RIGHT:
        g_desk.dockw = g_dicon + 2 * g_dpad;
        g_desk.dockh = g_dlen;
        g_desk.dockx = g_prefs.dockpos == DOCK_LEFT ? DOCK_MARGIN :
                       DESK_W - DOCK_MARGIN - g_desk.dockw;
        g_desk.docky = TOPBAR_H + (DESK_H - TOPBAR_H - g_dlen) / 2;
        break;

      default:
        g_desk.dockw = g_dlen;
        g_desk.dockh = g_dicon + 2 * g_dpad;
        g_desk.dockx = (DESK_W - g_dlen) / 2;
        g_desk.docky = DESK_H - DOCK_MARGIN - g_desk.dockh;
        break;
    }

  /* The atlas has the dock's size too */

  if (g_icons != NULL && g_dicon != TILE_ICON && g_dicon != g_iconrow1)
    {
      icons_draw(1, g_dicon);
    }
}

/* Where an item is in the dock (its icon's top-left) */

static void dock_item_xy(int i, int *x, int *y)
{
  if (dock_vertical())
    {
      *x = g_dpad;
      *y = g_dock[i].off;
    }
  else
    {
      *x = g_dock[i].off;
      *y = g_dpad;
    }
}

/* The dock's window where it is now: out, hidden, or on its way */

static void dock_place(void)
{
  int out = g_prefs.dockhide ? g_desk.dockout : DOCK_STEPS;
  int dist;
  int x = g_desk.dockx;
  int y = g_desk.docky;

  out  = out < 0 ? 0 : out;
  dist = (dock_vertical() ? g_desk.dockw : g_desk.dockh) + DOCK_MARGIN +
         20;
  dist = dist * (DOCK_STEPS - out) / DOCK_STEPS;

  switch (g_prefs.dockpos)
    {
      case DOCK_LEFT:
        x -= dist;
        break;

      case DOCK_RIGHT:
        x += dist;
        break;

      default:
        y += dist;
        break;
    }

  /* Its size where it is all on the display (not while it is hidden:
   * when it comes out), then where it goes
   */

  if (out > 0)
    {
      panel_move(g_desk.dock, g_desk.dockx, g_desk.docky, g_desk.dockw,
                 g_desk.dockh);
    }

  if (out == 0 || x != g_desk.dockx || y != g_desk.docky)
    {
      struct nxgl_point_s pos;

      pos.x = x;
      pos.y = y;
      nx_setposition(g_desk.dock, &pos);
    }
}

/* The thin window along the display's edge that brings a hidden dock
 * out
 */

static void edge_place(void)
{
  if (!g_prefs.dockhide)
    {
      if (g_desk.edge != NULL)
        {
          nx_closewindow(g_desk.edge);
          g_desk.edge = NULL;
        }

      return;
    }

  if (g_desk.edge == NULL)
    {
      g_desk.edge = panel_open(PART_EDGE, NXBE_WINDOW_LAYER, 0, 0, 1, 1);
      if (g_desk.edge == NULL)
        {
          return;
        }

      panel_effects(g_desk.edge, NX_LAYER_PANEL, 0, 0, 0, 0);
      ops_reset(&g_ops);
      ops_setlist(g_desk.edge, &g_ops);
      nx_setvisibility(g_desk.edge, false);
    }

  switch (g_prefs.dockpos)
    {
      case DOCK_LEFT:
        panel_move(g_desk.edge, 0, TOPBAR_H, EDGE, DESK_H - TOPBAR_H);
        break;

      case DOCK_RIGHT:
        panel_move(g_desk.edge, DESK_W - EDGE, TOPBAR_H, EDGE,
                   DESK_H - TOPBAR_H);
        break;

      default:
        panel_move(g_desk.edge, 0, DESK_H - EDGE, DESK_W, EDGE);
        break;
    }
}

static void tip_show(int item)
{
  const char *name;
  int app = item >= 0 ? g_dock[item].app : -1;
  int w;
  int x;
  int y;

  if (app < 0 || g_desk.menu != NULL)
    {
      nx_setvisibility(g_desk.tip, true);
      return;
    }

  name = g_apps[app]->name;
  w    = text_width(&g_desk_font_ui, name) + 20;
  dock_item_xy(item, &x, &y);
  x += g_desk.dockx;
  y += g_desk.docky;

  switch (g_prefs.dockpos)
    {
      case DOCK_LEFT:
        x = g_desk.dockx + g_desk.dockw + 10;
        y = y + g_dicon / 2 - TIP_H / 2;
        break;

      case DOCK_RIGHT:
        x = g_desk.dockx - 10 - w;
        y = y + g_dicon / 2 - TIP_H / 2;
        break;

      default:
        x = x + g_dicon / 2 - w / 2;
        y = g_desk.docky - TIP_H - 6;
        break;
    }

  x = x < 2 ? 2 : x + w > DESK_W - 2 ? DESK_W - 2 - w : x;

  ops_reset(&g_ops);
  ops_rrect(&g_ops, 0, 0, w, TIP_H, LUT_R6, 6, g_theme.popup,
            g_theme.popup_alpha);
  ops_text_centre(&g_ops, &g_desk_font_ui, w / 2,
                  (TIP_H - text_height(&g_desk_font_ui)) / 2, name,
                  g_theme.popup_text, 255);

  panel_move(g_desk.tip, x, y, w, TIP_H);
  ops_setlist(g_desk.tip, &g_ops);
  nx_setvisibility(g_desk.tip, false);
}

void desk_dock_draw(void)
{
  uint16_t fg = THEMED(C_WHITE, RGB(0x30, 0x30, 0x34));
  int n;
  int i;

  if (g_desk.dock == NULL)
    {
      return;
    }

  /* Its items may have changed (applications starting and ending): its
   * size with them
   */

  n = g_dlen;
  dock_build();
  if (g_dlen != n || g_desk.dockout < 0)
    {
      if (g_desk.dockout < 0)
        {
          g_desk.dockout = g_prefs.dockhide ? 0 : DOCK_STEPS;
        }

      dock_place();
    }

  ops_reset(&g_ops);

  /* A hairline of light along the top of the glass */

  ops_blend(&g_ops, 12, 0, g_desk.dockw - 24, 1, C_WHITE,
            g_theme.dock_edge);

  for (i = 0; i < g_ndock; i++)
    {
      int app = g_dock[i].app;
      int lift = g_desk.dockhover == i ? 4 : 0;
      int x;
      int y;

      dock_item_xy(i, &x, &y);
      if (app == SEP)
        {
          if (dock_vertical())
            {
              ops_blend(&g_ops, g_dpad + g_dicon / 6, y,
                        g_dicon - g_dicon / 3, 1, fg, 70);
            }
          else
            {
              ops_blend(&g_ops, x, g_dpad + g_dicon / 6, 1,
                        g_dicon - g_dicon / 3, fg, 70);
            }

          continue;
        }

      switch (g_prefs.dockpos)
        {
          case DOCK_LEFT:
            x += lift;
            break;

          case DOCK_RIGHT:
            x -= lift;
            break;

          default:
            y -= lift;
            break;
        }

      icon_put(&g_ops, app, x, y, g_dicon, 255);

      if (app != APP_LAUNCHER && app_running(app))
        {
          int dx;
          int dy;

          dock_item_xy(i, &dx, &dy);
          switch (g_prefs.dockpos)
            {
              case DOCK_LEFT:
                ops_disc(&g_ops, 3, dy + g_dicon / 2, LUT_R3, 3, fg, 200);
                break;

              case DOCK_RIGHT:
                ops_disc(&g_ops, g_desk.dockw - 3, dy + g_dicon / 2,
                         LUT_R3, 3, fg, 200);
                break;

              default:
                ops_disc(&g_ops, dx + g_dicon / 2, g_desk.dockh - 4,
                         LUT_R3, 3, fg, 200);
                break;
            }
        }
    }

  ops_setlist(g_desk.dock, &g_ops);
}

/* The dock's menu (the right button on an item) */

enum
{
  M_OPEN = 1,
  M_NEW,
  M_PIN,
  M_UNPIN,
  M_QUIT,
  M_DOCKSETTINGS
};

static void dock_pick(int id, void *arg)
{
  int app = (int)(intptr_t)arg;
  struct desk_win_s *w;

  switch (id)
    {
      case M_OPEN:
        w = desk_app_window(app);
        if (w != NULL)
          {
            desk_win_restore(w);
          }
        else
          {
            desk_launch(app);
          }
        break;

      case M_NEW:
        if (g_apps[app]->open != NULL && (w = g_apps[app]->open()) != NULL)
          {
            desk_win_title(w);
            nxtk_setvisibility(w->hwnd, false);
            desk_win_activate(w);
          }
        break;

      case M_PIN:
      case M_UNPIN:
        desk_dock_pin(app, id == M_PIN);
        break;

      case M_QUIT:
        desk_app_quit(app);
        break;

      case M_DOCKSETTINGS:
        desk_settings_show(SETTINGS_DOCK);
        break;
    }
}

static void app_menu(int app, int x, int y)
{
  struct desk_menu_item_s m[6];
  bool running = app_running(app);
  int n = 0;

  if (app == APP_LAUNCHER)
    {
      m[n].label = "Dock Settings...";
      m[n].id = M_DOCKSETTINGS;
      m[n++].enabled = true;
    }
  else
    {
      m[n].label = running ? "Show" : "Open";
      m[n].id = M_OPEN;
      m[n++].enabled = true;
      if (running && !g_apps[app]->single)
        {
          m[n].label = "New Window";
          m[n].id = M_NEW;
          m[n++].enabled = true;
        }

      m[n].label = "-";
      m[n++].id = 0;
      m[n].label = desk_dock_pinned(app) ? "Remove from Dock" :
                                           "Keep in Dock";
      m[n].id = desk_dock_pinned(app) ? M_UNPIN : M_PIN;
      m[n++].enabled = true;
      if (running)
        {
          m[n].label = "-";
          m[n++].id = 0;
          m[n].label = "Quit";
          m[n].id = M_QUIT;
          m[n++].enabled = true;
        }
    }

  desk_menu_show(x, y, m, n, dock_pick, (void *)(intptr_t)app);
}

void desk_dock_mouse(int x, int y, uint8_t buttons)
{
  static uint8_t last;
  uint8_t press = buttons & ~last;
  int hover = -1;
  int i;

  last = buttons;
  if (x >= 0 && y >= 0 && x < g_desk.dockw && y < g_desk.dockh)
    {
      int along = dock_vertical() ? y : x;

      for (i = 0; i < g_ndock; i++)
        {
          int len = g_dock[i].app == SEP ? 1 : g_dicon;

          if (g_dock[i].app != SEP && along >= g_dock[i].off - g_dpad / 2 &&
              along < g_dock[i].off + len + g_dpad / 2 + 1)
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

  if (hover < 0)
    {
      return;
    }

  if (press & NX_MOUSE_RIGHTBUTTON)
    {
      int ix;
      int iy;
      int app = g_dock[hover].app;

      dock_item_xy(hover, &ix, &iy);
      ix += g_desk.dockx;
      iy += g_desk.docky;
      nx_setvisibility(g_desk.tip, true);
      switch (g_prefs.dockpos)
        {
          case DOCK_LEFT:
            app_menu(app, g_desk.dockx + g_desk.dockw + 6, iy);
            break;

          case DOCK_RIGHT:
            app_menu(app, -(g_desk.dockx - 6), iy);
            break;

          default:
            app_menu(app, ix, -(g_desk.docky - 6));
            break;
        }

      return;
    }

  if (press & NX_MOUSE_LEFTBUTTON)
    {
      int app = g_dock[hover].app;
      struct desk_win_s *w = desk_app_window(app);

      /* A running app comes forward (or back from the dock); else starts */

      if (w != NULL && app != APP_LAUNCHER &&
          (w->minimized || w != g_desk.active || g_apps[app]->single))
        {
          if (g_desk.launcher != NULL)
            {
              desk_launcher_toggle();
            }

          desk_win_restore(w);
        }
      else
        {
          desk_launch(app);
        }
    }
}

bool desk_dock_pinned(int app)
{
  int i;

  for (i = 0; i < g_prefs.npinned; i++)
    {
      if (g_prefs.pinned[i] == app)
        {
          return true;
        }
    }

  return false;
}

/* Keep an application in the dock (at the end of the favourites), or not */

void desk_dock_pin(int app, bool pin)
{
  struct desk_win_s *w;
  int i;

  if (app <= APP_LAUNCHER || app >= APP_COUNT ||
      desk_dock_pinned(app) == pin)
    {
      return;
    }

  if (pin)
    {
      g_prefs.pinned[g_prefs.npinned++] = app;
    }
  else
    {
      for (i = 0; i < g_prefs.npinned; i++)
        {
          if (g_prefs.pinned[i] == app)
            {
              memmove(&g_prefs.pinned[i], &g_prefs.pinned[i + 1],
                      g_prefs.npinned - i - 1);
              g_prefs.npinned--;
              break;
            }
        }
    }

  g_desk.dockhover = -1;
  nx_setvisibility(g_desk.tip, true);
  desk_dock_draw();
  desk_prefs_save();

  /* Settings shows them */

  w = desk_app_window(APP_SETTINGS);
  if (w != NULL && g_apps[APP_SETTINGS]->draw != NULL)
    {
      g_apps[APP_SETTINGS]->draw(w);
    }
}

/****************************************************************************
 * Public Functions: pop-up menus
 ****************************************************************************/

static void menu_draw(void)
{
  const struct desk_font_s *f = &g_desk_font_ui;
  int y = MENU_PAD;
  int i;

  ops_reset(&g_ops);
  for (i = 0; i < g_menu.n; i++)
    {
      const struct desk_menu_item_s *it = &g_menu.item[i];

      if (strcmp(it->label, "-") == 0)
        {
          ops_blend(&g_ops, 10, y + MENU_SEP_H / 2, g_menu.w - 20, 1,
                    g_theme.popup_text, 40);
          y += MENU_SEP_H;
          continue;
        }

      if (i == g_menu.hover && it->enabled)
        {
          ops_rrect(&g_ops, 5, y, g_menu.w - 10, MENU_ITEM_H, LUT_R6, 6,
                    C_ACCENT, 255);
        }

      ops_text(&g_ops, f, 16, y + (MENU_ITEM_H - text_height(f)) / 2,
               it->label, !it->enabled ? g_theme.text_faint :
               i == g_menu.hover ? C_ACCENT_TEXT : g_theme.popup_text, 255);
      y += MENU_ITEM_H;
    }

  ops_setlist(g_desk.menu, &g_ops);
}

/* x or y negative: the menu's right or bottom edge is there */

void desk_menu_show(int x, int y, const struct desk_menu_item_s *items,
                    int n, void (*pick)(int id, void *arg), void *arg)
{
  int w = 150;
  int h = 2 * MENU_PAD;
  int i;

  desk_menu_close();
  n = n > MENU_ITEMS ? MENU_ITEMS : n;
  memcpy(g_menu.item, items, n * sizeof(items[0]));
  g_menu.n       = n;
  g_menu.hover   = -1;
  g_menu.pressed = false;
  g_menu.pick    = pick;
  g_menu.arg     = arg;

  for (i = 0; i < n; i++)
    {
      int tw = text_width(&g_desk_font_ui, items[i].label) + 32;

      w  = tw > w ? tw : w;
      h += strcmp(items[i].label, "-") == 0 ? MENU_SEP_H : MENU_ITEM_H;
    }

  g_menu.w = w;
  g_menu.h = h;
  x = x < 0 ? -x - w : x;
  y = y < 0 ? -y - h : y;
  x = x + w > DESK_W - 4 ? DESK_W - 4 - w : x < 4 ? 4 : x;
  y = y + h > DESK_H - 4 ? DESK_H - 4 - h : y < TOPBAR_H ? TOPBAR_H : y;

  g_desk.menu = panel_open(PART_MENU, NXBE_WINDOW_LAYER, x, y, w, h);
  if (g_desk.menu == NULL)
    {
      return;
    }

  panel_effects(g_desk.menu, NX_LAYER_OVERLAY, NX_FX_ROUND,
                g_theme.popup_alpha, g_theme.popup, 120);
  menu_draw();
  nx_setvisibility(g_desk.menu, false);
}

void desk_menu_close(void)
{
  if (g_desk.menu != NULL)
    {
      nx_closewindow(g_desk.menu);
      g_desk.menu = NULL;
    }
}

void desk_menu_mouse(int x, int y, uint8_t buttons)
{
  static uint8_t last;
  bool press = (buttons & ~last) != 0;
  bool release = (last & ~buttons) != 0;
  int hover = -1;
  int iy = MENU_PAD;
  int i;

  last = buttons;
  for (i = 0; i < g_menu.n; i++)
    {
      int ih = strcmp(g_menu.item[i].label, "-") == 0 ? MENU_SEP_H :
                                                         MENU_ITEM_H;

      if (x >= 0 && x < g_menu.w && y >= iy && y < iy + ih && ih ==
          MENU_ITEM_H && g_menu.item[i].enabled)
        {
          hover = i;
        }

      iy += ih;
    }

  if (hover != g_menu.hover)
    {
      g_menu.hover = hover;
      menu_draw();
    }

  if (press)
    {
      g_menu.pressed = true;
    }

  if (release && g_menu.pressed && hover >= 0)
    {
      void (*pick)(int id, void *arg) = g_menu.pick;
      int id = g_menu.item[hover].id;

      desk_menu_close();
      if (pick != NULL)
        {
          pick(id, g_menu.arg);
        }
    }
}

/****************************************************************************
 * Public Functions: the launcher
 ****************************************************************************/

/* The applications whose name has the search in it (any case) */

static bool name_matches(const char *name, const char *s)
{
  size_t n = strlen(s);

  for (; *name != '\0'; name++)
    {
      size_t i;

      for (i = 0; i < n && name[i] != '\0' &&
                  tolower((unsigned char)name[i]) ==
                  tolower((unsigned char)s[i]); i++)
        {
        }

      if (i == n)
        {
          return true;
        }
    }

  return n == 0;
}

static void launcher_filter(void)
{
  int i;

  g_l.n = 0;
  for (i = 1; i < APP_COUNT; i++)
    {
      if (name_matches(g_apps[i]->name, g_l.search.text))
        {
          g_l.apps[g_l.n++] = i;
        }
    }

  g_l.page  = 0;
  g_l.hover = -1;
  g_l.sel   = g_l.search.len > 0 && g_l.n > 0 ? 0 : -1;
}

/* The grid: as many columns and rows as fit between the search field and
 * the dock, in pages
 */

static void launcher_layout(void)
{
  int cw = TILE + TILE_GAP;
  int left = 60;
  int right = DESK_W - 60;
  int top = TOPBAR_H + 36 + SEARCH_H + 44;
  int bottom = DESK_H - 50;
  int per;
  int used;

  if (!g_prefs.dockhide)
    {
      switch (g_prefs.dockpos)
        {
          case DOCK_LEFT:
            left += DOCK_H + DOCK_MARGIN;
            break;

          case DOCK_RIGHT:
            right -= DOCK_H + DOCK_MARGIN;
            break;

          default:
            bottom -= DOCK_H + 2 * DOCK_MARGIN;
            break;
        }
    }

  g_l.cols = (right - left + TILE_GAP) / cw;
  g_l.rows = (bottom - top) / TILE_H;
  g_l.cols = g_l.cols < 1 ? 1 : g_l.cols;
  g_l.rows = g_l.rows < 1 ? 1 : g_l.rows;
  per      = g_l.cols * g_l.rows;
  g_l.npages = g_l.n > 0 ? (g_l.n + per - 1) / per : 1;
  g_l.page   = g_l.page >= g_l.npages ? g_l.npages - 1 : g_l.page;

  /* Centred on the columns used (all pages alike) */

  used  = g_l.n < g_l.cols ? (g_l.n > 0 ? g_l.n : 1) : g_l.cols;
  g_l.x0 = (left + right) / 2 - (used * cw - TILE_GAP) / 2;
  g_l.y0 = top;
}

static bool tile_pos(int k, int *x, int *y)
{
  int per = g_l.cols * g_l.rows;
  int i = k - g_l.page * per;

  if (i < 0 || i >= per || k >= g_l.n)
    {
      return false;
    }

  *x = g_l.x0 + (i % g_l.cols) * (TILE + TILE_GAP);
  *y = g_l.y0 + (i / g_l.cols) * TILE_H;
  return true;
}

static int search_x(void)
{
  return (DESK_W - SEARCH_W) / 2;
}

static int search_y(void)
{
  return TOPBAR_H + 36;
}

static int dots_y(void)
{
  return g_l.y0 + g_l.rows * TILE_H + 8;
}

void desk_launcher_draw(void)
{
  uint8_t a = (uint8_t)g_l.alpha;
  uint16_t fg = g_theme.scrim_text;
  const struct desk_font_s *f = &g_desk_font_ui;
  char fit[64];
  int sx = search_x();
  int sy = search_y();
  int ty;
  int k;

  if (g_desk.launcher == NULL)
    {
      return;
    }

  launcher_layout();
  ops_reset(&g_ops);
  ops_blend(&g_ops, 0, 0, DESK_W, DESK_H, g_theme.scrim,
            (uint8_t)(a * g_theme.scrim_alpha / 255));

  /* The search field: a pill, a magnifier, the text and its cursor */

  ops_rrect(&g_ops, sx, sy, SEARCH_W, SEARCH_H, LUT_R12, 12,
            THEMED(C_WHITE, C_WHITE), (uint8_t)(a * THEMED(36, 220) / 255));
  ops_disc(&g_ops, sx + 20, sy + SEARCH_H / 2 - 2, LUT_R6, 6, fg,
           (uint8_t)(a * 150 / 255));
  ops_disc(&g_ops, sx + 20, sy + SEARCH_H / 2 - 2, LUT_R6, 4,
           THEMED(RGB(0x40, 0x40, 0x48), C_WHITE), a);
  ops_blend(&g_ops, sx + 24, sy + SEARCH_H / 2 + 2, 2, 5, fg,
            (uint8_t)(a * 150 / 255));
  ty = sy + (SEARCH_H - text_height(f)) / 2;
  if (g_l.search.len == 0)
    {
      ops_text(&g_ops, f, sx + 36, ty, "Type to search", fg,
               (uint8_t)(a * 130 / 255));
      ops_fill(&g_ops, sx + 35, sy + 9, 1, SEARCH_H - 18, fg);
    }
  else
    {
      int tw = ui_fit(f, g_l.search.text, SEARCH_W - 56, fit, sizeof(fit));

      ops_text(&g_ops, f, sx + 36, ty, fit, fg, a);
      ops_fill(&g_ops, sx + 36 + tw + 1, sy + 9, 1, SEARCH_H - 18, fg);
    }

  /* The tiles of this page */

  for (k = 0; k < g_l.n; k++)
    {
      char name[32];
      int app = g_l.apps[k];
      int x;
      int y;

      if (!tile_pos(k, &x, &y))
        {
          continue;
        }

      list_room(&g_list, &g_ops, 40);

      if (g_l.hover == k || g_l.sel == k)
        {
          ops_rrect(&g_ops, x - 6, y - 8, TILE + 12, TILE_H - 4, LUT_R12,
                    12, g_l.sel == k ? C_ACCENT : fg,
                    (uint8_t)(a * (g_l.sel == k ? THEMED(110, 90) :
                                                  THEMED(40, 30)) / 255));
        }

      icon_put(&g_ops, app, x + (TILE - TILE_ICON) / 2, y + 4, TILE_ICON,
               a);
      ui_fit(f, g_apps[app]->name, TILE + 16, name, sizeof(name));
      ops_text_centre(&g_ops, f, x + TILE / 2, y + TILE_ICON + 12, name,
                      fg, a);
    }

  if (g_l.n == 0)
    {
      ops_text_centre(&g_ops, &g_desk_font_title, DESK_W / 2,
                      g_l.y0 + 40, "No applications found", fg,
                      (uint8_t)(a * 160 / 255));
    }

  /* The pages: a dot each, the current one brighter */

  if (g_l.npages > 1)
    {
      int dx = DESK_W / 2 - (g_l.npages * 20) / 2;

      for (k = 0; k < g_l.npages; k++)
        {
          list_room(&g_list, &g_ops, 1);
          ops_disc(&g_ops, dx + k * 20 + 10, dots_y() + 10, LUT_R3, 3, fg,
                   (uint8_t)(a * (k == g_l.page ? 230 : 80) / 255));
        }
    }

  list_set(g_desk.launcher, &g_list, &g_ops);
}

void desk_launcher_toggle(void)
{
  desk_menu_close();
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
          g_l.alpha = 0;
          ui_field_set(&g_l.search, "");
          memset(&g_l.keys, 0, sizeof(g_l.keys));
          launcher_filter();
          g_desk.launcherfade = LAUNCHER_STEPS;
          desk_launcher_draw();
          nx_setvisibility(g_desk.launcher, false);
          nx_raise(g_desk.topbar);
          nx_raise(g_desk.dock);
        }
    }

  desk_topbar_draw();
}

static void launcher_open_app(int k)
{
  if (k >= 0 && k < g_l.n)
    {
      int app = g_l.apps[k];
      struct desk_win_s *w = desk_app_window(app);

      if (w != NULL && g_apps[app]->single)
        {
          desk_launcher_toggle();
          desk_win_restore(w);
        }
      else
        {
          desk_launch(app);
        }
    }
}

void desk_launcher_mouse(int x, int y, uint8_t buttons)
{
  uint8_t press = buttons & ~g_l.last;
  uint8_t release = g_l.last & ~buttons;
  int hover = -1;
  int k;

  g_l.last = buttons;
  for (k = 0; k < g_l.n; k++)
    {
      int tx;
      int ty;

      if (tile_pos(k, &tx, &ty) && x >= tx && x < tx + TILE && y >= ty &&
          y < ty + TILE_H - 12)
        {
          hover = k;
        }
    }

  if (hover != g_l.hover)
    {
      g_l.hover = hover;
      desk_launcher_draw();
    }

  if ((press & NX_MOUSE_RIGHTBUTTON) && hover >= 0)
    {
      app_menu(g_l.apps[hover], x, y);
      return;
    }

  if ((release & NX_MOUSE_LEFTBUTTON) == 0)
    {
      return;
    }

  /* The search field and the page dots are not "elsewhere" */

  if (y >= search_y() && y < search_y() + SEARCH_H && x >= search_x() &&
      x < search_x() + SEARCH_W)
    {
      return;
    }

  if (g_l.npages > 1 && y >= dots_y() && y < dots_y() + 20)
    {
      int dx = DESK_W / 2 - (g_l.npages * 20) / 2;

      if (x >= dx && x < dx + g_l.npages * 20)
        {
          g_l.page = (x - dx) / 20;
          g_l.hover = -1;
          desk_launcher_draw();
          return;
        }
    }

  if (hover >= 0)
    {
      launcher_open_app(hover);
    }
  else
    {
      desk_launcher_toggle();
    }
}

/* Keys while the launcher is open: the search, arrows move the selection
 * (across pages), Page Up/Down the page, Enter opens, Escape clears the
 * search or closes
 */

static void launcher_move(int d)
{
  int per = g_l.cols * g_l.rows;

  if (g_l.n == 0)
    {
      return;
    }

  if (g_l.sel < 0)
    {
      g_l.sel = g_l.page * per;
    }
  else if (g_l.sel + d >= 0 && g_l.sel + d < g_l.n)
    {
      g_l.sel += d;
    }

  g_l.sel  = g_l.sel >= g_l.n ? g_l.n - 1 : g_l.sel;
  g_l.page = per > 0 ? g_l.sel / per : 0;
}

bool desk_launcher_key(const uint8_t *ch, int n)
{
  int keys[16];
  int nk;
  int i;

  if (g_desk.launcher == NULL)
    {
      return false;
    }

  nk = ui_keys(&g_l.keys, ch, n, keys, 16);
  for (i = 0; i < nk; i++)
    {
      int key = keys[i];

      switch (key & KEY_MASK)
        {
          case KEY_ESC:
            if (g_l.search.len > 0)
              {
                ui_field_set(&g_l.search, "");
                launcher_filter();
                break;
              }

            desk_launcher_toggle();
            return true;

          case '\r':
          case '\n':
            if (g_l.sel >= 0)
              {
                launcher_open_app(g_l.sel);
                return true;
              }
            break;

          case KEY_LEFT:
            launcher_move(-1);
            break;

          case KEY_RIGHT:
          case '\t':
            launcher_move(1);
            break;

          case KEY_UP:
            launcher_move(-g_l.cols);
            break;

          case KEY_DOWN:
            launcher_move(g_l.cols);
            break;

          case KEY_PGUP:
          case KEY_PGDN:
            g_l.page += (key & KEY_MASK) == KEY_PGUP ? -1 : 1;
            g_l.page  = g_l.page < 0 ? 0 : g_l.page >= g_l.npages ?
                        g_l.npages - 1 : g_l.page;
            g_l.sel   = -1;
            break;

          default:
            if (ui_field_key(&g_l.search, key))
              {
                launcher_filter();
              }
            break;
        }
    }

  desk_launcher_draw();
  return true;
}

/****************************************************************************
 * Public Functions: all of them
 ****************************************************************************/

/* The work area: the display less the top bar and the dock's side (all of
 * it when the dock hides itself)
 */

void desk_layout(void)
{
  int t = DOCK_H + 2 * DOCK_MARGIN;

  g_desk.work.pt1.x = 0;
  g_desk.work.pt1.y = TOPBAR_H + 6;
  g_desk.work.pt2.x = DESK_W - 1;
  g_desk.work.pt2.y = DESK_H - 1;
  if (!g_prefs.dockhide)
    {
      switch (g_prefs.dockpos)
        {
          case DOCK_LEFT:
            g_desk.work.pt1.x += t - DOCK_MARGIN;
            break;

          case DOCK_RIGHT:
            g_desk.work.pt2.x -= t - DOCK_MARGIN;
            break;

          default:
            g_desk.work.pt2.y -= t;
            break;
        }
    }
}

int desk_panels_init(void)
{
  icons_init();

#ifdef CONFIG_FPCDESK_WALLPAPER
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
#endif

  /* The top bar */

  g_desk.topbar = panel_open(PART_TOPBAR, NXBE_WINDOW_LAYER, 0, 0, DESK_W,
                             TOPBAR_H);
  panel_effects(g_desk.topbar, NX_LAYER_PANEL, 0, g_theme.panel_alpha,
                g_theme.panel_tint, 0);
  desk_topbar_draw();
  nx_setvisibility(g_desk.topbar, false);

  /* The dock (desk_dock_draw() gives it its size and place) */

  g_desk.dock  = panel_open(PART_DOCK, NXBE_WINDOW_LAYER, 0, 0, 10, 10);
  panel_effects(g_desk.dock, NX_LAYER_PANEL, NX_FX_ROUND,
                g_theme.dock_alpha, g_theme.dock_tint, 110);
  desk_dock_draw();
  nx_setvisibility(g_desk.dock, false);
  edge_place();

  /* The dock's tooltip, shown on hover */

  g_desk.tip = panel_open(PART_TIP, NXBE_WINDOW_LAYER, 0, 0, 10, 10);
  panel_effects(g_desk.tip, NX_LAYER_OVERLAY, 0, 0, 0, 0);
  return 0;
}

/* The display has a new size (a display mode), or the theme or the dock's
 * settings have changed: the wallpaper first (its surface is the
 * largest), then the top bar, the dock and the launcher
 */

void desk_panels_relayout(void)
{
  struct nxgl_size_s size;

  size.w = DESK_W;
  size.h = DESK_H;
  if (g_desk.wallpaper != NULL)
    {
      nx_setsize(g_desk.wallpaper, &size);
      desk_wallpaper_draw();
    }

  size.h = TOPBAR_H;
  nx_setsize(g_desk.topbar, &size);
  panel_effects(g_desk.topbar, NX_LAYER_PANEL, 0, g_theme.panel_alpha,
                g_theme.panel_tint, 0);
  desk_topbar_draw();

  panel_effects(g_desk.dock, NX_LAYER_PANEL, NX_FX_ROUND,
                g_theme.dock_alpha, g_theme.dock_tint, 110);
  g_desk.dockhover = -1;
  g_desk.dockout   = g_prefs.dockhide ? 0 : DOCK_STEPS;
  g_desk.dockwant  = false;
  g_dlen           = -1;                  /* Placed again */
  desk_dock_draw();
  edge_place();
  nx_setvisibility(g_desk.tip, true);
  desk_menu_close();

  if (g_desk.launcher != NULL)
    {
      size.h = DESK_H;
      nx_setsize(g_desk.launcher, &size);
      desk_launcher_draw();
    }
}

/* Every pointer event: a hidden dock comes out while the pointer is on
 * its edge, the dock, its tooltip or its menu, and goes when it leaves
 */

void desk_panels_mouse(int part, int x, int y, uint8_t buttons)
{
  bool want;

  if (!g_prefs.dockhide)
    {
      return;
    }

  want = part == PART_EDGE || part == PART_DOCK || part == PART_TIP ||
         (part == PART_MENU && g_desk.dockwant) ||
         (g_desk.menu != NULL && g_desk.dockwant);
  g_desk.dockwant = want;
}

void desk_panels_tick(void)
{
  desk_topbar_draw();
}

bool desk_panels_animate(void)
{
  bool more = false;

  if (g_desk.launcher != NULL && g_desk.launcherfade > 0)
    {
      g_desk.launcherfade--;
      g_l.alpha = 255 - g_desk.launcherfade * 255 / LAUNCHER_STEPS;
      desk_launcher_draw();
      more |= g_desk.launcherfade > 0;
    }

  /* The dock sliding out or away */

  if (g_prefs.dockhide && g_desk.dock != NULL)
    {
      int target = g_desk.dockwant ? DOCK_STEPS : 0;

      if (g_desk.dockout != target)
        {
          g_desk.dockout += g_desk.dockout < target ? 1 : -1;
          dock_place();
          if (g_desk.dockout == 0)
            {
              g_desk.dockhover = -1;
              nx_setvisibility(g_desk.tip, true);
              desk_dock_draw();
            }

          more = true;
        }
    }

  return more;
}
