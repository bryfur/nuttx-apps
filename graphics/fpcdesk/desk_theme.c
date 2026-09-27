/****************************************************************************
 * apps/graphics/fpcdesk/desk_theme.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The themes (dark, after macOS's and GNOME's dark ones; light, their
 * light ones) and the accent colours.
 ****************************************************************************/

#include <nuttx/config.h>

#include "desk.h"

/****************************************************************************
 * Public Data
 ****************************************************************************/

struct desk_theme_s g_theme;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct desk_theme_s g_themes[THEME_COUNT] =
{
  /* Dark */

  {
    .dark            = true,
    .desktop         = RGB(0x00, 0x00, 0x00),
    .window_bg       = RGB(0x20, 0x20, 0x24),
    .card            = RGB(0x30, 0x30, 0x34),
    .sidebar         = RGB(0x2a, 0x2a, 0x2e),
    .bar             = RGB(0x2a, 0x2a, 0x2e),
    .field           = RGB(0x16, 0x16, 0x1a),
    .separator       = RGB(0x14, 0x14, 0x16),
    .text            = RGB(0xe8, 0xe8, 0xea),
    .text_dim        = RGB(0x98, 0x98, 0x9e),
    .text_faint      = RGB(0x68, 0x68, 0x6e),
    .control         = RGB(0x3a, 0x3a, 0x40),
    .control_hover   = RGB(0x48, 0x48, 0x50),
    .control_pressed = RGB(0x2a, 0x2a, 0x30),
    .control_text    = RGB(0xe8, 0xe8, 0xea),
    .destructive     = RGB(0xe0, 0x3c, 0x34),
    .error           = RGB(0xff, 0x7b, 0x72),
    .warning         = RGB(0xff, 0x9f, 0x0a),
    .success         = RGB(0x30, 0xd1, 0x58),
    .hilite          = RGB(0xff, 0xff, 0xff),
    .shade           = RGB(0x00, 0x00, 0x00),
    .thumb           = RGB(0x80, 0x80, 0x88),
    .title_active    = RGB(0x38, 0x38, 0x3c),
    .title_inactive  = RGB(0x28, 0x28, 0x2c),
    .title_hilite    = RGB(0x48, 0x48, 0x4d),
    .title_text      = RGB(0xec, 0xec, 0xee),
    .title_dim       = RGB(0x8e, 0x8e, 0x93),
    .light_off       = RGB(0x48, 0x48, 0x4c),
    .panel_tint      = RGB(0x00, 0x00, 0x00),
    .panel_alpha     = 150,
    .panel_text      = RGB(0xff, 0xff, 0xff),
    .dock_tint       = RGB(0xf0, 0xf0, 0xff),
    .dock_alpha      = 60,
    .dock_edge       = 70,
    .popup           = RGB(0x20, 0x20, 0x24),
    .popup_alpha     = 235,
    .popup_text      = RGB(0xff, 0xff, 0xff),
    .scrim           = RGB(0x06, 0x06, 0x10),
    .scrim_alpha     = 175,
    .scrim_text      = RGB(0xff, 0xff, 0xff),
    .shadow_active   = 170,
    .shadow_inactive = 100,
    .border          = 34,
    .darkedge        = false,
    .term_bg         = RGB(0x18, 0x18, 0x1c),
    .term_fg         = RGB(0xd8, 0xd8, 0xd8),
    .term_cursor     = RGB(0xd8, 0xd8, 0xd8),
    .term_pal        =
    {
      RGB(0x3a, 0x3a, 0x40), RGB(0xff, 0x5f, 0x57), RGB(0x32, 0xd7, 0x4b),
      RGB(0xfe, 0xbc, 0x2e), RGB(0x3a, 0x8e, 0xff), RGB(0xbf, 0x5a, 0xf2),
      RGB(0x40, 0xc8, 0xe0), RGB(0xd8, 0xd8, 0xd8),
      RGB(0x6e, 0x6e, 0x76), RGB(0xff, 0x8a, 0x80), RGB(0x6c, 0xf0, 0x85),
      RGB(0xff, 0xd6, 0x5c), RGB(0x78, 0xb4, 0xff), RGB(0xd8, 0x92, 0xff),
      RGB(0x80, 0xe4, 0xf4), RGB(0xff, 0xff, 0xff)
    },
  },

  /* Light */

