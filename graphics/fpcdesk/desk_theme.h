/****************************************************************************
 * apps/graphics/fpcdesk/desk_theme.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's colours by role, for a dark or light theme and an accent
 * chosen at run time.  Draw with g_theme.<role> (or desk.h's C_* names),
 * or THEMED(dark, light) for an application's own colours; never keep a
 * theme colour in a static initialiser, since the theme changes while
 * windows are open and everything is then drawn again.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_THEME_H
#define __APPS_GRAPHICS_FPCDESK_DESK_THEME_H

#include <stdbool.h>
#include <stdint.h>

#include "desk_font.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Themes and accents (desk_theme_set()) */

#define THEME_DARK          0
#define THEME_LIGHT         1
#define THEME_COUNT         2

#define ACCENT_BLUE         0
#define ACCENT_PURPLE       1
#define ACCENT_PINK         2
#define ACCENT_RED          3
#define ACCENT_ORANGE       4
#define ACCENT_YELLOW       5
#define ACCENT_GREEN        6
#define ACCENT_GRAPHITE     7
#define ACCENT_COUNT        8

/* A colour of an application's own that depends on the theme */

#define THEMED(d_, l_)      (g_theme.dark ? (d_) : (l_))

/* Metrics: corner radii (with their shape tables, desk.h LUT_R*), the
 * usual spacing, the height of a control
 */

#define UI_RADIUS_SMALL     3              /* LUT_R3: scroll thumbs, tags */
#define UI_RADIUS_CONTROL   6              /* LUT_R6: buttons, fields */
#define UI_RADIUS_CARD      12             /* LUT_R12: cards, tiles */
#define UI_PAD              12             /* Between groups, card insets */
#define UI_GAP              8              /* Between controls */
#define UI_CONTROL_H        28             /* Buttons, fields, segments */
#define UI_ROW_H            24             /* List rows */

/* Font roles */

#define FONT_BODY           (&g_desk_font_ui)      /* Regular 13 */
#define FONT_BODY_BOLD      (&g_desk_font_uib)     /* Medium 13 */
#define FONT_SMALL          (&g_desk_font_small)   /* Regular 11: captions */
#define FONT_HEADING        (&g_desk_font_title)   /* Medium 20 */
#define FONT_DISPLAY        (&g_desk_font_big)     /* Light 44: the time */
#define FONT_MONO           (&g_desk_font_mono)    /* Plex Mono 15, 9 x 18 */
#define FONT_MONO_BOLD      (&g_desk_font_monob)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* RGB565 colours by role */

struct desk_theme_s
{
  bool     dark;                   /* Dark theme (light text on dark) */

  /* Surfaces */

  uint16_t desktop;                /* Behind everything, without wallpaper */
  uint16_t window_bg;              /* Client areas */
  uint16_t card;                   /* Grouped content on window_bg: cards,
                                    * dialogs */
  uint16_t sidebar;                /* Sidebars (Files' places, Settings) */
  uint16_t bar;                    /* Tool and status bars */
  uint16_t field;                  /* Inside text fields */
  uint16_t separator;              /* Hairlines between areas */

  /* Text */

  uint16_t text;
  uint16_t text_dim;               /* Secondary: labels, units, captions */
  uint16_t text_faint;             /* Hints, disabled, grid lines' labels */

  /* The accent */

  uint16_t accent;
  uint16_t accent_text;            /* Text and symbols on the accent */
  uint16_t selection;              /* Selected rows where the accent itself
                                    * is too strong (text stays text) */
  uint16_t focus;                  /* Focus rings (the accent) */

  /* Controls: buttons, segmented controls, switches that are off */

  uint16_t control;
  uint16_t control_hover;
  uint16_t control_pressed;
  uint16_t control_text;

  /* States */

  uint16_t destructive;            /* Buttons that delete, end, discard */
  uint16_t error;                  /* Error text */
  uint16_t warning;
  uint16_t success;

  /* Overlays, blended at a low alpha over anything */

  uint16_t hilite;                 /* Hover: lighter on dark, darker on
                                    * light */
  uint16_t shade;                  /* Hairlines under bars, pressing */

  /* Scroll bars */

  uint16_t thumb;

  /* Window frames */

  uint16_t title_active;
  uint16_t title_inactive;
  uint16_t title_hilite;           /* The current tab, a pressed button */
  uint16_t title_text;
  uint16_t title_dim;
  uint16_t light_off;              /* Traffic lights of inactive windows */

  /* Panels: the top bar, the dock, menus, tooltips, the launcher */

  uint16_t panel_tint;             /* The top bar's backdrop */
  uint8_t  panel_alpha;
  uint16_t panel_text;
  uint16_t dock_tint;              /* The dock's glass */
  uint8_t  dock_alpha;
  uint8_t  dock_edge;              /* Alpha of its hairline of light */
  uint16_t popup;                  /* Menus, tooltips */
  uint8_t  popup_alpha;
  uint16_t popup_text;
  uint16_t scrim;                  /* The launcher's veil over the desktop */
  uint8_t  scrim_alpha;
  uint16_t scrim_text;             /* Text on it */

  /* Window effects (nx_effects_s), before Settings scale the shadow */

  uint8_t  shadow_active;
  uint8_t  shadow_inactive;
  uint8_t  border;                 /* The 1-pixel edge's alpha */
  bool     darkedge;               /* ... dark (NX_FX_DARKEDGE) */

  /* The terminal */

  uint16_t term_bg;
  uint16_t term_fg;
  uint16_t term_cursor;
  uint16_t term_pal[16];           /* ANSI colours 0-15 */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern struct desk_theme_s g_theme;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Set g_theme's colours only; desk_theme_changed() redraws */

void desk_theme_set(int theme, int accent);

/* A theme's colours before an accent (previews) */

const struct desk_theme_s *desk_theme_get(int theme);

/* An accent's name and colour in the current theme (swatches) */

const char *desk_accent_name(int accent);
uint16_t desk_accent_colour(int accent);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_THEME_H */
