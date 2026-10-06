/****************************************************************************
 * apps/graphics/fpcdesk/desk_settings.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings: a sidebar of pages (Appearance, Display, Dock, Mouse &
 * Keyboard, Date & Time, Sound, Wi-Fi, Bluetooth, About this computer),
 * built with the widget kit
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
#ifdef CONFIG_FPCDESK_SOUND
#  include "desk_sound.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SET_W          700             /* The window's client area */
#define SET_H          536
#define SIDE_W         188             /* The sidebar */
#define SIDE_ROW       32
#define PAD            24              /* Around a page */
#ifdef CONFIG_NETUTILS_NETCOP
#  define NWG          210             /* Widgets */
#else
#  define NWG          150
#endif
#define SWATCH_W       98              /* A wallpaper's preview: four */
#define SWATCH_H       64              /* across the page */
#define NWALLS         4               /* The drawn ones, then the image */
#define THEME_W        132             /* A theme's preview */
#define THEME_H        80
#define RES_MAX        4
#define APPLEVELS      3               /* Sound: applications' levels */

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
  S_FX3D,
  S_FXEACH,                            /* + effect (FX_* bit number) */
  S_CLOCK24 = S_FXEACH + FX_COUNT,
  S_CLOCKDATE,
  S_CLOCKSECS,
  S_MODE,
  S_WALL,                              /* + wallpaper */
  S_DOCKSIZE = S_WALL + NWALLS,
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
  S_VOLUME,                            /* Sound */
  S_VOLUMEV,
  S_MUTE,
  S_ALERTS,
  S_ALERTSV,
  S_ALERTTEST,
  S_EFFECTS,
  S_OUTPUT,
  S_SNDCARD,
  S_ALERTCARD,
  S_APPSLBL,
  S_NOAPPS,
  S_APPCARD,
  S_APPROW,                            /* + application */
  S_APPNAME = S_APPROW + APPLEVELS,    /* + application */
  S_APPLEVEL = S_APPNAME + APPLEVELS,  /* + 2 x application: the slider,
                                        * its value */
  S_PIN = S_APPLEVEL + 2 * APPLEVELS,  /* + app */
  S_NET = S_PIN + APP_COUNT,           /* Wi-Fi and Bluetooth */
  S_W_NOHW = S_NET,
  S_W_CARD,
  S_W_ONLBL,
  S_W_ON,
  S_W_NETROW,
  S_W_NETLBL,
  S_W_DISC,
  S_W_NETHEAD,
  S_W_SCAN,
  S_W_LIST,
  S_W_JOINCARD,
  S_W_JOINLBL,
  S_W_PASS,
  S_W_JOIN,
  S_W_MSG,
  S_W_SAVEDHEAD,
  S_W_SAVED,
  S_W_FORGET,
  S_B_NOHW,
  S_B_BODY,
  S_B_PAIRED,
  S_B_DISC,
  S_B_FORGET,
  S_B_SCAN,
  S_B_FOUND,
  S_B_PAIR,
  S_B_MSG,
  S_NET_END
};

#define NAPS           24              /* Wi-Fi: networks listed */
#define NSAVED         16
#define NFOUND         16              /* Bluetooth: devices found */

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

#ifdef CONFIG_NETUTILS_NETCOP
  /* Wi-Fi: the networks in range, the saved ones, the one being joined */

  struct kit_list_s aplist;
  struct kit_list_s savedlist;
  struct nc_wifi_ap aps[NAPS];
  struct nc_wifi_saved saved[NSAVED];
  int naps;
  int nsaved;
  bool scanning;
  bool scanned;                        /* Since the hardware came */
  bool joining;
  struct ui_field_s pass;
  char wonlbl[64];
  char wnetlbl[96];
  char joinlbl[64];
  char wmsg[80];

  /* Bluetooth: the bonded devices (g_net's), those found nearby */

  struct kit_list_s pairedlist;
  struct kit_list_s foundlist;
  struct nc_ble_adv found[NFOUND];
  int nfound;
  bool bscanning;
  bool pairing;
  uint32_t blegen;                     /* g_net's devices as listed */
  char bmsg[96];
#endif

#ifdef CONFIG_FPCDESK_SOUND
  /* Sound: the applications whose levels the page shows (the mixer's
   * levels but the system sounds')
   */

  struct fpcmix_levels_s levels;
  int napps;
  uint8_t app[APPLEVELS];
  char output[80];
  char volume[8];
  char alerts[8];
  char appname[APPLEVELS][40];
  char applevel[APPLEVELS][8];
#endif
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct settings_s *g_st;        /* While the window is open */
static int g_page;

static const char * const g_pages[SETTINGS_PAGES] =
{
  "Appearance", "Display", "Dock", "Mouse & Keyboard", "Date & Time",
#ifdef CONFIG_FPCDESK_SOUND
  "Sound",
#endif
#ifdef CONFIG_NETUTILS_NETCOP
  "Wi-Fi", "Bluetooth",
#endif
  "About this computer"
};

