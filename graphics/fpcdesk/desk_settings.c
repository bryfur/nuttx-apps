/****************************************************************************
 * apps/graphics/fpcdesk/desk_settings.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings: a sidebar of pages (Appearance, Display, Dock, Mouse &
 * Keyboard, Date & Time, About this computer), each a function that draws
 * it and says where its controls are (ctl_add()), and one that acts on a
 * control (a press, a slider's drag).  Everything set is kept in g_prefs
 * and saved at once (desk_prefs_save()).
 *
 * A new section goes into its page's draw function (its controls with ids
 * of their own in the enum below) and into settings_act().
 ****************************************************************************/

#include <nuttx/config.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "desk.h"
#include "desk_ui.h"
#include "desk_widgets.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SET_W          700             /* The window's client area */
#define SET_H          492
#define SIDE_W         188             /* The sidebar */
#define SIDE_ROW       32
#define PAD            24              /* Around a page */
#define MAX_CTL        64
#define SWATCH_GAP     14              /* Between wallpapers' previews */
#define NWALLS         4               /* The drawn ones, then the image */
#define THEME_W        132             /* A theme's preview */
#define THEME_H        80
#define RES_MAX        4

/* Controls: what each is (for drawing its hover and acting on it) */

enum
{
  K_BUTTON = 0,                        /* Acts on a press */
  K_SEGMENTS,                          /* n parts across */
  K_SLIDER,                            /* Follows a drag */
  K_STEPPER,                           /* - and + */
  K_FIELD                              /* Takes the keys */
};

/* Their ids */