  {
    .dark            = false,
    .desktop         = RGB(0x9c, 0xa6, 0xb6),
    .window_bg       = RGB(0xf5, 0xf5, 0xf7),
    .card            = RGB(0xff, 0xff, 0xff),
    .sidebar         = RGB(0xeb, 0xeb, 0xee),
    .bar             = RGB(0xee, 0xee, 0xf1),
    .field           = RGB(0xff, 0xff, 0xff),
    .separator       = RGB(0xd4, 0xd4, 0xd9),
    .text            = RGB(0x1d, 0x1d, 0x1f),
    .text_dim        = RGB(0x6e, 0x6e, 0x73),
    .text_faint      = RGB(0xa8, 0xa8, 0xae),
    .control         = RGB(0xe3, 0xe3, 0xe8),
    .control_hover   = RGB(0xd7, 0xd7, 0xdd),
    .control_pressed = RGB(0xc6, 0xc6, 0xcd),
    .control_text    = RGB(0x1d, 0x1d, 0x1f),
    .destructive     = RGB(0xe0, 0x3c, 0x34),
    .error           = RGB(0xd7, 0x2c, 0x20),
    .warning         = RGB(0xc2, 0x74, 0x00),
    .success         = RGB(0x1f, 0xa2, 0x45),
    .hilite          = RGB(0x00, 0x00, 0x00),
    .shade           = RGB(0x00, 0x00, 0x00),
    .thumb           = RGB(0xa4, 0xa4, 0xac),
    .title_active    = RGB(0xe4, 0xe4, 0xe8),
    .title_inactive  = RGB(0xf2, 0xf2, 0xf4),
    .title_hilite    = RGB(0xf8, 0xf8, 0xfa),
    .title_text      = RGB(0x26, 0x26, 0x28),
    .title_dim       = RGB(0xa0, 0xa0, 0xa6),
    .light_off       = RGB(0xd0, 0xd0, 0xd4),
    .panel_tint      = RGB(0xf4, 0xf4, 0xf7),
    .panel_alpha     = 200,
    .panel_text      = RGB(0x1d, 0x1d, 0x1f),
    .dock_tint       = RGB(0xfa, 0xfa, 0xff),
    .dock_alpha      = 150,
    .dock_edge       = 160,
    .popup           = RGB(0xf6, 0xf6, 0xf8),
    .popup_alpha     = 245,
    .popup_text      = RGB(0x1d, 0x1d, 0x1f),
    .scrim           = RGB(0xe6, 0xe8, 0xee),
    .scrim_alpha     = 235,
    .scrim_text      = RGB(0x1d, 0x1d, 0x1f),
    .shadow_active   = 130,
    .shadow_inactive = 70,
    .border          = 46,
    .darkedge        = true,
    .term_bg         = RGB(0xff, 0xff, 0xff),
    .term_fg         = RGB(0x2a, 0x2a, 0x2e),
    .term_cursor     = RGB(0x3a, 0x3a, 0x40),
    .term_pal        =
    {
      RGB(0x38, 0x3a, 0x42), RGB(0xd0, 0x3c, 0x30), RGB(0x3c, 0x94, 0x3a),
      RGB(0xa8, 0x72, 0x00), RGB(0x30, 0x6a, 0xe0), RGB(0x9a, 0x2a, 0x9a),
      RGB(0x08, 0x7a, 0xa8), RGB(0x98, 0x99, 0x9e),
      RGB(0x69, 0x6c, 0x77), RGB(0xe4, 0x56, 0x49), RGB(0x50, 0xa1, 0x4f),
      RGB(0xc1, 0x84, 0x01), RGB(0x40, 0x78, 0xf2), RGB(0xa6, 0x26, 0xa4),
      RGB(0x01, 0x84, 0xbc), RGB(0xc8, 0xc8, 0xcc)
    },
  },
};

/* The accents, as macOS has them: for dark and for light */

static const struct
{
  const char *name;
  uint16_t dark;
  uint16_t light;
} g_accents[ACCENT_COUNT] =
{
  { "Blue",     RGB(0x0a, 0x84, 0xff), RGB(0x00, 0x7a, 0xff) },
  { "Purple",   RGB(0xbf, 0x5a, 0xf2), RGB(0xa5, 0x50, 0xd8) },
  { "Pink",     RGB(0xff, 0x37, 0x5f), RGB(0xf0, 0x2c, 0x58) },
  { "Red",      RGB(0xff, 0x45, 0x3a), RGB(0xf0, 0x3a, 0x2e) },
  { "Orange",   RGB(0xff, 0x9f, 0x0a), RGB(0xf5, 0x8c, 0x00) },
  { "Yellow",   RGB(0xff, 0xd6, 0x0a), RGB(0xf5, 0xc4, 0x00) },
  { "Green",    RGB(0x32, 0xd7, 0x4b), RGB(0x28, 0xb4, 0x46) },
  { "Graphite", RGB(0x98, 0x98, 0x9d), RGB(0x8a, 0x8a, 0x90) },
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void desk_theme_set(int theme, int accent)
{
  if (theme < 0 || theme >= THEME_COUNT)
    {
      theme = THEME_DARK;
    }

  if (accent < 0 || accent >= ACCENT_COUNT)
    {
      accent = ACCENT_BLUE;
    }

  g_theme = g_themes[theme];
  g_theme.accent = g_theme.dark ? g_accents[accent].dark :
                                  g_accents[accent].light;
  g_theme.focus  = g_theme.accent;

  /* Light accents (yellow, orange, graphite on dark) carry dark text */

  g_theme.accent_text = accent == ACCENT_YELLOW ? RGB(0x1d, 0x1d, 0x1f) :
                                                  RGB(0xff, 0xff, 0xff);

  /* A selection: the accent darkened into the card on dark, a pale
   * accent on light
   */

  g_theme.selection = g_theme.dark ?
                      colour_mix(g_theme.card, g_theme.accent, 110) :
                      colour_mix(g_theme.card, g_theme.accent, 60);
}

const struct desk_theme_s *desk_theme_get(int theme)
{
  return &g_themes[theme == THEME_LIGHT ? THEME_LIGHT : THEME_DARK];
}

const char *desk_accent_name(int accent)
{
  return accent >= 0 && accent < ACCENT_COUNT ? g_accents[accent].name : "";
}

uint16_t desk_accent_colour(int accent)
{
  if (accent < 0 || accent >= ACCENT_COUNT)
    {
      accent = ACCENT_BLUE;
    }

  return g_theme.dark ? g_accents[accent].dark : g_accents[accent].light;
}