static const uint16_t g_pagetile[SETTINGS_PAGES] =
{
  RGB(0x0a, 0x84, 0xff), RGB(0x5e, 0x5c, 0xe6), RGB(0x8e, 0x8e, 0x93),
  RGB(0x34, 0xc7, 0x59), RGB(0xff, 0x95, 0x00),
#ifdef CONFIG_FPCDESK_SOUND
  RGB(0xff, 0x2d, 0x55),
#endif
#ifdef CONFIG_NETUTILS_NETCOP
  RGB(0x32, 0xad, 0xe6), RGB(0x00, 0x64, 0xd2),
#endif
  RGB(0x64, 0x7a, 0x96)
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
static const char * const g_wallnames[NWALLS] =
{
  "Aurora", "Sunset", "Graphite", "Image"
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
  else if (id >= S_WALL && id < S_WALL + NWALLS)
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

/* The 3D effects' row: the master switch, each effect a check box below
 * its label (disabled while the switch is off)
 */

static void row_effects(struct kit_s *k)
{
  static const char *const name[FX_COUNT] =
  {
    "Open", "Minimise", "Drag", "Maximise", "Alt-Tab"
  };

  static const char *const tip[FX_COUNT] =
  {
    "Windows open and close with a tilt",
    "Minimised windows go into the Dock with a genie",
    "A window swings as it is dragged",
    "Maximising and restoring zoom",
    "Alt-Tab shows a cover flow, else a row of icons"
  };

  struct kit_widget_s *wg;
  int i;

  wg = kit_row(k, KIT_BOX_NONE);
  wg->padx = 14;
  wg->pady = 9;
  wg->gap  = 12;
    wg = kit_col(k, KIT_BOX_NONE);
    wg->gap = 5;
    kit_grow(k, 1);
      kit_label(k, 0, "3D effects", KIT_TEXT_BODY);
      wg = kit_row(k, KIT_BOX_NONE);
      wg->gap = 10;
        for (i = 0; i < FX_COUNT; i++)
          {
            kit_check(k, S_FXEACH + i, name[i], false);
            kit_tip(k, tip[i]);
          }
      kit_end(k);
    kit_end(k);
    kit_switch(k, S_FX3D, false);
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
      row_effects(k);
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

      kit_label(k, 0, "Drawn by the GPU, or an image from your home folder",
                KIT_TEXT_DIM);
      wg = kit_row(k, KIT_BOX_NONE);
      wg->gap = 10;
        for (i = 0; i < NWALLS; i++)
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

#ifdef CONFIG_FPCDESK_SOUND
/* Sound: where it goes, the volume and mute; the alert volume, a test,
 * interface sounds; the levels of the applications that have played
 */

static void page_sound(struct settings_s *st)
{
  struct kit_s *k = &st->kit;
  int i;

  page_begin(k, SETTINGS_SOUND, "Sound");
    kit_label(k, S_OUTPUT, st->output, KIT_TEXT_SMALL | KIT_TEXT_DIM);
    kit_card(k)->id = S_SNDCARD;
      row_slider(k, S_VOLUME, "Volume", st->volume, 0, FPCMIX_VOLUME_MAX);
      row_switch(k, S_MUTE, "Mute");
    kit_end(k);

    kit_card(k)->id = S_ALERTCARD;
      row_slider(k, S_ALERTS, "Alert volume\nThe system's sounds",
                 st->alerts, 0, FPCMIX_VOLUME_MAX);
      kit_card_row(k, 0, "Alert sound");
        kit_button(k, S_ALERTTEST, "Play", KIT_ICON_NONE, KIT_BTN_NORMAL);
        kit_size(k, 96, 0);
      kit_end(k);
      row_switch(k, S_EFFECTS, "Interface sounds\nWindows opening, closing");
    kit_end(k);

    kit_label(k, S_APPSLBL, "Applications", KIT_TEXT_BOLD);
    kit_label(k, S_NOAPPS, "None has played yet", KIT_TEXT_DIM);
    kit_card(k)->id = S_APPCARD;
      for (i = 0; i < APPLEVELS; i++)
        {
          kit_card_row(k, S_APPNAME + i, st->appname[i])->id = S_APPROW + i;
            kit_label(k, S_APPLEVEL + 2 * i + 1, st->applevel[i],
                      KIT_TEXT_DIM);
            kit_slider(k, S_APPLEVEL + 2 * i, 0, 0, FPCMIX_VOLUME_MAX);
            kit_size(k, 200, 0);
          kit_end(k);
        }
    kit_end(k);
  kit_end(k);
}

static bool sound_playing(const struct fpcmix_info_s *info, const char *name)
{
  int i;

  for (i = 0; i < info->nchan && i < FPCMIX_NCHAN_MAX; i++)
    {
      if (info->chan[i].state == FPCMIX_PLAYING &&
          strcmp(info->chan[i].name, name) == 0)
        {
          return true;
        }
    }

  return false;
}
#endif

#ifdef CONFIG_NETUTILS_NETCOP
static void settings_sync(struct settings_s *st);
static void set_text(struct settings_s *st, int id, char *buf, size_t size,
                     const char *fmt, ...);

/* Wi-Fi and Bluetooth: the network coprocessor (desk_net.c, docs/netcop.md).
 * What it shows (the link, the WiFi's state, the bonded devices) is g_net,
 * kept up to date by its events; scans, joins and pairing are requests
 * whose answers come back here (desk_net_request()).  No coprocessor:
 * "No WiFi hardware", and nothing else.
 */

/* Signal bars, and a lock for a secured network, in KIT_ICON x KIT_ICON */

static void net_bars(struct desk_ops_s *o, int x, int y, int bars, bool lock,
                     uint16_t fg)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      int h = 4 + 3 * i;

      ops_rrect(o, x + i * 3, y + 14 - h, 2, h, LUT_R3, 1, fg,
                i < bars ? 255 : 60);
    }

  if (lock)
    {
      ops_rrect(o, x + 12, y + 3, 5, 5, LUT_R3, 2, fg, 255);
      ops_rrect(o, x + 13, y + 4, 3, 4, LUT_R3, 1, g_theme.window_bg, 255);
      ops_fill(o, x + 11, y + 7, 7, 6, fg);
    }
}

/* A BLE device: a keyboard (keys), a mouse (a body), a pad (a cross) */

static void ble_glyph(struct desk_ops_s *o, int x, int y, uint8_t kind,
                      uint16_t fg)
{
  if (kind & NC_BLE_KIND_KEYBOARD)
    {
      int i;

      ops_rrect(o, x, y + 3, 16, 10, LUT_R3, 2, fg, 255);
      for (i = 0; i < 4; i++)
        {
          ops_fill(o, x + 2 + i * 3, y + 5, 2, 2, g_theme.window_bg);
        }

      ops_fill(o, x + 4, y + 9, 8, 2, g_theme.window_bg);
    }
  else if (kind & NC_BLE_KIND_MOUSE)
    {
      ops_rrect(o, x + 3, y + 1, 10, 14, LUT_R6, 5, fg, 255);
      ops_fill(o, x + 8, y + 2, 1, 5, g_theme.window_bg);
    }
  else if (kind & NC_BLE_KIND_GAMEPAD)
    {
      ops_rrect(o, x, y + 3, 16, 10, LUT_R6, 5, fg, 255);
      ops_fill(o, x + 3, y + 7, 5, 2, g_theme.window_bg);
      ops_fill(o, x + 4, y + 6, 2, 4, g_theme.window_bg);
      ops_disc(o, x + 12, y + 8, LUT_R3, 1, g_theme.window_bg, 255);
    }
  else
    {
      ops_disc(o, x + 8, y + 8, LUT_R6, 6, fg, 255);
    }
}

static const char *ble_kind(uint8_t kind)
{
  return kind & NC_BLE_KIND_KEYBOARD ? "Keyboard" :
         kind & NC_BLE_KIND_MOUSE ? "Mouse" :
         kind & NC_BLE_KIND_GAMEPAD ? "Game pad" : "Other";
}

static const char *ap_text(struct kit_s *k, struct kit_widget_s *wg,
                           int row, int col, char *buf, int size)
{
  struct settings_s *st = k->priv;
  const struct nc_wifi_ap *a = &st->aps[row];

  if (col == 0)
    {
      return a->ssid[0] != '\0' ? a->ssid : "(hidden)";
    }

  return a->flags & NC_WIFI_AP_CURRENT ? "Connected" :
         a->flags & NC_WIFI_AP_SAVED ? "Saved" :
         ncx_wifi_auth_str(a->auth);
}

static void ap_icon(struct kit_s *k, struct kit_widget_s *wg,
                    struct desk_ops_s *o, int row, int x, int y, uint16_t fg,
                    uint16_t bg)
{
  struct settings_s *st = k->priv;

  net_bars(o, x - 2, y, ncx_rssi_bars(st->aps[row].rssi),
           st->aps[row].auth != NC_WIFI_AUTH_OPEN, fg);
}

static const char *saved_text(struct kit_s *k, struct kit_widget_s *wg,
                              int row, int col, char *buf, int size)
{
  struct settings_s *st = k->priv;

  if (col == 0)
    {
      return st->saved[row].ssid;
    }

  return st->saved[row].flags & NC_WIFI_AP_CURRENT ? "Connected" :
         ncx_wifi_auth_str(st->saved[row].auth);
}

static const char *paired_text(struct kit_s *k, struct kit_widget_s *wg,
                               int row, int col, char *buf, int size)
{
  const struct nc_ble_dev *d = &g_net.ble[row];

  if (col == 0)
    {
      return d->name[0] != '\0' ? d->name : ble_kind(d->kind);
    }

  if (d->state != NC_BLE_ST_CONNECTED)
    {
      return d->state == NC_BLE_ST_CONNECTING ? "Connecting" :
             "Not connected";
    }

  if (d->battery == 0xff)
    {
      return "Connected";
    }

  snprintf(buf, size, "Connected, %u%%", d->battery);
  return buf;
}

static void paired_icon(struct kit_s *k, struct kit_widget_s *wg,
                        struct desk_ops_s *o, int row, int x, int y,
                        uint16_t fg, uint16_t bg)
{
  ble_glyph(o, x, y, g_net.ble[row].kind, fg);
}

static const char *found_text(struct kit_s *k, struct kit_widget_s *wg,
                              int row, int col, char *buf, int size)
{
  struct settings_s *st = k->priv;
  const struct nc_ble_adv *a = &st->found[row];

  if (col == 0)
    {
      return a->name[0] != '\0' ? a->name : ble_kind(a->kind);
    }

  return a->flags & NC_BLE_DEV_BONDED ? "Paired" : ble_kind(a->kind);
}

static void found_icon(struct kit_s *k, struct kit_widget_s *wg,
                       struct desk_ops_s *o, int row, int x, int y,
                       uint16_t fg, uint16_t bg)
{
  struct settings_s *st = k->priv;

  ble_glyph(o, x, y, st->found[row].kind, fg);
}

static const struct kit_col_s g_netcols[2] =
{
  { "Name", 0, 0, 0 },
  { "", 130, 0, KIT_COL_DIM | KIT_COL_RIGHT }
};

static void page_wifi(struct settings_s *st)
{
  struct kit_s *k = &st->kit;
  struct kit_widget_s *wg;

  page_begin(k, SETTINGS_WIFI, "Wi-Fi");
    kit_label(k, S_W_NOHW, "No WiFi hardware\nThe network coprocessor (a "
              "XIAO ESP32-C6 on UART0) does not answer.", KIT_TEXT_DIM);

    kit_card(k)->id = S_W_CARD;
      kit_card_row(k, S_W_ONLBL, st->wonlbl);
        kit_switch(k, S_W_ON, false);
      kit_end(k);
      kit_card_row(k, S_W_NETLBL, st->wnetlbl)->id = S_W_NETROW;
        kit_button(k, S_W_DISC, "Disconnect", KIT_ICON_NONE,
                   KIT_BTN_NORMAL);
      kit_end(k);
    kit_end(k);

    wg = kit_row(k, KIT_BOX_NONE);
    wg->id  = S_W_NETHEAD;
    wg->gap = 8;
      kit_label(k, 0, "Networks", KIT_TEXT_BOLD);
      kit_grow(k, 1);
      kit_button(k, S_W_SCAN, "Scan", KIT_ICON_REFRESH, KIT_BTN_NORMAL);
    kit_end(k);

    st->aplist.sel     = -1;
    st->aplist.sortcol = -1;
    st->aplist.ncols   = 2;
    st->aplist.cols    = g_netcols;
    st->aplist.text    = ap_text;
    st->aplist.icon    = ap_icon;
    st->aplist.empty   = "No networks found";
    kit_list(k, S_W_LIST, &st->aplist, KIT_LIST_PLAIN);
    kit_size(k, 0, 5 * KIT_ROW_H + 8);

    kit_card(k)->id = S_W_JOINCARD;
      kit_card_row(k, S_W_JOINLBL, st->joinlbl);
        kit_field(k, S_W_PASS, &st->pass, "Password")->flags |=
          KIT_F_SECRET;
        kit_size(k, 180, 0);
        kit_button(k, S_W_JOIN, "Join", KIT_ICON_NONE, KIT_BTN_DEFAULT);
      kit_end(k);
    kit_end(k);
    kit_label(k, S_W_MSG, st->wmsg, KIT_TEXT_SMALL | KIT_TEXT_DIM);

    wg = kit_row(k, KIT_BOX_NONE);
    wg->id  = S_W_SAVEDHEAD;
    wg->gap = 8;
      kit_label(k, 0, "Saved networks", KIT_TEXT_BOLD);
      kit_grow(k, 1);
      kit_button(k, S_W_FORGET, "Forget", KIT_ICON_NONE, KIT_BTN_NORMAL);
    kit_end(k);

    st->savedlist.sel     = -1;
    st->savedlist.sortcol = -1;
    st->savedlist.ncols   = 2;
    st->savedlist.cols    = g_netcols;
    st->savedlist.text    = saved_text;
    st->savedlist.empty   = "None";
    kit_list(k, S_W_SAVED, &st->savedlist, KIT_LIST_PLAIN);
    kit_size(k, 0, 3 * KIT_ROW_H + 8);
  kit_end(k);
}

static void page_bluetooth(struct settings_s *st)
{
  struct kit_s *k = &st->kit;
  struct kit_widget_s *wg;

  page_begin(k, SETTINGS_BLUETOOTH, "Bluetooth");
    kit_label(k, S_B_NOHW, "No Bluetooth hardware\nThe network coprocessor "
              "(a XIAO ESP32-C6 on UART0) does not answer.", KIT_TEXT_DIM);

    wg = kit_col(k, KIT_BOX_NONE);
    wg->id  = S_B_BODY;
    wg->gap = 12;
      wg = kit_row(k, KIT_BOX_NONE);
      wg->gap = 8;
        kit_label(k, 0, "My devices\nKeyboards, mice and game pads (BLE)",
                  KIT_TEXT_BOLD);
        kit_grow(k, 1);
        kit_button(k, S_B_DISC, "Disconnect", KIT_ICON_NONE,
                   KIT_BTN_NORMAL);
        kit_button(k, S_B_FORGET, "Forget", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_end(k);

      st->pairedlist.sel     = -1;
      st->pairedlist.sortcol = -1;
      st->pairedlist.ncols   = 2;
      st->pairedlist.cols    = g_netcols;
      st->pairedlist.text    = paired_text;
      st->pairedlist.icon    = paired_icon;
      st->pairedlist.empty   = "None paired";
      kit_list(k, S_B_PAIRED, &st->pairedlist, KIT_LIST_PLAIN);
      kit_size(k, 0, 3 * KIT_ROW_H + 8);

      wg = kit_row(k, KIT_BOX_NONE);
      wg->gap = 8;
        kit_label(k, 0, "Nearby", KIT_TEXT_BOLD);
        kit_grow(k, 1);
        kit_button(k, S_B_SCAN, "Scan", KIT_ICON_REFRESH, KIT_BTN_NORMAL);
        kit_button(k, S_B_PAIR, "Pair", KIT_ICON_NONE, KIT_BTN_DEFAULT);
      kit_end(k);

      st->foundlist.sel     = -1;
      st->foundlist.sortcol = -1;
      st->foundlist.ncols   = 2;
      st->foundlist.cols    = g_netcols;
      st->foundlist.text    = found_text;
      st->foundlist.icon    = found_icon;
      st->foundlist.empty   = "Scan to find devices";
      kit_list(k, S_B_FOUND, &st->foundlist, KIT_LIST_PLAIN);
      kit_size(k, 0, 4 * KIT_ROW_H + 8);

      kit_label(k, S_B_MSG, st->bmsg, KIT_TEXT_DIM);
    kit_end(k);
  kit_end(k);
}

static void net_msg(struct settings_s *st, int id, const char *fmt, ...)
{
  char *buf = id == S_W_MSG ? st->wmsg : st->bmsg;
  size_t size = id == S_W_MSG ? sizeof(st->wmsg) : sizeof(st->bmsg);
  char tmp[96];
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

static void cb_scan(void *arg, const struct ncx_msg *m, const uint8_t *body,
                    size_t n)
{
  struct settings_s *st = arg;
  struct nc_rd rd;
  int cnt;

  if (m->kind == NCX_MSG_PART)
    {
      nc_rd_init(&rd, body, n);
      for (cnt = nc_rd_u8(&rd); cnt > 0 && st->naps < NAPS; cnt--)
        {
          if (nc_dec_wifi_ap(&rd, &st->aps[st->naps]) != 0)
            {
              break;
            }

          st->naps++;
        }

      return;
    }

  st->scanning = false;
  st->aplist.n = st->naps;
  if (st->aplist.sel >= st->naps)
    {
      st->aplist.sel = -1;
    }

  kit_list_changed(&st->kit, S_W_LIST);
  if (g_st == st)
    {
      settings_sync(st);
      kit_flush(&st->kit);
    }
}

static void cb_saved(void *arg, const struct ncx_msg *m, const uint8_t *body,
                     size_t n)
{
  struct settings_s *st = arg;
  struct nc_rd rd;
  int cnt;

  if (m->kind == NCX_MSG_PART)
    {
      nc_rd_init(&rd, body, n);
      for (cnt = nc_rd_u8(&rd); cnt > 0 && st->nsaved < NSAVED; cnt--)
        {
          if (nc_dec_wifi_saved(&rd, &st->saved[st->nsaved]) != 0)
            {
              break;
            }

          st->nsaved++;
        }

      return;
    }

  st->savedlist.n = st->nsaved;
  if (st->savedlist.sel >= st->nsaved)
    {
      st->savedlist.sel = -1;
    }

  kit_list_changed(&st->kit, S_W_SAVED);
  kit_flush(&st->kit);
}

static void wifi_refresh(struct settings_s *st)
{
  struct nc_wifi_scan sc;
  uint8_t body[4];
  struct nc_wr w;

  if (!st->scanning)
    {
      memset(&sc, 0, sizeof(sc));
      nc_wr_init(&w, body, sizeof(body));
      nc_enc_wifi_scan(&w, &sc);
      st->naps = 0;
      st->scanning = desk_net_request(NC_OP_WIFI_SCAN, body, w.n, cb_scan,
                                      st) == 0;
    }

  st->nsaved = 0;
  desk_net_request(NC_OP_WIFI_SAVED, NULL, 0, cb_saved, st);
  st->scanned = true;
}

static void cb_wifi_done(void *arg, const struct ncx_msg *m,
                         const uint8_t *body, size_t n)
{
  struct settings_s *st = arg;

  if (m->kind != NCX_MSG_FINAL)
    {
      return;
    }

  if (m->code == NC_OP_WIFI_CONNECT)
    {
      st->joining = false;
      if (m->status < 0)
        {
          net_msg(st, S_W_MSG, "%s", m->status == -EACCES ?
                  "Wrong password" : m->status == -ENOENT ?
                  "The network is not in range" : m->status == -ETIMEDOUT ?
                  "No answer from the network" : "Could not join");
        }
      else
        {
          net_msg(st, S_W_MSG, "%s", "");
          memset(&st->pass, 0, sizeof(st->pass));
          st->aplist.sel = -1;
          kit_list_changed(&st->kit, S_W_LIST);
        }
    }

  wifi_refresh(st);
  settings_sync(st);
  kit_flush(&st->kit);
}

static void wifi_join(struct settings_s *st)
{
  const struct nc_wifi_ap *a;
  struct nc_wifi_connect c;
  uint8_t body[128];
  struct nc_wr w;

  if (st->aplist.sel < 0 || st->aplist.sel >= st->naps || st->joining)
    {
      return;
    }

  a = &st->aps[st->aplist.sel];
  memset(&c, 0, sizeof(c));
  c.flags     = NC_WIFI_CONN_SAVE;
  c.timeout_s = 20;
  strlcpy(c.ssid, a->ssid, sizeof(c.ssid));
  strlcpy(c.password, st->pass.text, sizeof(c.password));
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_wifi_connect(&w, &c);
  if (desk_net_request(NC_OP_WIFI_CONNECT, body, w.n, cb_wifi_done, st) == 0)
    {
      st->joining = true;
      net_msg(st, S_W_MSG, "Joining %s...", a->ssid);
    }
}

static void wifi_onoff(struct settings_s *st, bool on)
{
  struct nc_wifi_config c;
  uint8_t body[48];
  uint8_t stay = NC_WIFI_DISC_STAY;
  struct nc_wr w;
  int i;

  memset(&c, 0, sizeof(c));
  c.mask        = NC_WIFI_CFG_AUTOCONNECT;
  c.autoconnect = on;
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_wifi_config(&w, &c);
  desk_net_request(NC_OP_WIFI_CONFIG, body, w.n, cb_wifi_done, st);
  if (!on)
    {
      desk_net_request(NC_OP_WIFI_DISCONNECT, &stay, 1, cb_wifi_done, st);
      return;
    }

  /* On: the strongest saved network in range, now */

  for (i = 0; i < st->naps; i++)
    {
      if (st->aps[i].flags & NC_WIFI_AP_SAVED)
        {
          memset(&st->pass, 0, sizeof(st->pass));
          st->aplist.sel = i;
          wifi_join(st);
          break;
        }
    }
}

static void cb_ble_scan(void *arg, const struct ncx_msg *m,
                        const uint8_t *body, size_t n)
{
  struct settings_s *st = arg;
  struct nc_rd rd;
  int cnt;

  if (m->kind == NCX_MSG_PART)
    {
      nc_rd_init(&rd, body, n);
      for (cnt = nc_rd_u8(&rd); cnt > 0 && st->nfound < NFOUND; cnt--)
        {
          if (nc_dec_ble_adv(&rd, &st->found[st->nfound]) != 0)
            {
              break;
            }

          st->nfound++;
        }

      st->foundlist.n = st->nfound;
      kit_list_changed(&st->kit, S_B_FOUND);
      kit_flush(&st->kit);
      return;
    }

  st->bscanning = false;
  if (!st->pairing)
    {
      net_msg(st, S_B_MSG, "%s", st->nfound > 0 ? "" :
              "Nothing found: is it in pairing mode?");
    }

  settings_sync(st);
  kit_flush(&st->kit);
}

static void cb_ble_done(void *arg, const struct ncx_msg *m,
                        const uint8_t *body, size_t n)
{
  struct settings_s *st = arg;

  if (m->kind != NCX_MSG_FINAL)
    {
      return;
    }

  if (m->code == NC_OP_BLE_PAIR)
    {
      struct nc_ble_dev d;
      struct nc_rd rd;

      st->pairing = false;
      kit_dialog_close(&st->kit);
      nc_rd_init(&rd, body, n);
      if (m->status >= 0 && nc_dec_ble_dev(&rd, &d) == 0)
        {
          net_msg(st, S_B_MSG, "Paired with %s", d.name);
        }
      else
        {
          net_msg(st, S_B_MSG, "%s", m->status == -EACCES ?
                  "Pairing refused (a wrong passkey?)" :
                  m->status == -ETIMEDOUT ? "No answer from the device" :
                  m->status == -EOPNOTSUPP ?
                  "Not a keyboard, mouse or game pad" : "Could not pair");
        }
    }

  settings_sync(st);
  kit_flush(&st->kit);
}

static void ble_pair(struct settings_s *st)
{
  struct nc_ble_pair pr;
  uint8_t body[16];
  struct nc_wr w;
  const struct nc_ble_adv *a;

  if (st->foundlist.sel < 0 || st->foundlist.sel >= st->nfound ||
      st->pairing)
    {
      return;
    }

  a = &st->found[st->foundlist.sel];
  memset(&pr, 0, sizeof(pr));
  memcpy(pr.addr, a->addr, 6);
  pr.addr_type = a->addr_type;
  pr.timeout_s = 30;
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_ble_pair(&w, &pr);
  if (desk_net_request(NC_OP_BLE_PAIR, body, w.n, cb_ble_done, st) == 0)
    {
      st->pairing = true;
      net_msg(st, S_B_MSG, "Pairing with %s...", a->name);
    }
}

/* A passkey the coprocessor says, while pairing */

static char g_pktext[96];

static void ble_passkey(struct settings_s *st)
{
  static const char * const yesno[2] =
  {
    "Pair", "Cancel"
  };

  const struct nc_ble_passkey *pk = &g_net.pk;

  g_net.pk_pending = false;
  if (!st->pairing)
    {
      return;
    }

  switch (pk->action)
    {
      case NC_BLE_PK_DISPLAY:
        net_msg(st, S_B_MSG, "Type %06lu on the keyboard, then Enter",
                (unsigned long)pk->passkey);
        break;

      case NC_BLE_PK_CONFIRM:
        snprintf(g_pktext, sizeof(g_pktext), "Does the device show "
                 "%06lu?", (unsigned long)pk->passkey);
        kit_dialog(&st->kit, S_B_PAIR, KIT_DLG_CONFIRM, "Pair", g_pktext,
                   yesno, 2);
        break;

      default:
        kit_dialog(&st->kit, S_B_PAIR, KIT_DLG_PROMPT,
                   "The passkey the device shows", "", yesno, 2);
        break;
    }
}

static void ble_reply(struct settings_s *st, bool accept, uint32_t key)
{
  struct nc_ble_passkey pk = g_net.pk;
  uint8_t body[16];
  struct nc_wr w;

  pk.action  = accept;
  pk.passkey = key;
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_ble_passkey(&w, &pk);
  desk_net_request(NC_OP_BLE_PASSKEY_REPLY, body, w.n, cb_ble_done, st);
}

static void ble_act(struct settings_s *st, uint16_t op)
{
  int sel = st->pairedlist.sel;

  if (sel >= 0 && sel < g_net.nble)
    {
      desk_net_request(op, g_net.ble[sel].addr, 6, cb_ble_done, st);
    }
}

/* What the pages show, from g_net and what came back */

static void net_sync(struct settings_s *st)
{
  struct kit_s *k = &st->kit;
  const struct nc_wifi_status *ws = &g_net.wifi;
  bool whw = desk_net_wifi_hw();
  bool bhw = desk_net_ble_hw();
  bool up = g_net.have_wifi && ws->state == NC_WIFI_UP;
  bool on = g_net.have_wifi &&
            ((ws->flags & NC_WIFI_ST_AUTOCONNECT) != 0 ||
             ws->state != NC_WIFI_IDLE);
  const struct nc_wifi_ap *sel = NULL;
  bool needpass;

  /* Wi-Fi */

  kit_show(k, S_W_NOHW, !whw);
  kit_show(k, S_W_CARD, whw);
  kit_show(k, S_W_NETHEAD, whw);
  kit_show(k, S_W_LIST, whw);
  kit_show(k, S_W_SAVEDHEAD, whw);
  kit_show(k, S_W_SAVED, whw);
  if (whw && !st->scanned && g_page == SETTINGS_WIFI)
    {
      wifi_refresh(st);
    }
  else if (!whw)
    {
      st->scanned = false;
    }

  kit_set_value(k, S_W_ON, on);
  set_text(st, S_W_ONLBL, st->wonlbl, sizeof(st->wonlbl), "Wi-Fi\n%s",
           !g_net.have_wifi ? "" :
           up ? "Connected" :
           ws->state == NC_WIFI_CONNECTING ||
           ws->state == NC_WIFI_CONNECTED ? "Connecting..." :
           ws->state == NC_WIFI_RETRYING ? "Looking for a network" :
           on ? "Not connected" : "Off");
  kit_show(k, S_W_NETROW, whw && g_net.have_wifi && ws->ssid[0] != '\0' &&
           ws->state != NC_WIFI_IDLE);
  set_text(st, S_W_NETLBL, st->wnetlbl, sizeof(st->wnetlbl),
           up ? "%s\n%u.%u.%u.%u, signal %d dBm" : "%s\n%s", ws->ssid,
           up ? ws->ip[0] : 0, up ? ws->ip[1] : 0, up ? ws->ip[2] : 0,
           up ? ws->ip[3] : 0, ws->rssi);
  if (!up)
    {
      set_text(st, S_W_NETLBL, st->wnetlbl, sizeof(st->wnetlbl), "%s\n%s",
               ws->ssid, ncx_wifi_state_str(ws->state));
    }

  kit_enable(k, S_W_SCAN, !st->scanning);
  kit_enable(k, S_W_FORGET, st->savedlist.sel >= 0);
  if (st->aplist.sel >= 0 && st->aplist.sel < st->naps)
    {
      sel = &st->aps[st->aplist.sel];
    }

  needpass = sel != NULL && sel->auth != NC_WIFI_AUTH_OPEN &&
             !(sel->flags & NC_WIFI_AP_SAVED);
  kit_show(k, S_W_JOINCARD, whw && sel != NULL &&
           !(sel->flags & NC_WIFI_AP_CURRENT));
  if (sel != NULL)
    {
      set_text(st, S_W_JOINLBL, st->joinlbl, sizeof(st->joinlbl),
               "Join %s\n%s", sel->ssid, needpass ?
               ncx_wifi_auth_str(sel->auth) : sel->flags & NC_WIFI_AP_SAVED ?
               "Saved" : "Open");
    }

  kit_show(k, S_W_PASS, needpass);
  kit_enable(k, S_W_JOIN, !st->joining);
  kit_show(k, S_W_MSG, whw);

  /* Bluetooth */

  kit_show(k, S_B_NOHW, !bhw);
  kit_show(k, S_B_BODY, bhw);
  if (st->pairedlist.n != g_net.nble)
    {
      st->pairedlist.n = g_net.nble;
      if (st->pairedlist.sel >= g_net.nble)
        {
          st->pairedlist.sel = -1;
        }
    }

  if (st->blegen != g_net.blegen)
    {
      st->blegen = g_net.blegen;
      kit_list_changed(k, S_B_PAIRED);
    }

  kit_enable(k, S_B_DISC, st->pairedlist.sel >= 0);
  kit_enable(k, S_B_FORGET, st->pairedlist.sel >= 0);
  kit_enable(k, S_B_SCAN, !st->bscanning);
  kit_enable(k, S_B_PAIR, st->foundlist.sel >= 0 && !st->pairing);
  if (g_net.pk_pending)
    {
      ble_passkey(st);
    }
}

/* An event of the Wi-Fi or Bluetooth pages: true if it was theirs */

static bool net_event(struct settings_s *st, int id, int ev, int v)
{
  struct kit_s *k = &st->kit;

  if (ev == KIT_EV_DIALOG && k->key == S_B_PAIR)
    {
      if (g_net.pk.action == NC_BLE_PK_CONFIRM)
        {
          ble_reply(st, k->arg == KIT_ANS_OK, g_net.pk.passkey);
        }
      else
        {
          ble_reply(st, k->arg == KIT_ANS_OK,
                    k->answer != NULL ? strtoul(k->answer, NULL, 10) : 0);
        }

      return true;
    }

  if (id < S_NET || id >= S_NET_END)
    {
      return false;
    }

  switch (id)
    {
      case S_W_ON:
        if (ev == KIT_EV_CHANGE)
          {
            wifi_onoff(st, v != 0);
          }
        break;

      case S_W_DISC:
        if (ev == KIT_EV_CLICK)
          {
            uint8_t stay = NC_WIFI_DISC_STAY;

            desk_net_request(NC_OP_WIFI_DISCONNECT, &stay, 1, cb_wifi_done,
                             st);
          }
        break;

      case S_W_SCAN:
        if (ev == KIT_EV_CLICK)
          {
            wifi_refresh(st);
          }
        break;

      case S_W_LIST:
        memset(&st->pass, 0, sizeof(st->pass));
        net_msg(st, S_W_MSG, "%s", "");
        if (ev == KIT_EV_ACTIVATE)
          {
            const struct nc_wifi_ap *a = st->aplist.sel >= 0 ?
                                         &st->aps[st->aplist.sel] : NULL;

            if (a != NULL && (a->auth == NC_WIFI_AUTH_OPEN ||
                              (a->flags & NC_WIFI_AP_SAVED)))
              {
                wifi_join(st);
              }
          }

        settings_sync(st);
        if (kit_get(k, S_W_PASS) != NULL &&
            !(kit_get(k, S_W_PASS)->flags & KIT_F_HIDDEN))
          {
            kit_focus(k, S_W_PASS);
          }
        break;

      case S_W_PASS:
        if (ev == KIT_EV_ACTIVATE)
          {
            wifi_join(st);
          }
        break;

      case S_W_JOIN:
        if (ev == KIT_EV_CLICK)
          {
            wifi_join(st);
          }
        break;

      case S_W_FORGET:
        if (ev == KIT_EV_CLICK && st->savedlist.sel >= 0 &&
            st->savedlist.sel < st->nsaved)
          {
            uint8_t body[NC_SSID_MAX + 2];
            struct nc_wr w;

            nc_wr_init(&w, body, sizeof(body));
            nc_wr_str8(&w, st->saved[st->savedlist.sel].ssid);
            desk_net_request(NC_OP_WIFI_FORGET, body, w.n, cb_wifi_done,
                             st);
            st->savedlist.sel = -1;
          }
        break;

      case S_B_SCAN:
        if (ev == KIT_EV_CLICK && !st->bscanning)
          {
            struct nc_ble_scan sc;
            uint8_t body[8];
            struct nc_wr w;

            memset(&sc, 0, sizeof(sc));
            sc.duration_ms = 5000;
            sc.flags = NC_BLE_SCAN_HID_ONLY;
            nc_wr_init(&w, body, sizeof(body));
            nc_enc_ble_scan(&w, &sc);
            st->nfound = 0;
            st->foundlist.n = 0;
            st->foundlist.sel = -1;
            kit_list_changed(k, S_B_FOUND);
            st->bscanning = desk_net_request(NC_OP_BLE_SCAN, body, w.n,
                                             cb_ble_scan, st) == 0;
            net_msg(st, S_B_MSG, "%s", "Looking for devices...");
          }
        break;

      case S_B_FOUND:
        if (ev == KIT_EV_ACTIVATE)
          {
            ble_pair(st);
          }
        break;

      case S_B_PAIR:
        if (ev == KIT_EV_CLICK)
          {
            ble_pair(st);
          }
        break;

      case S_B_DISC:
        if (ev == KIT_EV_CLICK)
          {
            ble_act(st, NC_OP_BLE_DISCONNECT);
          }
        break;

      case S_B_FORGET:
        if (ev == KIT_EV_CLICK)
          {
            ble_act(st, NC_OP_BLE_FORGET);
            st->pairedlist.sel = -1;
          }
        break;

      default:
        break;
    }

  settings_sync(st);
  return true;
}
#endif /* CONFIG_NETUTILS_NETCOP */

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

#ifdef CONFIG_FPCDESK_SOUND
static void sound_sync(struct settings_s *st);
#endif

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
  kit_set_value(k, S_FX3D, g_prefs.effects && g_desk.quad);
  kit_enable(k, S_FX3D, g_desk.quad);
  for (i = 0; i < FX_COUNT; i++)
    {
      kit_set_value(k, S_FXEACH + i, (g_prefs.fx >> i) & 1);
      kit_enable(k, S_FXEACH + i, g_desk.quad && g_prefs.effects);
    }

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
#ifdef CONFIG_FPCDESK_WALLPAPER
  for (i = 0; i < NWALLS; i++)
    {
      kit_set_value(k, S_WALL + i, g_desk.wallstyle == i);
    }
#endif

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

#ifdef CONFIG_FPCDESK_SOUND
  if (g_page == SETTINGS_SOUND)
    {
      sound_sync(st);                  /* It reads the mixer */
    }
#endif

#ifdef CONFIG_NETUTILS_NETCOP
  net_sync(st);
#endif
}

#ifdef CONFIG_FPCDESK_SOUND
static void sound_sync(struct settings_s *st)
{
  struct kit_s *k = &st->kit;
  const struct fpcmix_info_s *info = desk_sound_refresh();
  int v;
  int i;

  kit_show(k, S_SNDCARD, info != NULL);
  kit_show(k, S_ALERTCARD, info != NULL);
  kit_show(k, S_APPSLBL, info != NULL);
  if (info == NULL)
    {
      set_text(st, S_OUTPUT, st->output, sizeof(st->output), "%s",
               "No audio (/dev/mixer)");
      kit_show(k, S_NOAPPS, false);
      kit_show(k, S_APPCARD, false);
      return;
    }

  set_text(st, S_OUTPUT, st->output, sizeof(st->output), "%s", info->gpu ?
           "Output: HDMI, played by the GPU" :
           "No audio in the GPU (a bitstream before v7): sound is "
           "discarded");
  kit_set_value(k, S_VOLUME, info->master);
  set_text(st, S_VOLUMEV, st->volume, sizeof(st->volume), "%d%%",
           (info->master + 5) / 10);
  kit_set_value(k, S_MUTE, info->mute);
  v = desk_sound_getlevel(DESK_SOUND_ALERTS);
  kit_set_value(k, S_ALERTS, v);
  set_text(st, S_ALERTSV, st->alerts, sizeof(st->alerts), "%d%%",
           (v + 5) / 10);
  kit_set_value(k, S_EFFECTS, desk_sound_effects());

  st->napps = 0;
  if (desk_sound_levels(&st->levels) >= 0)
    {
      for (i = 0; i < st->levels.n && st->napps < APPLEVELS; i++)
        {
          if (strcmp(st->levels.l[i].name, DESK_SOUND_ALERTS) != 0)
            {
              st->app[st->napps++] = i;
            }
        }
    }

  kit_show(k, S_NOAPPS, st->napps == 0);
  kit_show(k, S_APPCARD, st->napps > 0);
  for (i = 0; i < APPLEVELS; i++)
    {
      kit_show(k, S_APPROW + i, i < st->napps);
      if (i < st->napps)
        {
          const struct fpcmix_level_s *l = &st->levels.l[st->app[i]];

          set_text(st, S_APPNAME + i, st->appname[i], sizeof(st->appname[i]),
                   "%s%s", l->name, sound_playing(info, l->name) ?
                   "\nPlaying" : "");
          kit_set_value(k, S_APPLEVEL + 2 * i, l->level);
          set_text(st, S_APPLEVEL + 2 * i + 1, st->applevel[i],
                   sizeof(st->applevel[i]), "%d%%", (l->level + 5) / 10);
        }
    }
}
#endif

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

  settings_sync(st);                           /* A page's own (Sound) */
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

#ifdef CONFIG_NETUTILS_NETCOP
  if (net_event(st, id, ev, v))
    {
      return;
    }
#endif

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
      else if (id >= S_WALL && id < S_WALL + NWALLS &&
               g_desk.wallstyle != id - S_WALL)
        {
          /* The image may not be there (or the GPU cannot show it,
           * without BC1): the first drawn one then, as it says
           */

          g_desk.wallstyle = id - S_WALL;
          desk_wallpaper_draw();
          settings_sync(st);
          desk_prefs_save();
        }
#endif
      else if (id >= S_PIN && id < S_PIN + APP_COUNT)
        {
          desk_dock_pin(id - S_PIN, !desk_dock_pinned(id - S_PIN));
        }
#ifdef CONFIG_FPCDESK_SOUND
      else if (id == S_ALERTTEST)
        {
          desk_sound_play(SND_NOTIFY);
        }
#endif

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

      case S_FX3D:                     /* (settings_sync() enables the */
        g_prefs.effects = v;           /* effects' check boxes) */
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

#ifdef CONFIG_FPCDESK_SOUND
      case S_VOLUME:
        desk_sound_master(v);
        desk_topbar_draw();
        if (ev == KIT_EV_ACTIVATE)
          {
            desk_sound_play(SND_VOLUME);       /* How loud it is now */
            desk_prefs_save();
          }
        break;

      case S_MUTE:
        desk_sound_mute(v ? 1 : 0);
        desk_topbar_draw();
        desk_prefs_save();
        break;

      case S_ALERTS:
        desk_sound_level(DESK_SOUND_ALERTS, v);
        if (ev == KIT_EV_ACTIVATE)
          {
            desk_sound_play(SND_ALERT);
            desk_prefs_save();
          }
        break;

      case S_EFFECTS:
        desk_sound_set_effects(v);
        desk_prefs_save();
        break;

      case S_APPLEVEL:
      case S_APPLEVEL + 2:
      case S_APPLEVEL + 4:
        if ((id - S_APPLEVEL) / 2 < st->napps)
          {
            desk_sound_level(st->levels.l[st->app[(id - S_APPLEVEL) / 2]].name,
                             v);
            if (ev == KIT_EV_ACTIVATE)
              {
                desk_prefs_save();
              }
          }
        break;
#endif

      case S_TZ:
        v = g_prefs.tzoff + k->arg * 30;
        g_prefs.tzoff = v < -12 * 60 ? -12 * 60 : v > 14 * 60 ? 14 * 60 : v;
        desk_tz_apply();
        desk_topbar_draw();
        desk_prefs_save();
        break;

      default:
        if (id >= S_FXEACH && id < S_FXEACH + FX_COUNT)
          {
            g_prefs.fx = v ? g_prefs.fx | (1 << (id - S_FXEACH)) :
                             g_prefs.fx & ~(1 << (id - S_FXEACH));
            desk_prefs_save();
          }
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
#ifdef CONFIG_FPCDESK_SOUND
    page_sound(st);
#endif
#ifdef CONFIG_NETUTILS_NETCOP
    page_wifi(st);
    page_bluetooth(st);
#endif
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
      settings_sync(st);               /* (deskctl fx may have changed the */
      kit_activate(&st->kit);          /* effects) */
    }
}

static void settings_close(struct desk_win_s *w)
{
#ifdef CONFIG_NETUTILS_NETCOP
  desk_net_cancel(w->priv);                    /* No more answers for it */
#endif
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

/* What a page shows has changed (the mixer's state): what changed drawn
 * again if it is showing and nothing is being dragged
 */

void desk_settings_changed(int page)
{
  if (g_st != NULL && g_page == page && g_st->kit.down == KIT_NONE)
    {
      settings_sync(g_st);
      kit_flush(&g_st->kit);
    }
}

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
