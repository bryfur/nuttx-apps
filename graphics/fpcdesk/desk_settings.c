/****************************************************************************
 * apps/graphics/fpcdesk/desk_settings.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings: a sidebar of pages (Appearance, Display, Dock, Mouse &
 * Keyboard, Date & Time, About this computer), built with the widget kit
 * (desk_kit.h): each page a column of cards of rows, all of them built
 * when the window opens and the one chosen shown.  Everything set is kept
 * in g_prefs and saved at once (desk_prefs_save()); settings_sync() puts
 * what g_prefs says into the widgets, and only those that change are
 * drawn again.
 *
 * A new setting goes into its page's function (a widget with an id of
 * its own in the enum below), settings_sync() and settings_event().
 ****************************************************************************/

#include <nuttx/config.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "desk.h"
#include "desk_kit.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SET_W          700             /* The window's client area */
#define SET_H          492
#define SIDE_W         188             /* The sidebar */
#define SIDE_ROW       32
#define PAD            24              /* Around a page */
#define NWG            150             /* Widgets */
#define SWATCH_W       132             /* A wallpaper's preview */
#define SWATCH_H       80
#define THEME_W        132             /* A theme's preview */
#define THEME_H        80
#define RES_MAX        4

/* The widgets' ids */

enum
{
  S_NONE = 0,
  S_SIDEBAR,
  S_PAGE,                              /* + page: its column */
  S_THEME = S_PAGE + SETTINGS_PAGES,   /* + theme */
  S_ACCENT = S_THEME + THEME_COUNT,
  S_ACCENTLBL,
  S_SHADOW,
  S_BORDER,
  S_CLOCK24,
  S_CLOCKDATE,
  S_CLOCKSECS,
  S_MODE,
  S_WALL,                              /* + wallpaper */
  S_DOCKSIZE = S_WALL + 3,
  S_DOCKSIZEV,
  S_DOCKPOS,
  S_DOCKHIDE,
  S_REPEAT,
  S_REPDELAY,
  S_REPDELAYV,
  S_REPRATE,
  S_REPRATEV,
  S_TEST,
  S_PTRSPEED,
  S_PTRSPEEDV,
  S_NOINPUT,
  S_CLOCK,
  S_DATE,
  S_YEAR,
  S_MONTH,
  S_DAY,
  S_HOUR,
  S_MINUTE,
  S_TZ,
  S_ABOUT,
  S_PIN                                /* + app */
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct settings_s
{
  struct kit_s kit;
  struct kit_widget_s wg[NWG];
  struct kit_list_s pages;             /* The sidebar */
  struct ui_field_s test;              /* Mouse & Keyboard: try the keys */
  bool input;                          /* /dev/desktop is there */
  uint32_t pinned;                     /* Pinned apps as drawn, a bit each */
  uint16_t accents[ACCENT_COUNT];
  int nmodes;
  char modes[RES_MAX][16];
  const char *modep[RES_MAX];

  /* The texts the widgets show */

  char accentcap[48];
  char docksize[8];
  char repdelay[16];
  char reprate[16];
  char ptrspeed[8];
  char clock[24];
  char date[40];
  char year[8];
  char month[8];
  char day[8];
  char hour[8];
  char minute[8];
  char tz[16];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct settings_s *g_st;        /* While the window is open */
static int g_page;

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

static const char * const g_shadows[4] =
{
  "Off", "Light", "Normal", "Strong"
};

static const char * const g_where[3] =
{
  "Left", "Bottom", "Right"
};

static const uint8_t g_whereorder[3] =
{
  DOCK_LEFT, DOCK_BOTTOM, DOCK_RIGHT
};

static const char * const g_months[12] =
{
  "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
  "Nov", "Dec"
};

#ifdef CONFIG_FPCDESK_WALLPAPER
static const char * const g_wallnames[3] =
{
  "Aurora", "Sunset", "Graphite"
};
#endif

/****************************************************************************
 * Private Functions: drawing what is the application's own
 ****************************************************************************/

/* A small picture of a theme: a desktop with a window on it */

static void theme_preview(struct desk_ops_s *o, int t, int x, int y,
                          bool sel, bool hot)
{
  const struct desk_theme_s *th = desk_theme_get(t);
  uint16_t accent = desk_accent_colour(g_prefs.accent);
  int wx = x + 16;
  int wy = y + 14;
  int ww = THEME_W - 32;
  int wh = THEME_H - 24;

  ops_fill(o, x - 4, y - 4, THEME_W + 8, THEME_H + 32, g_theme.window_bg);
  if (sel || hot)
    {
      ops_rrect(o, x - 4, y - 4, THEME_W + 8, THEME_H + 8, LUT_R12, 12,
                sel ? C_ACCENT : g_theme.control_hover, 255);
    }

  ops_rrect(o, x, y, THEME_W, THEME_H, LUT_R6, 6,
            t == THEME_DARK ? RGB(0x2c, 0x30, 0x48) : RGB(0x9c, 0xa6, 0xb6),
            255);
  ops_rrect(o, x, y, THEME_W, 8, LUT_R6, 4, th->panel_tint,
            th->panel_alpha);
  ops_rrect(o, wx, wy, ww, wh, LUT_R6, 6, th->title_active, 255);
  ops_rrect(o, wx, wy + 12, ww, wh - 12, LUT_R6, 6, th->window_bg, 255);
  ops_fill(o, wx, wy + 12, ww, 6, th->window_bg);
  ops_disc(o, wx + 7, wy + 6, LUT_R3, 2, C_CLOSE, 255);
  ops_disc(o, wx + 13, wy + 6, LUT_R3, 2, C_MINIMIZE, 255);
  ops_disc(o, wx + 19, wy + 6, LUT_R3, 2, C_MAXIMIZE, 255);
  ops_fill(o, wx + 8, wy + 20, ww - 30, 3, th->text);
  ops_fill(o, wx + 8, wy + 27, ww - 44, 3, th->text_dim);
  ops_fill(o, wx + 8, wy + 34, ww - 36, 3, th->text_dim);
  ops_rrect(o, wx + ww - 30, wy + wh - 14, 24, 9, LUT_R3, 3, accent, 255);
  ops_text_centre(o, sel ? KIT_FONT_BOLD : KIT_FONT, x + THEME_W / 2,
                  y + THEME_H + 8, t == THEME_DARK ? "Dark" : "Light",
                  sel ? g_theme.text : g_theme.text_dim, 255);
}

/* The kit's draw function: previews, the apps to pin, About */

static void settings_paint(struct kit_s *k, struct kit_widget_s *wg, int x0,
                           int y0, int x1, int y1)
{
  int look = kit_look(k, wg);
  bool hot = (look & KIT_LOOK_MASK) != KIT_LOOK_NORMAL;
  struct desk_ops_s *o;
  int id = wg->id;

  if (id >= S_THEME && id < S_THEME + THEME_COUNT)
    {
      o = kit_ops(k, 40);
      theme_preview(o, id - S_THEME, wg->x + 4, wg->y + 4, wg->value != 0,
                    hot);
    }
#ifdef CONFIG_FPCDESK_WALLPAPER
  else if (id >= S_WALL && id < S_WALL + 3)
    {
      int i = id - S_WALL;
      int x = wg->x + 4;
      int y = wg->y + 4;

      o = kit_ops(k, 80);
      ops_fill(o, wg->x, wg->y, wg->w, wg->h, g_theme.window_bg);
      if (wg->value || hot)
        {
          ops_rrect(o, x - 4, y - 4, SWATCH_W + 8, SWATCH_H + 8, LUT_R12, 12,
                    wg->value ? C_ACCENT : g_theme.control_hover, 255);
        }

      desk_wallpaper_preview(o, i, x, y, SWATCH_W, SWATCH_H);
      o = kit_ops(k, 20);
      ops_text_centre(o, wg->value ? KIT_FONT_BOLD : KIT_FONT,
                      x + SWATCH_W / 2, y + SWATCH_H + 10, g_wallnames[i],
                      wg->value ? g_theme.text : g_theme.text_dim, 255);
    }
#endif
  else if (id >= S_PIN && id < S_PIN + APP_COUNT)
    {
      int app = id - S_PIN;

      o = kit_ops(k, 80 + strlen(g_apps[app]->name));
      ops_fill(o, wg->x, wg->y, wg->w, wg->h, g_theme.window_bg);
      if (hot)
        {
          ops_rrect(o, wg->x, wg->y, wg->w, wg->h, LUT_R6, 6, C_HILITE,
                    THEMED(18, 12));
        }

      if (look & KIT_LOOK_FOCUS)
        {
          kit_paint_ring(o, wg->x + 8, wg->y + 7, KIT_CHECK, KIT_CHECK,
                         KIT_R_CONTROL, g_theme.window_bg);
        }

      kit_paint_check(o, wg->x + 8, wg->y + 7, desk_dock_pinned(app),
                      KIT_LOOK_NORMAL);
      if (g_apps[app]->icon != NULL)
        {
          g_apps[app]->icon(o, wg->x + 36, wg->y + 2, 28, 255);
        }

      o = kit_ops(k, strlen(g_apps[app]->name) + 2);
      ops_text(o, KIT_FONT, wg->x + 74,
               wg->y + (wg->h - text_height(KIT_FONT)) / 2,
               g_apps[app]->name, g_theme.text, 255);
    }
  else if (id == S_ABOUT)
    {
      o = kit_ops(k, 1);
      ops_fill(o, wg->x, wg->y, wg->w, wg->h, g_theme.window_bg);
      ops_draw((NXWINDOW)k->w->hwnd, o);
      desk_about_draw((NXWINDOW)k->w->hwnd, wg->x, wg->y, wg->w);
    }
}

/* The sidebar's rows: a small coloured tile */

static const char *page_text(struct kit_s *k, struct kit_widget_s *wg,
                             int row, int col, char *buf, int size)
{
  return g_pages[row];
}

static void page_icon(struct kit_s *k, struct kit_widget_s *wg,
                      struct desk_ops_s *o, int row, int x, int y,
                      uint16_t fg, uint16_t bg)
{
  ops_rrect(o, x - 2, y - 2, 20, 20, LUT_R6, 6, g_pagetile[row], 255);
  ops_blend(o, x + 2, y - 2, 12, 1, C_WHITE, 70);
}

/****************************************************************************
 * Private Functions: the pages
 ****************************************************************************/

static struct kit_widget_s *page_begin(struct kit_s *k, int page,
                                       const char *heading)
{
  struct kit_widget_s *wg = kit_col(k, KIT_BOX_NONE);

  wg->id   = S_PAGE + page;
  wg->flags |= KIT_F_SCROLL;           /* Taller than the window: it
                                        * scrolls (800 x 600) */
  wg->padx = PAD;
  wg->pady = 18;
  wg->gap  = 12;
  kit_grow(k, 1);
  if (page != g_page)
    {
      wg->flags |= KIT_F_HIDDEN;
    }

  if (heading != NULL)
    {
      kit_label(k, 0, heading, KIT_TEXT_HEADING);
    }

  return wg;
}

/* A row of a card: a label, a value's text, a slider */

static void row_slider(struct kit_s *k, int id, const char *label,
                       char *value, int min, int max)
{
  kit_card_row(k, 0, label);
    kit_label(k, id + 1, value, KIT_TEXT_DIM);
    kit_slider(k, id, min, min, max);
    kit_size(k, 200, 0);
  kit_end(k);
}

static void row_switch(struct kit_s *k, int id, const char *label)
{
  kit_card_row(k, 0, label);
    kit_switch(k, id, false);
  kit_end(k);
}

static void page_appearance(struct settings_s *st)
{
  struct kit_s *k = &st->kit;
  struct kit_widget_s *wg;
  int t;

  page_begin(k, SETTINGS_APPEARANCE, "Appearance");
    wg = kit_row(k, KIT_BOX_NONE);
    wg->gap = 16;
      for (t = 0; t < THEME_COUNT; t++)
        {
          kit_button(k, S_THEME + t, NULL, KIT_ICON_NONE, KIT_BTN_CUSTOM);
          kit_size(k, THEME_W + 8, THEME_H + 32);
        }
    kit_end(k);

    kit_card(k);
      kit_card_row(k, S_ACCENTLBL, st->accentcap);
        kit_swatches(k, S_ACCENT, st->accents, ACCENT_COUNT, 0);
      kit_end(k);
      kit_card_row(k, 0, "Window shadows");
        kit_tabs(k, S_SHADOW, g_shadows, 4, 0, KIT_TABS_SEGMENTS);
        kit_size(k, 260, 28);
      kit_end(k);
      row_switch(k, S_BORDER, "Window edges\nA hairline around each window");
    kit_end(k);

    kit_card(k);
      row_switch(k, S_CLOCK24, "24-hour clock");
      row_switch(k, S_CLOCKDATE, "Show the date\nIn the top bar");
      row_switch(k, S_CLOCKSECS, "Show seconds");
    kit_end(k);
  kit_end(k);
}

static void page_display(struct settings_s *st)
{
  struct kit_s *k = &st->kit;

  page_begin(k, SETTINGS_DISPLAY, "Display");
    kit_label(k, 0, "Resolution", KIT_TEXT_BOLD);
    if (st->nmodes > 0)
      {
        kit_tabs(k, S_MODE, st->modep, st->nmodes, 0, KIT_TABS_SEGMENTS);
        kit_size(k, 0, 30);
      }
    else
      {
        kit_label(k, 0, "This display has one mode", KIT_TEXT_DIM);
      }

    kit_spacer(k, 0);
    kit_label(k, 0, "Wallpaper", KIT_TEXT_BOLD);
#ifdef CONFIG_FPCDESK_WALLPAPER
    {
      struct kit_widget_s *wg;
      int i;

      kit_label(k, 0, "Drawn by the GPU: a gradient and soft glows",
                KIT_TEXT_DIM);
      wg = kit_row(k, KIT_BOX_NONE);
      wg->gap = 10;
        for (i = 0; i < 3; i++)
          {
            kit_button(k, S_WALL + i, NULL, KIT_ICON_NONE, KIT_BTN_CUSTOM);
            kit_size(k, SWATCH_W + 8, SWATCH_H + 34);
          }
      kit_end(k);
    }
#else
    kit_label(k, 0, "No wallpaper in this build (CONFIG_FPCDESK_WALLPAPER).",
              KIT_TEXT_DIM);
#endif
  kit_end(k);
}

static void page_dock(struct settings_s *st)
{
  struct kit_s *k = &st->kit;
  struct kit_widget_s *wg;
  int col;
  int i;

  page_begin(k, SETTINGS_DOCK, "Dock");
    kit_card(k);
      row_slider(k, S_DOCKSIZE, "Size\nOf its icons", st->docksize,
                 DOCK_ICON_MIN, DOCK_ICON_MAX);
      kit_card_row(k, 0, "Position on screen");
        kit_tabs(k, S_DOCKPOS, g_where, 3, 1, KIT_TABS_SEGMENTS);
        kit_size(k, 240, 28);
      kit_end(k);
      row_switch(k, S_DOCKHIDE, "Hide automatically\nIt comes back when the "
                 "pointer touches the edge");
    kit_end(k);

    kit_label(k, 0, "In the Dock\nOr: the right button on an icon in the "
              "Dock or the launcher", KIT_TEXT_BOLD);

    /* The applications, in two columns */

    wg = kit_row(k, KIT_BOX_NONE);
    wg->gap = 8;
      for (col = 0; col < 2; col++)
        {
          kit_col(k, KIT_BOX_NONE)->gap = 2;
          kit_grow(k, 1);
            for (i = 1 + col; i < APP_COUNT; i += 2)
              {
                kit_button(k, S_PIN + i, NULL, KIT_ICON_NONE,
                           KIT_BTN_CUSTOM)->align = KIT_A_FILL;
                kit_size(k, 0, 32);
              }
          kit_end(k);
        }
    kit_end(k);
  kit_end(k);
}

static void page_input(struct settings_s *st)
{
  struct kit_s *k = &st->kit;

  page_begin(k, SETTINGS_INPUT, "Mouse & Keyboard");
    kit_card(k);
      row_switch(k, S_REPEAT, "Key repeat\nA key held down repeats");
      row_slider(k, S_REPDELAY, "Delay until repeat", st->repdelay, 150,
                 1000);
      row_slider(k, S_REPRATE, "Repeat rate", st->reprate, 5, 50);
      kit_card_row(k, 0, "Try it");
        kit_field(k, S_TEST, &st->test, "Type here");
        kit_size(k, 240, 0);
      kit_end(k);
    kit_end(k);

    kit_card(k);
      row_slider(k, S_PTRSPEED, "Pointer speed\nHow far it goes for the "
                 "mouse", st->ptrspeed, 25, 300);
    kit_end(k);

    kit_label(k, S_NOINPUT, "No desktop session (/dev/desktop): these have "
              "no effect.", KIT_TEXT_SMALL | KIT_TEXT_DIM);
  kit_end(k);
}

static void page_datetime(struct settings_s *st)
{
  struct kit_s *k = &st->kit;

  page_begin(k, SETTINGS_DATETIME, "Date & Time");
    kit_label(k, S_CLOCK, st->clock, KIT_TEXT_DISPLAY | KIT_TEXT_CENTRE);
    kit_label(k, S_DATE, st->date, KIT_TEXT_DIM | KIT_TEXT_CENTRE);
    kit_card(k);
      kit_card_row(k, 0, "Date");
        kit_stepper(k, S_YEAR, st->year);
        kit_stepper(k, S_MONTH, st->month);
        kit_stepper(k, S_DAY, st->day);
      kit_end(k);
      kit_card_row(k, 0, "Time\nSeconds start again at 0");
        kit_stepper(k, S_HOUR, st->hour);
        kit_stepper(k, S_MINUTE, st->minute);
      kit_end(k);
      kit_card_row(k, 0, "Time zone\nOffset from UTC");
        kit_stepper(k, S_TZ, st->tz);
        kit_size(k, 150, 0);
      kit_end(k);
    kit_end(k);

    kit_label(k, 0, "There is no battery-backed clock: the time starts "
              "again at each boot.", KIT_TEXT_SMALL | KIT_TEXT_DIM);
  kit_end(k);
}

static void page_about(struct settings_s *st)
{
  struct kit_s *k = &st->kit;

  page_begin(k, SETTINGS_ABOUT, NULL)->pady = 0;
    kit_custom(k, S_ABOUT)->flags |= KIT_F_NOFOCUS;
    kit_grow(k, 1);
  kit_end(k);
}

/****************************************************************************
 * Private Functions: keeping the widgets up to date
 ****************************************************************************/

/* A text: changed only if it is different (then drawn again) */

static void set_text(struct settings_s *st, int id, char *buf, size_t size,
                     const char *fmt, ...)
{
  char tmp[64];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (strcmp(tmp, buf) != 0)
    {
      strlcpy(buf, tmp, size);
      kit_set_text(&st->kit, id, buf);
    }
}

static void settings_sync(struct settings_s *st)
{
  struct kit_s *k = &st->kit;
  struct nxgl_size_s size;
  struct tm tm;
  int off = g_prefs.tzoff < 0 ? -g_prefs.tzoff : g_prefs.tzoff;
  uint32_t pinned = 0;
  int i;

  /* Appearance */

  for (i = 0; i < THEME_COUNT; i++)
    {
      kit_set_value(k, S_THEME + i, g_prefs.theme == i);
    }

  for (i = 0; i < ACCENT_COUNT; i++)
    {
      if (st->accents[i] != desk_accent_colour(i))
        {
          st->accents[i] = desk_accent_colour(i);
          kit_invalidate(k, S_ACCENT);
        }
    }

  kit_set_value(k, S_ACCENT, g_prefs.accent);
  set_text(st, S_ACCENTLBL, st->accentcap, sizeof(st->accentcap),
           "Accent colour\n%s", desk_accent_name(g_prefs.accent));
  kit_set_value(k, S_SHADOW, g_prefs.shadow);
  kit_set_value(k, S_BORDER, g_prefs.border);
  kit_set_value(k, S_CLOCK24, g_prefs.clock24);
  kit_set_value(k, S_CLOCKDATE, g_prefs.clockdate);
  kit_set_value(k, S_CLOCKSECS, g_prefs.clocksecs);

  /* Display */

  for (i = 0; i < st->nmodes && nx_getdisplaymode(g_desk.hnx, i, &size) >= 0;
       i++)
    {
      snprintf(st->modes[i], sizeof(st->modes[i]), "%d x %d", size.w,
               size.h);
    }

  kit_set_value(k, S_MODE, nx_getdisplaymode(g_desk.hnx, -1, NULL));
  for (i = 0; i < 3; i++)
    {
      kit_set_value(k, S_WALL + i, g_desk.wallstyle == i);
    }

  /* Dock */

  kit_set_value(k, S_DOCKSIZE, g_prefs.dockicon);
  set_text(st, S_DOCKSIZEV, st->docksize, sizeof(st->docksize), "%d",
           g_prefs.dockicon);
  for (i = 0; i < 3; i++)
    {
      if (g_whereorder[i] == g_prefs.dockpos)
        {
          kit_set_value(k, S_DOCKPOS, i);
        }
    }

  kit_set_value(k, S_DOCKHIDE, g_prefs.dockhide);
  for (i = 1; i < APP_COUNT && i < 32; i++)
    {
      pinned |= desk_dock_pinned(i) ? 1u << i : 0;
      if (((st->pinned ^ pinned) >> i) & 1)
        {
          kit_invalidate(k, S_PIN + i);
        }
    }

  st->pinned = pinned;

  /* Mouse & Keyboard */

  kit_set_value(k, S_REPEAT, g_prefs.repdelay > 0);
  kit_set_value(k, S_REPDELAY, g_prefs.repdelay > 0 ? g_prefs.repdelay :
                500);
  set_text(st, S_REPDELAYV, st->repdelay, sizeof(st->repdelay), "%d ms",
           g_prefs.repdelay > 0 ? g_prefs.repdelay : 500);
  kit_enable(k, S_REPDELAY, g_prefs.repdelay > 0);
  kit_enable(k, S_REPRATE, g_prefs.repdelay > 0);
  kit_set_value(k, S_REPRATE, 1000 / g_prefs.reprate);
  set_text(st, S_REPRATEV, st->reprate, sizeof(st->reprate), "%d a second",
           1000 / g_prefs.reprate);
  kit_set_value(k, S_PTRSPEED, g_prefs.ptrspeed);
  set_text(st, S_PTRSPEEDV, st->ptrspeed, sizeof(st->ptrspeed), "%d%%",
           g_prefs.ptrspeed);
  kit_show(k, S_NOINPUT, !st->input);

  /* Date & Time */

  desk_localtime(&tm);
  {
    char buf[40];

    strftime(buf, sizeof(buf), g_prefs.clock24 ? "%H:%M:%S" :
             "%l:%M:%S %p", &tm);
    set_text(st, S_CLOCK, st->clock, sizeof(st->clock), "%s", buf);
    strftime(buf, sizeof(buf), "%A, %e %B %Y", &tm);
    set_text(st, S_DATE, st->date, sizeof(st->date), "%s", buf);
  }

  set_text(st, S_YEAR, st->year, sizeof(st->year), "%d",
           tm.tm_year + 1900);
  set_text(st, S_MONTH, st->month, sizeof(st->month), "%s",
           g_months[tm.tm_mon % 12]);
  set_text(st, S_DAY, st->day, sizeof(st->day), "%d", tm.tm_mday);
  set_text(st, S_HOUR, st->hour, sizeof(st->hour), "%02d", tm.tm_hour);
  set_text(st, S_MINUTE, st->minute, sizeof(st->minute), "%02d",
           tm.tm_min);
  set_text(st, S_TZ, st->tz, sizeof(st->tz), "UTC%c%02d:%02d",
           g_prefs.tzoff < 0 ? '-' : '+', off / 60, off % 60);
}

static void show_page(struct settings_s *st, int page)
{
  int i;

  g_page = page;
  for (i = 0; i < SETTINGS_PAGES; i++)
    {
      kit_show(&st->kit, S_PAGE + i, i == page);
    }

  if (st->pages.sel != page)
    {
      kit_list_select(&st->kit, S_SIDEBAR, page);
    }

  if (page == SETTINGS_ABOUT)
    {
      kit_invalidate(&st->kit, S_ABOUT);         /* The display's size */
    }
}

/****************************************************************************
 * Private Functions: acting
 ****************************************************************************/

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

static void settings_event(struct kit_s *k, struct kit_widget_s *wg, int ev)
{
  struct settings_s *st = k->priv;
  int id = wg != NULL ? wg->id : 0;
  int v = wg != NULL ? wg->value : 0;

  if (ev == KIT_EV_KEY)
    {
      /* Up and down go through the pages (but in the field) */

      int key = k->key & KEY_MASK;

      if (key == KEY_UP && g_page > 0)
        {
          show_page(st, g_page - 1);
          k->key = 0;
        }
      else if (key == KEY_DOWN && g_page < SETTINGS_PAGES - 1)
        {
          show_page(st, g_page + 1);
          k->key = 0;
        }

      return;
    }

  if (ev == KIT_EV_CLICK)
    {
      if (id >= S_THEME && id < S_THEME + THEME_COUNT)
        {
          g_prefs.theme = id - S_THEME;
          desk_theme_set(g_prefs.theme, g_prefs.accent);
          settings_sync(st);
          desk_theme_changed();                /* Settings too */
          desk_prefs_save();
        }
#ifdef CONFIG_FPCDESK_WALLPAPER
      else if (id >= S_WALL && id < S_WALL + 3 &&
               g_desk.wallstyle != id - S_WALL)
        {
          g_desk.wallstyle = id - S_WALL;
          desk_wallpaper_draw();
          desk_prefs_save();
        }
#endif
      else if (id >= S_PIN && id < S_PIN + APP_COUNT)
        {
          desk_dock_pin(id - S_PIN, !desk_dock_pinned(id - S_PIN));
        }

      settings_sync(st);
      return;
    }

  if (ev != KIT_EV_CHANGE && ev != KIT_EV_ACTIVATE)
    {
      return;
    }

  switch (id)
    {
      case S_SIDEBAR:
        show_page(st, st->pages.sel >= 0 ? st->pages.sel : 0);
        break;

      case S_ACCENT:
        g_prefs.accent = v;
        desk_theme_set(g_prefs.theme, g_prefs.accent);
        settings_sync(st);
        desk_theme_changed();
        desk_prefs_save();
        break;

      case S_SHADOW:
        g_prefs.shadow = v;
        desk_theme_changed();
        desk_prefs_save();
        break;

      case S_BORDER:
        g_prefs.border = v;
        desk_theme_changed();
        desk_prefs_save();
        break;

      case S_CLOCK24:
      case S_CLOCKDATE:
      case S_CLOCKSECS:
        *(id == S_CLOCK24 ? &g_prefs.clock24 : id == S_CLOCKDATE ?
          &g_prefs.clockdate : &g_prefs.clocksecs) = v;
        desk_topbar_draw();
        desk_prefs_save();
        break;

      /* A resolution: the desktop lays itself out again when the server
       * says the mode has changed (EV_MODE), Settings with it
       */

      case S_MODE:
        if (v != nx_getdisplaymode(g_desk.hnx, -1, NULL) &&
            nx_setdisplaymode(g_desk.hnx, v) >= 0)
          {
            desk_prefs_save();
          }
        break;

      case S_DOCKSIZE:
        if (v != g_prefs.dockicon)
          {
            g_prefs.dockicon = v;
            desk_dock_draw();                  /* The dock follows */
          }

        if (ev == KIT_EV_ACTIVATE)
          {
            desk_relayout();                   /* And the windows */
            desk_prefs_save();
          }
        break;

      case S_DOCKPOS:
        g_prefs.dockpos = g_whereorder[v];
        desk_relayout();
        desk_prefs_save();
        break;

      case S_DOCKHIDE:
        g_prefs.dockhide = v;
        desk_relayout();
        desk_prefs_save();
        break;

      case S_REPEAT:
        g_prefs.repdelay = v ? 500 : 0;
        desk_input_apply();
        desk_prefs_save();
        break;

      case S_REPDELAY:
      case S_REPRATE:
      case S_PTRSPEED:
        if (id == S_REPDELAY)
          {
            g_prefs.repdelay = v / 10 * 10;
          }
        else if (id == S_REPRATE)
          {
            g_prefs.reprate = 1000 / v;
          }
        else
          {
            g_prefs.ptrspeed = v / 5 * 5;
          }

        if (ev == KIT_EV_ACTIVATE)
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
        time_step(id, k->arg);
        break;

      case S_TZ:
        v = g_prefs.tzoff + k->arg * 30;
        g_prefs.tzoff = v < -12 * 60 ? -12 * 60 : v > 14 * 60 ? 14 * 60 : v;
        desk_topbar_draw();
        desk_prefs_save();
        break;

      default:
        break;
    }

  settings_sync(st);
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static void settings_build(struct settings_s *st, struct desk_win_s *w)
{
  struct kit_s *k = &st->kit;
  struct kit_widget_s *wg;
  struct nxgl_size_s size;

  kit_init(k, w, st->wg, NWG, settings_event);
  k->priv = st;
  k->draw = settings_paint;

  for (st->nmodes = 0; st->nmodes < RES_MAX &&
       nx_getdisplaymode(g_desk.hnx, st->nmodes, &size) >= 0; st->nmodes++)
    {
      st->modep[st->nmodes] = st->modes[st->nmodes];
    }

  st->pages.n       = SETTINGS_PAGES;
  st->pages.sel     = g_page;
  st->pages.rowh    = SIDE_ROW;
  st->pages.sortcol = -1;
  st->pages.text    = page_text;
  st->pages.icon    = page_icon;

  kit_row(k, KIT_BOX_WINDOW);
    wg = kit_col(k, KIT_BOX_SIDEBAR);
    wg->padx = 10;
    wg->pady = 12;
    kit_size(k, SIDE_W, 0);
      kit_list(k, S_SIDEBAR, &st->pages, KIT_LIST_SIDEBAR);
      kit_grow(k, 1);
    kit_end(k);

    page_appearance(st);
    page_display(st);
    page_dock(st);
    page_input(st);
    page_datetime(st);
    page_about(st);
  kit_end(k);
}

static void settings_draw(struct desk_win_s *w)
{
  struct settings_s *st = w->priv;

  if (st != NULL)
    {
      settings_sync(st);
      kit_flush(&st->kit);
    }
}

static void settings_expose(struct desk_win_s *w, int x, int y, int cw,
                            int ch)
{
  struct settings_s *st = w->priv;

  if (st != NULL)
    {
      settings_sync(st);
      kit_expose(&st->kit, x, y, cw, ch);
    }
}

static void settings_resize(struct desk_win_s *w)
{
  struct settings_s *st = w->priv;

  if (st != NULL)
    {
      kit_resize(&st->kit);
    }
}

static void settings_mouse(struct desk_win_s *w, int x, int y,
                           uint8_t buttons)
{
  struct settings_s *st = w->priv;

  if (st != NULL)
    {
      kit_mouse(&st->kit, x, y, buttons);
    }
}

static void settings_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct settings_s *st = w->priv;

  if (st != NULL)
    {
      kit_key(&st->kit, ch, n);
    }
}

static void settings_tick(struct desk_win_s *w)
{
  struct settings_s *st = w->priv;

  if (st != NULL)
    {
      if (g_page == SETTINGS_DATETIME)
        {
          settings_sync(st);                   /* The clock's labels */
        }

      kit_tick(&st->kit);
    }
}

static void settings_focus(struct desk_win_s *w)
{
  struct settings_s *st = w->priv;

  if (st != NULL)
    {
      kit_activate(&st->kit);
    }
}

static void settings_close(struct desk_win_s *w)
{
  g_st = NULL;                                 /* w->priv goes after */
}

static struct desk_win_s *settings_open(void)
{
  struct settings_s *st;
  struct desk_win_s *w;
  int fd;

  st = calloc(1, sizeof(*st));
  if (st == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_SETTINGS, "Settings", SET_W, SET_H);
  if (w == NULL)
    {
      free(st);
      return NULL;
    }

  w->priv = st;
  g_st    = st;
  fd = open("/dev/desktop", O_RDONLY);
  st->input = fd >= 0;
  if (fd >= 0)
    {
      close(fd);
    }

  settings_build(st, w);
  settings_sync(st);
  kit_focus(&st->kit, S_SIDEBAR);
  kit_layout(&st->kit);
  kit_flush(&st->kit);
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
  .expose = settings_expose,
  .resize = settings_resize,
  .mouse  = settings_mouse,
  .key    = settings_key,
  .tick   = settings_tick,
  .focus  = settings_focus,
  .close  = settings_close,
  .single = true,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Settings at a page (the dock's menu: Dock Settings...) */

void desk_settings_show(int page)
{
  g_page = page >= 0 && page < SETTINGS_PAGES ? page : 0;
  desk_launch(APP_SETTINGS);
  if (g_st != NULL)
    {
      show_page(g_st, g_page);
      kit_flush(&g_st->kit);
    }
}