enum
{
  S_NONE = 0,
  S_PAGE,                              /* + page */
  S_THEME = S_PAGE + SETTINGS_PAGES,   /* + theme */
  S_ACCENT = S_THEME + THEME_COUNT,    /* + accent */
  S_SHADOW = S_ACCENT + ACCENT_COUNT,
  S_BORDER,
  S_CLOCK24,
  S_CLOCKDATE,
  S_CLOCKSECS,
  S_MODE,
  S_WALL,                              /* + wallpaper */
  S_DOCKSIZE = S_WALL + 8,
  S_DOCKPOS,
  S_DOCKHIDE,
  S_REPEAT,
  S_REPDELAY,
  S_REPRATE,
  S_PTRSPEED,
  S_TEST,
  S_YEAR,
  S_MONTH,
  S_DAY,
  S_HOUR,
  S_MINUTE,
  S_TZ,
  S_PIN                                /* + app */
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ctl_s
{
  int16_t x;                           /* Frame coordinates */
  int16_t y;
  int16_t w;
  int16_t h;
  uint8_t id;
  uint8_t kind;
  uint8_t n;                           /* K_SEGMENTS: how many */
};

struct settings_s
{
  struct desk_win_s *w;
  int page;
  int hot;                             /* Control under the pointer */
  int hotpart;                         /* ... which part of it */
  int drag;                            /* Slider being dragged */
  uint8_t buttons;
  struct ctl_s ctl[MAX_CTL];
  int nctl;
  struct ui_field_s test;              /* Mouse & Keyboard: try the keys */
  struct ui_keys_s keys;
  bool input;                          /* /dev/desktop is there */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The operations being built: from the heap while a window is open */

static struct desk_ops_s *g_op;
static NXWINDOW g_hwnd;
static struct settings_s g_st;

static const char * const g_pages[SETTINGS_PAGES] =
{
  "Appearance", "Display", "Dock", "Mouse & Keyboard", "Date & Time",
  "About this computer"
};

static const uint16_t g_pagetile[SETTINGS_PAGES] =
{
  RGB(0x0a, 0x84, 0xff), RGB(0x5e, 0x5c, 0xe6), RGB(0x8e, 0x8e, 0x93),
  RGB(0x34, 0xc7, 0x59), RGB(0xff, 0x95, 0x00), RGB(0x64, 0x7a, 0x96)
};

#ifdef CONFIG_FPCDESK_WALLPAPER
static const char * const g_wallnames[NWALLS] =
{
  "Aurora", "Sunset", "Graphite", "Image"
};
#endif

/****************************************************************************
 * Private Functions: drawing
 ****************************************************************************/

static void need(int n)
{
  ui_flush(g_hwnd, g_op, n);
}

static void ctl_add(int id, int kind, int x, int y, int w, int h, int n)
{
  struct ctl_s *c;

  if (g_st.nctl == MAX_CTL)
    {
      return;
    }

  c       = &g_st.ctl[g_st.nctl++];
  c->x    = x;
  c->y    = y;
  c->w    = w;
  c->h    = h;
  c->id   = id;
  c->kind = kind;
  c->n    = n;
}

/* Text, with room made for it in the list first */

static void text(const struct desk_font_s *f, int x, int y, const char *str,
                 uint16_t c)
{
  need(strlen(str) + 1);
  ops_text(g_op, f, x, y, str, c, 255);
}

static void text_centre(const struct desk_font_s *f, int cx, int y,
                        const char *str, uint16_t c)
{
  need(strlen(str) + 1);
  ops_text_centre(g_op, f, cx, y, str, c, 255);
}

static bool hot(int id)
{
  return g_st.hot == id;
}

static void heading(int x, int *y, const char *s)
{
  text(&g_desk_font_title, x, *y, s, g_theme.text);
  *y += 40;
}

static void caption(int x, int *y, const char *s)
{
  text(&g_desk_font_uib, x + 2, *y, s, g_theme.text);
  *y += 22;
}

/* A switch at the right of a card's row */

static void row_switch(int id, int x, int y, int w, const char *label,
                       const char *cap, bool on)
{
  int sx = x + w - 14 - WG_SWITCH_W;
  int sy = y + (WG_ROW_H - WG_SWITCH_H) / 2;

  need(80);
  wg_row_label(g_op, x, y, label, cap);
  wg_switch(g_op, sx, sy, on, hot(id));
  ctl_add(id, K_BUTTON, sx, sy, WG_SWITCH_W, WG_SWITCH_H, 0);
}

static void row_segments(int id, int x, int y, int w, int sw,
                         const char *label, const char * const *labels,
                         int n, int sel)
{
  int sx = x + w - 14 - sw;
  int sy = y + (WG_ROW_H - 28) / 2;

  need(90);
  wg_row_label(g_op, x, y, label, NULL);
  wg_segments(g_op, sx, sy, sw, 28, labels, n, sel,
              hot(id) ? g_st.hotpart : -1);
  ctl_add(id, K_SEGMENTS, sx, sy, sw, 28, n);
}

/* A slider with its value's text left of it */

static void row_slider(int id, int x, int y, int w, const char *label,
                       const char *cap, int v, int min, int max,
                       const char *value)
{
  int sw = 200;
  int sx = x + w - 14 - sw;
  int sy = y + (WG_ROW_H - WG_SLIDER_H) / 2;

  need(90);
  wg_row_label(g_op, x, y, label, cap);
  text(&g_desk_font_ui, sx - 12 -
           text_width(&g_desk_font_ui, value),
           y + (WG_ROW_H - text_height(&g_desk_font_ui)) / 2, value,
           g_theme.text_dim);
  wg_slider(g_op, sx, sy, sw, v, min, max, hot(id) || g_st.drag == id);
  ctl_add(id, K_SLIDER, sx, sy, sw, WG_SLIDER_H, 0);
}

static void stepper(int id, int x, int y, int w, const char *value)
{
  need(12);
  wg_stepper(g_op, x, y, w, value, hot(id) ? g_st.hotpart : 0);
  ctl_add(id, K_STEPPER, x, y, w, WG_STEP, 0);
}

/* A small picture of a theme: a desktop with a window on it */

static void theme_preview(int t, int x, int y)
{
  const struct desk_theme_s *th = desk_theme_get(t);
  bool sel = g_prefs.theme == t;
  uint16_t accent = desk_accent_colour(g_prefs.accent);
  int wx = x + 16;
  int wy = y + 14;
  int ww = THEME_W - 32;
  int wh = THEME_H - 24;

  need(24);
  if (sel || hot(S_THEME + t))
    {
      ops_rrect(g_op, x - 4, y - 4, THEME_W + 8, THEME_H + 8, LUT_R12, 12,
                sel ? C_ACCENT : g_theme.control_hover, 255);
    }

  ops_rrect(g_op, x, y, THEME_W, THEME_H, LUT_R6, 6,
            t == THEME_DARK ? RGB(0x2c, 0x30, 0x48) : RGB(0x9c, 0xa6, 0xb6),
            255);
  ops_rrect(g_op, x, y, THEME_W, 8, LUT_R6, 4, th->panel_tint,
            th->panel_alpha);
  ops_rrect(g_op, wx, wy, ww, wh, LUT_R6, 6, th->title_active, 255);
  ops_rrect(g_op, wx, wy + 12, ww, wh - 12, LUT_R6, 6, th->window_bg, 255);
  ops_fill(g_op, wx, wy + 12, ww, 6, th->window_bg);
  ops_disc(g_op, wx + 7, wy + 6, LUT_R3, 2, C_CLOSE, 255);
  ops_disc(g_op, wx + 13, wy + 6, LUT_R3, 2, C_MINIMIZE, 255);
  ops_disc(g_op, wx + 19, wy + 6, LUT_R3, 2, C_MAXIMIZE, 255);
  ops_fill(g_op, wx + 8, wy + 20, ww - 30, 3, th->text);
  ops_fill(g_op, wx + 8, wy + 27, ww - 44, 3, th->text_dim);
  ops_fill(g_op, wx + 8, wy + 34, ww - 36, 3, th->text_dim);
  ops_rrect(g_op, wx + ww - 30, wy + wh - 14, 24, 9, LUT_R3, 3, accent,
            255);
  text_centre(sel ? &g_desk_font_uib : &g_desk_font_ui,
                  x + THEME_W / 2, y + THEME_H + 8,
                  t == THEME_DARK ? "Dark" : "Light",
                  sel ? g_theme.text : g_theme.text_dim);
  ctl_add(S_THEME + t, K_BUTTON, x, y, THEME_W, THEME_H + 24, 0);
}

/****************************************************************************
 * Private Functions: the pages
 ****************************************************************************/

static void page_appearance(int x, int y, int w)
{
  static const char * const shadows[4] =
  {
    "Off", "Light", "Normal", "Strong"
  };

  char cap[40];
  int i;

  heading(x, &y, "Appearance");
  theme_preview(THEME_DARK, x + 4, y + 4);
  theme_preview(THEME_LIGHT, x + 4 + THEME_W + 24, y + 4);
  y += THEME_H + 44;

  need(40);
  wg_card(g_op, x, y, w, 3);
  snprintf(cap, sizeof(cap), "%s", desk_accent_name(g_prefs.accent));
  wg_row_label(g_op, x, y, "Accent colour", cap);
  for (i = 0; i < ACCENT_COUNT; i++)
    {
      int cx = x + w - 14 - WG_SWATCH_R - (ACCENT_COUNT - 1 - i) * 30;
      int cy = y + WG_ROW_H / 2;

      need(4);
      wg_swatch(g_op, cx, cy, desk_accent_colour(i), g_prefs.accent == i,
                hot(S_ACCENT + i));
      ctl_add(S_ACCENT + i, K_BUTTON, cx - 14, cy - 14, 28, 28, 0);
    }

  row_segments(S_SHADOW, x, y + WG_ROW_H, w, 260, "Window shadows",
               shadows, 4, g_prefs.shadow);
  row_switch(S_BORDER, x, y + 2 * WG_ROW_H, w, "Window edges",
             "A hairline around each window", g_prefs.border);
  y += 3 * WG_ROW_H + 16;

  need(8);
  need(12);
  wg_card(g_op, x, y, w, 3);
  row_switch(S_CLOCK24, x, y, w, "24-hour clock", NULL, g_prefs.clock24);
  row_switch(S_CLOCKDATE, x, y + WG_ROW_H, w, "Show the date",
             "In the top bar", g_prefs.clockdate);
  row_switch(S_CLOCKSECS, x, y + 2 * WG_ROW_H, w, "Show seconds", NULL,
             g_prefs.clocksecs);
}

static void page_display(int x, int y, int w)
{
  static char labels[RES_MAX][16];
  static const char *lp[RES_MAX];
  struct nxgl_size_s size;
  int n;

  heading(x, &y, "Display");
  caption(x, &y, "Resolution");

  for (n = 0; n < RES_MAX && nx_getdisplaymode(g_desk.hnx, n, &size) >= 0;
       n++)
    {
      snprintf(labels[n], sizeof(labels[n]), "%d x %d", size.w, size.h);
      lp[n] = labels[n];
    }

  if (n > 0)
    {
      need(40);
      wg_segments(g_op, x, y, w, 30, lp, n,
                  nx_getdisplaymode(g_desk.hnx, -1, NULL),
                  hot(S_MODE) ? g_st.hotpart : -1);
      ctl_add(S_MODE, K_SEGMENTS, x, y, w, 30, n);
    }
  else
    {
      text(&g_desk_font_ui, x + 2, y, "This display has one mode",
               g_theme.text_dim);
    }

  y += 50;

#ifdef CONFIG_FPCDESK_WALLPAPER
  {
    int i;

    /* Four previews across the page: the three drawn ones and the image
     * (wallpaper.bmp at home, shown in BC1 by a v6 GPU)
     */

    int sw = (w - 8 - (NWALLS - 1) * SWATCH_GAP) / NWALLS;
    int sh = sw * 2 / 3;

    caption(x, &y, "Wallpaper");
    text(&g_desk_font_ui, x + 2, y,
             "Drawn by the GPU, or an image from your home folder",
             g_theme.text_dim);
    y += 28;

    for (i = 0; i < NWALLS; i++)
      {
        int sx = x + 4 + i * (sw + SWATCH_GAP);

        need(24);
        if (g_desk.wallstyle == i || hot(S_WALL + i))
          {
            ops_rrect(g_op, sx - 4, y - 4, sw + 8, sh + 8,
                      LUT_R12, 12, g_desk.wallstyle == i ? C_ACCENT :
                      g_theme.control_hover, 255);
          }

        desk_wallpaper_preview(g_op, i, sx, y, sw, sh);
        text_centre(g_desk.wallstyle == i ? &g_desk_font_uib :
                        &g_desk_font_ui, sx + sw / 2,
                        y + sh + 10, g_wallnames[i],
                        g_desk.wallstyle == i ? g_theme.text :
                        g_theme.text_dim);
        ctl_add(S_WALL + i, K_BUTTON, sx, y, sw, sh + 26, 0);
      }
  }
#else
  text(&g_desk_font_ui, x + 2, y,
           "No wallpaper in this build (CONFIG_FPCDESK_WALLPAPER).",
           g_theme.text_dim);
#endif
}

static void page_dock(int x, int y, int w)
{
  static const char * const where[3] =
  {
    "Left", "Bottom", "Right"
  };

  static const uint8_t order[3] =
  {
    DOCK_LEFT, DOCK_BOTTOM, DOCK_RIGHT
  };

  char value[16];
  int sel = 1;
  int col;
  int k = 0;
  int i;

  for (i = 0; i < 3; i++)
    {
      sel = order[i] == g_prefs.dockpos ? i : sel;
    }

  heading(x, &y, "Dock");
  need(12);
  wg_card(g_op, x, y, w, 3);
  snprintf(value, sizeof(value), "%d", g_prefs.dockicon);
  row_slider(S_DOCKSIZE, x, y, w, "Size", "Of its icons", g_prefs.dockicon,
             DOCK_ICON_MIN, DOCK_ICON_MAX, value);
  row_segments(S_DOCKPOS, x, y + WG_ROW_H, w, 240, "Position on screen",
               where, 3, sel);
  row_switch(S_DOCKHIDE, x, y + 2 * WG_ROW_H, w, "Hide automatically",
             "It comes back when the pointer touches the edge",
             g_prefs.dockhide);
  y += 3 * WG_ROW_H + 18;

  caption(x, &y, "In the Dock");
  text(&g_desk_font_small, x + 2, y - 2,
           "Or: the right button on an icon in the Dock or the launcher",
           g_theme.text_dim);
  y += 20;

  /* The applications, in two columns */

  col = (w - 8) / 2;
  for (i = 1; i < APP_COUNT; i++)
    {
      int cx = x + (k % 2) * (col + 8);
      int cy = y + (k / 2) * 34;
      bool pinned = desk_dock_pinned(i);

      need(40);
      if (hot(S_PIN + i))
        {
          ops_rrect(g_op, cx, cy, col, 32, LUT_R6, 6, C_HILITE,
                    THEMED(18, 12));
        }

      wg_check(g_op, cx + 8, cy + 7, pinned, false);
      if (g_apps[i]->icon != NULL)
        {
          g_apps[i]->icon(g_op, cx + 36, cy + 2, 28, 255);
        }

      text(&g_desk_font_ui, cx + 74,
               cy + (32 - text_height(&g_desk_font_ui)) / 2,
               g_apps[i]->name, g_theme.text);
      ctl_add(S_PIN + i, K_BUTTON, cx, cy, col, 32, 0);
      k++;
    }
}

static void page_input(int x, int y, int w)
{
  char value[24];

  heading(x, &y, "Mouse & Keyboard");
  need(12);
  wg_card(g_op, x, y, w, 4);
  row_switch(S_REPEAT, x, y, w, "Key repeat", "A key held down repeats",
             g_prefs.repdelay > 0);
  snprintf(value, sizeof(value), "%d ms",
           g_prefs.repdelay > 0 ? g_prefs.repdelay : 500);
  row_slider(S_REPDELAY, x, y + WG_ROW_H, w, "Delay until repeat", NULL,
             g_prefs.repdelay > 0 ? g_prefs.repdelay : 500, 150, 1000,
             value);
  snprintf(value, sizeof(value), "%d a second", 1000 / g_prefs.reprate);
  row_slider(S_REPRATE, x, y + 2 * WG_ROW_H, w, "Repeat rate", NULL,
             1000 / g_prefs.reprate, 5, 50, value);

  need(30 + UI_FIELD_MAX / 4);
  wg_row_label(g_op, x, y + 3 * WG_ROW_H, "Try it", NULL);
  ui_field_draw(g_op, &g_st.test, x + w - 14 - 240, y + 3 * WG_ROW_H + 8,
                240, 28, true);
  ctl_add(S_TEST, K_FIELD, x + w - 14 - 240, y + 3 * WG_ROW_H + 8, 240, 28,
          0);
  y += 4 * WG_ROW_H + 16;

  need(12);
  wg_card(g_op, x, y, w, 1);
  snprintf(value, sizeof(value), "%d%%", g_prefs.ptrspeed);
  row_slider(S_PTRSPEED, x, y, w, "Pointer speed", "How far it goes for "
             "the mouse", g_prefs.ptrspeed, 25, 300, value);
  y += WG_ROW_H + 16;

  if (!g_st.input)
    {
      text(&g_desk_font_small, x + 2, y,
               "No desktop session (/dev/desktop): these have no effect.",
               g_theme.text_dim);
    }
}

static void page_datetime(int x, int y, int w)
{
  static const char * const months[12] =
  {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
    "Nov", "Dec"
  };

  char buf[40];
  struct tm tm;
  int off = g_prefs.tzoff < 0 ? -g_prefs.tzoff : g_prefs.tzoff;
  int sx;

  desk_localtime(&tm);
  heading(x, &y, "Date & Time");

  strftime(buf, sizeof(buf), g_prefs.clock24 ? "%H:%M:%S" : "%l:%M:%S %p",
           &tm);
  text_centre(&g_desk_font_big, x + w / 2, y, buf, g_theme.text);
  strftime(buf, sizeof(buf), "%A, %e %B %Y", &tm);
  text_centre(&g_desk_font_ui, x + w / 2, y + 58, buf,
                  g_theme.text_dim);
  y += 92;

  need(60);
  wg_card(g_op, x, y, w, 3);
  wg_row_label(g_op, x, y, "Date", NULL);
  sx = x + w - 14 - 3 * 96 - 16;
  snprintf(buf, sizeof(buf), "%d", tm.tm_year + 1900);
  stepper(S_YEAR, sx, y + 10, 96, buf);
  stepper(S_MONTH, sx + 104, y + 10, 96, months[tm.tm_mon % 12]);
  snprintf(buf, sizeof(buf), "%d", tm.tm_mday);
  stepper(S_DAY, sx + 208, y + 10, 96, buf);

  y += WG_ROW_H;
  need(40);
  wg_row_label(g_op, x, y, "Time", "Seconds start again at 0");
  sx = x + w - 14 - 2 * 96 - 8;
  snprintf(buf, sizeof(buf), "%02d", tm.tm_hour);
  stepper(S_HOUR, sx, y + 10, 96, buf);
  snprintf(buf, sizeof(buf), "%02d", tm.tm_min);
  stepper(S_MINUTE, sx + 104, y + 10, 96, buf);

  y += WG_ROW_H;
  need(40);
  wg_row_label(g_op, x, y, "Time zone", "Offset from UTC");
  snprintf(buf, sizeof(buf), "UTC%c%02d:%02d",
           g_prefs.tzoff < 0 ? '-' : '+', off / 60, off % 60);
  stepper(S_TZ, x + w - 14 - 150, y + 10, 150, buf);
  y += WG_ROW_H + 16;

  text(&g_desk_font_small, x + 2, y,
           "There is no battery-backed clock: the time starts again at "
           "each boot.", g_theme.text_dim);
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static void settings_draw(struct desk_win_s *w)
{
  int x = SIDE_W + PAD;
  int cw = w->csize.w - x - PAD;
  int y = TITLE_H + 20;
  int i;

  if (g_op == NULL)
    {
      return;
    }

  g_st.w    = w;
  g_hwnd    = (NXWINDOW)w->hwnd;
  g_st.nctl = 0;

  ops_reset(g_op);

  /* The sidebar */

  ops_fill(g_op, 0, TITLE_H, SIDE_W, w->csize.h, g_theme.sidebar);
  ops_fill(g_op, SIDE_W, TITLE_H, 1, w->csize.h, g_theme.separator);
  for (i = 0; i < SETTINGS_PAGES; i++)
    {
      int ry = TITLE_H + 12 + i * (SIDE_ROW + 2);

      need(12);
      wg_sidebar_row(g_op, 10, ry, SIDE_W - 20, SIDE_ROW, g_pages[i],
                     g_pagetile[i], g_st.page == i, hot(S_PAGE + i));
      ctl_add(S_PAGE + i, K_BUTTON, 10, ry, SIDE_W - 20, SIDE_ROW, 0);
    }

  /* The page */

  need(4);
  ops_fill(g_op, SIDE_W + 1, TITLE_H, w->csize.w - SIDE_W - 1, w->csize.h,
           C_WINDOW_BG);

  switch (g_st.page)
    {
      case SETTINGS_APPEARANCE:
        page_appearance(x, y, cw);
        break;

      case SETTINGS_DISPLAY:
        page_display(x, y, cw);
        break;

      case SETTINGS_DOCK:
        page_dock(x, y, cw);
        break;

      case SETTINGS_INPUT:
        page_input(x, y, cw);
        break;

      case SETTINGS_DATETIME:
        page_datetime(x, y, cw);
        break;

      default:
        ops_draw(g_hwnd, g_op);
        desk_about_draw(g_hwnd, x, TITLE_H, cw);
        break;
    }

  ops_draw(g_hwnd, g_op);
}

/* Set the clock: one field of the date or time up or down, within its
 * range (it wraps, as a watch's crown does)
 */

static void time_step(int id, int d)
{
  static const uint8_t mdays[12] =
  {
    31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
  };

  struct timespec ts;
  struct tm tm;
  time_t t;

  clock_gettime(CLOCK_REALTIME, &ts);
  t = ts.tv_sec + (time_t)g_prefs.tzoff * 60;
  gmtime_r(&t, &tm);

  switch (id)
    {
      case S_YEAR:
        tm.tm_year += d;
        tm.tm_year = tm.tm_year < 70 ? 70 : tm.tm_year > 199 ? 199 :
                     tm.tm_year;
        break;

      case S_MONTH:
        tm.tm_mon = (tm.tm_mon + 12 + d) % 12;
        break;

      case S_DAY:
        tm.tm_mday = (tm.tm_mday - 1 + mdays[tm.tm_mon] + d) %
                     mdays[tm.tm_mon] + 1;
        break;

      case S_HOUR:
        tm.tm_hour = (tm.tm_hour + 24 + d) % 24;
        break;

      default:
        tm.tm_min = (tm.tm_min + 60 + d) % 60;
        tm.tm_sec = 0;
        ts.tv_nsec = 0;
        break;
    }

  /* February has 28 days most years: the 29th becomes March's 1st */

  ts.tv_sec = timegm(&tm) - (time_t)g_prefs.tzoff * 60;
  clock_settime(CLOCK_REALTIME, &ts);
  desk_topbar_draw();
}

/* Act on control c: pressed at (x, y), or dragged there (a slider) */

static void settings_act(struct ctl_s *c, int x, int y, bool done)
{
  int id = c->id;
  int part = c->kind == K_SEGMENTS ? wg_segment_at(c->x, c->w, c->n, x) :
             c->kind == K_STEPPER ? wg_stepper_at(c->x, c->w, x) : 0;
  int v;

  if (id >= S_PAGE && id < S_PAGE + SETTINGS_PAGES)
    {
      g_st.page = id - S_PAGE;
      return;
    }

  if (id >= S_THEME && id < S_THEME + THEME_COUNT)
    {
      g_prefs.theme = id - S_THEME;
      desk_theme_set(g_prefs.theme, g_prefs.accent);
      desk_theme_changed();
      desk_prefs_save();
      return;
    }

  if (id >= S_ACCENT && id < S_ACCENT + ACCENT_COUNT)
    {
      g_prefs.accent = id - S_ACCENT;
      desk_theme_set(g_prefs.theme, g_prefs.accent);
      desk_theme_changed();
      desk_prefs_save();
      return;
    }

#ifdef CONFIG_FPCDESK_WALLPAPER
  if (id >= S_WALL && id < S_WALL + NWALLS)
    {
      if (g_desk.wallstyle != id - S_WALL)
        {
          /* The image may not be there (or the GPU cannot show it): the
           * first drawn one then (desk_wallpaper_draw())
           */

          g_desk.wallstyle = id - S_WALL;
          desk_wallpaper_draw();
          desk_prefs_save();
        }

      return;
    }
#endif

  if (id >= S_PIN && id < S_PIN + APP_COUNT)
    {
      desk_dock_pin(id - S_PIN, !desk_dock_pinned(id - S_PIN));
      return;
    }

  switch (id)
    {
      case S_SHADOW:
        if (part >= 0)
          {
            g_prefs.shadow = part;
            desk_theme_changed();
            desk_prefs_save();
          }
        break;

      case S_BORDER:
        g_prefs.border = !g_prefs.border;
        desk_theme_changed();
        desk_prefs_save();
        break;

      case S_CLOCK24:
      case S_CLOCKDATE:
      case S_CLOCKSECS:
        if (id == S_CLOCK24)
          {
            g_prefs.clock24 = !g_prefs.clock24;
          }
        else if (id == S_CLOCKDATE)
          {
            g_prefs.clockdate = !g_prefs.clockdate;
          }
        else
          {
            g_prefs.clocksecs = !g_prefs.clocksecs;
          }

        desk_topbar_draw();
        desk_prefs_save();
        break;

      /* A resolution: the desktop lays itself out again when the server
       * says the mode has changed (EV_MODE), Settings with it
       */

      case S_MODE:
        if (part >= 0 && part != nx_getdisplaymode(g_desk.hnx, -1, NULL) &&
            nx_setdisplaymode(g_desk.hnx, part) >= 0)
          {
            desk_prefs_save();
          }
        break;

      case S_DOCKSIZE:
        v = wg_slider_value(c->x, c->w, DOCK_ICON_MIN, DOCK_ICON_MAX, x);
        if (v != g_prefs.dockicon)
          {
            g_prefs.dockicon = v;
            desk_dock_draw();                  /* The dock follows */
          }

        if (done)
          {
            desk_relayout();                   /* And the windows */
            desk_prefs_save();
          }
        break;

      case S_DOCKPOS:
        if (part >= 0)
          {
            static const uint8_t order[3] =
            {
              DOCK_LEFT, DOCK_BOTTOM, DOCK_RIGHT
            };

            g_prefs.dockpos = order[part];
            desk_relayout();
            desk_prefs_save();
          }
        break;

      case S_DOCKHIDE:
        g_prefs.dockhide = !g_prefs.dockhide;
        desk_relayout();
        desk_prefs_save();
        break;

      case S_REPEAT:
        g_prefs.repdelay = g_prefs.repdelay > 0 ? 0 : 500;
        desk_input_apply();
        desk_prefs_save();
        break;

      case S_REPDELAY:
        g_prefs.repdelay = wg_slider_value(c->x, c->w, 150, 1000, x) / 10 *
                           10;
        if (done)
          {
            desk_input_apply();
            desk_prefs_save();
          }
        break;

      case S_REPRATE:
        v = wg_slider_value(c->x, c->w, 5, 50, x);
        g_prefs.reprate = 1000 / v;
        if (done)
          {
            desk_input_apply();
            desk_prefs_save();
          }
        break;

      case S_PTRSPEED:
        g_prefs.ptrspeed = wg_slider_value(c->x, c->w, 25, 300, x) / 5 * 5;
        if (done)
          {
            desk_input_apply();
            desk_prefs_save();
          }
        break;

      case S_YEAR:
      case S_MONTH:
      case S_DAY:
      case S_HOUR:
      case S_MINUTE:
        if (part != 0)
          {
            time_step(id, part);
          }
        break;

      case S_TZ:
        if (part != 0)
          {
            v = g_prefs.tzoff + part * 30;
            g_prefs.tzoff = v < -12 * 60 ? -12 * 60 : v > 14 * 60 ?
                            14 * 60 : v;
            desk_topbar_draw();
            desk_prefs_save();
          }
        break;

      default:
        break;
    }
}

static struct ctl_s *ctl_at(int x, int y)
{
  int i;

  for (i = g_st.nctl - 1; i >= 0; i--)
    {
      struct ctl_s *c = &g_st.ctl[i];

      if (x >= c->x && x < c->x + c->w && y >= c->y && y < c->y + c->h)
        {
          return c;
        }
    }

  return NULL;
}

static struct ctl_s *ctl_find(int id)
{
  int i;

  for (i = 0; i < g_st.nctl; i++)
    {
      if (g_st.ctl[i].id == id)
        {
          return &g_st.ctl[i];
        }
    }

  return NULL;
}

static void settings_mouse(struct desk_win_s *w, int x, int y,
                           uint8_t buttons)
{
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(g_st.buttons & NX_MOUSE_LEFTBUTTON);
  bool release = !(buttons & NX_MOUSE_LEFTBUTTON) &&
                 (g_st.buttons & NX_MOUSE_LEFTBUTTON);
  struct ctl_s *c;
  int hotid = 0;
  int part = 0;

  g_st.buttons = buttons;
  y += TITLE_H;

  /* A slider follows the pointer until the button goes up */

  if (g_st.drag != 0)
    {
      c = ctl_find(g_st.drag);
      if (c != NULL)
        {
          settings_act(c, x, y, release);
        }

      if (release)
        {
          g_st.drag = 0;
        }

      settings_draw(w);
      return;
    }

  c = x >= 0 ? ctl_at(x, y) : NULL;
  if (c != NULL)
    {
      hotid = c->id;
      part  = c->kind == K_SEGMENTS ? wg_segment_at(c->x, c->w, c->n, x) :
              c->kind == K_STEPPER ? wg_stepper_at(c->x, c->w, x) : 0;
    }

  if (hotid != g_st.hot || part != g_st.hotpart)
    {
      g_st.hot     = hotid;
      g_st.hotpart = part;
      if (!press)
        {
          settings_draw(w);
        }
    }

  if (press && c != NULL && c->kind != K_FIELD)
    {
      if (c->kind == K_SLIDER)
        {
          g_st.drag = c->id;
        }

      settings_act(c, x, y, false);
      settings_draw(w);
    }
  else if (press)
    {
      settings_draw(w);
    }
}

/* Keys: into the field to try them (Mouse & Keyboard); else up and down
 * the pages
 */

static void settings_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  int keys[16];
  int nk = ui_keys(&g_st.keys, ch, n, keys, 16);
  int i;

  for (i = 0; i < nk; i++)
    {
      if (g_st.page == SETTINGS_INPUT && ui_field_key(&g_st.test, keys[i]))
        {
          continue;
        }

      if ((keys[i] & KEY_MASK) == KEY_UP && g_st.page > 0)
        {
          g_st.page--;
        }
      else if ((keys[i] & KEY_MASK) == KEY_DOWN &&
               g_st.page < SETTINGS_PAGES - 1)
        {
          g_st.page++;
        }
    }

  settings_draw(w);
}

static void settings_tick(struct desk_win_s *w)
{
  if (g_st.page == SETTINGS_DATETIME && g_st.drag == 0)
    {
      settings_draw(w);
    }
}

static void settings_close(struct desk_win_s *w)
{
  free(g_op);
  g_op = NULL;
}

static struct desk_win_s *settings_open(void)
{
  struct desk_win_s *w;
  int fd;

  g_op = malloc(sizeof(*g_op));
  if (g_op == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_SETTINGS, "Settings", SET_W, SET_H);
  if (w == NULL)
    {
      settings_close(NULL);
    }
  else
    {
      g_st.hot  = 0;
      g_st.drag = 0;
      fd = open("/dev/desktop", O_RDONLY);
      g_st.input = fd >= 0;
      if (fd >= 0)
        {
          close(fd);
        }

      settings_draw(w);
    }

  return w;
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
      ops_disc(o, kx, ly, s >= 40 ? LUT_R6 : LUT_R3, s >= 40 ? 6 : 3,
               C_WHITE, a);
      ops_disc(o, kx, ly, s >= 40 ? LUT_R6 : LUT_R3, s >= 40 ? 6 : 3,
               RGB(0x30, 0x30, 0x38), (uint8_t)(a / 5));
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_settings =
{
  .name   = "Settings",
  .icon   = icon_settings,
  .open   = settings_open,
  .draw   = settings_draw,
  .mouse  = settings_mouse,
  .key    = settings_key,
  .tick   = settings_tick,
  .close  = settings_close,
  .single = true,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Settings at a page (the dock's menu: Dock Settings...) */

void desk_settings_show(int page)
{
  struct desk_win_s *w;

  g_st.page = page >= 0 && page < SETTINGS_PAGES ? page : 0;
  desk_launch(APP_SETTINGS);
  w = desk_app_window(APP_SETTINGS);
  if (w != NULL)
    {
      settings_draw(w);
    }
}
