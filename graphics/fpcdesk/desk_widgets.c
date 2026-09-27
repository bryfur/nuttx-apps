/****************************************************************************
 * apps/graphics/fpcdesk/desk_widgets.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Controls drawn into a list of operations (desk_widgets.h).  Round shapes
 * come from the client shape tables (LUT_R3, R6, R12), so their radii are
 * those.
 ****************************************************************************/

#include <nuttx/config.h>

#include "desk_widgets.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void wg_switch(struct desk_ops_s *o, int x, int y, bool on, bool hot)
{
  uint16_t track = on ? C_ACCENT :
                   hot ? g_theme.control_hover : g_theme.control;
  int kx = on ? x + WG_SWITCH_W - 14 : x + 14;

  ops_rrect(o, x, y, WG_SWITCH_W, WG_SWITCH_H, LUT_R12, 12, track, 255);
  ops_disc(o, kx, y + WG_SWITCH_H / 2 + 1, LUT_R12, 12, C_BLACK, 40);
  ops_disc(o, kx, y + WG_SWITCH_H / 2, LUT_R12, 12, C_WHITE, 255);
}

void wg_segments(struct desk_ops_s *o, int x, int y, int w, int h,
                 const char * const *labels, int n, int sel, int hot)
{
  const struct desk_font_s *f = &g_desk_font_ui;
  int i;

  ops_rrect(o, x, y, w, h, LUT_R6, 6, g_theme.control, 255);
  for (i = 0; i < n; i++)
    {
      int sx = x + w * i / n;
      int sw = x + w * (i + 1) / n - sx;
      uint16_t fg = g_theme.control_text;

      if (i == sel)
        {
          ops_rrect(o, sx + 2, y + 2, sw - 4, h - 4, LUT_R6, 5, C_ACCENT,
                    255);
          fg = C_ACCENT_TEXT;
        }
      else if (i == hot)
        {
          ops_rrect(o, sx + 2, y + 2, sw - 4, h - 4, LUT_R6, 5,
                    g_theme.control_hover, 255);
        }
      else if (i > 0 && i - 1 != sel && i - 1 != hot)
        {
          ops_blend(o, sx, y + 7, 1, h - 14, g_theme.text, 50);
        }

      ops_text_centre(o, i == sel ? &g_desk_font_uib : f, sx + sw / 2,
                      y + (h - text_height(f)) / 2, labels[i], fg, 255);
    }
}

int wg_segment_at(int x, int w, int n, int px)
{
  int i;

  for (i = 0; i < n; i++)
    {
      if (px >= x + w * i / n && px < x + w * (i + 1) / n)
        {
          return i;
        }
    }

  return -1;
}

void wg_slider(struct desk_ops_s *o, int x, int y, int w, int v, int min,
               int max, bool hot)
{
  int r = WG_SLIDER_H / 2;
  int span = w - 2 * r;
  int kx = x + r + (max > min ? (v - min) * span / (max - min) : 0);
  int ty = y + WG_SLIDER_H / 2 - 3;

  ops_rrect(o, x + r - 3, ty, span + 6, 6, LUT_R3, 3, g_theme.control_hover,
            255);
  ops_rrect(o, x + r - 3, ty, kx - x - r + 6, 6, LUT_R3, 3, C_ACCENT, 255);
  ops_disc(o, kx, y + r + 1, LUT_R12, 12, C_BLACK, hot ? 70 : 45);
  ops_disc(o, kx, y + r, LUT_R12, 12, C_WHITE, 255);
}

int wg_slider_value(int x, int w, int min, int max, int px)
{
  int r = WG_SLIDER_H / 2;
  int span = w - 2 * r;
  int v;

  if (span <= 0)
    {
      return min;
    }

  v = min + ((px - x - r) * (max - min) + span / 2) / span;
  return v < min ? min : v > max ? max : v;
}

void wg_check(struct desk_ops_s *o, int x, int y, bool on, bool hot)
{
  int i;

  if (!on)
    {
      ops_rrect(o, x, y, WG_CHECK, WG_CHECK, LUT_R3, 3,
                THEMED(RGB(0x70, 0x70, 0x78), RGB(0xa8, 0xa8, 0xb0)), 255);
      ops_rrect(o, x + 1, y + 1, WG_CHECK - 2, WG_CHECK - 2, LUT_R3, 3,
                hot ? g_theme.control_hover : g_theme.field, 255);
      return;
    }

  ops_rrect(o, x, y, WG_CHECK, WG_CHECK, LUT_R3, 3, C_ACCENT, 255);

  /* The tick: a short stroke down, a long one up, 2 pixels thick */

  for (i = 0; i < 4; i++)
    {
      ops_fill(o, x + 4 + i, y + 8 + i, 2, 2, C_ACCENT_TEXT);
    }

  for (i = 0; i < 7; i++)
    {
      ops_fill(o, x + 7 + i, y + 11 - i, 2, 2, C_ACCENT_TEXT);
    }
}

void wg_stepper(struct desk_ops_s *o, int x, int y, int w,
                const char *value, int hot)
{
  const struct desk_font_s *f = &g_desk_font_ui;
  int ty = y + (WG_STEP - text_height(f)) / 2;

  ops_rrect(o, x, y, WG_STEP, WG_STEP, LUT_R6, 6,
            hot < 0 ? g_theme.control_hover : g_theme.control, 255);
  ops_rrect(o, x + w - WG_STEP, y, WG_STEP, WG_STEP, LUT_R6, 6,
            hot > 0 ? g_theme.control_hover : g_theme.control, 255);
  ops_text_centre(o, &g_desk_font_uib, x + WG_STEP / 2, ty - 1,
                  DESK_CH_MINUS, g_theme.control_text, 255);
  ops_text_centre(o, &g_desk_font_uib, x + w - WG_STEP / 2, ty - 1, "+",
                  g_theme.control_text, 255);
  ops_text_centre(o, &g_desk_font_uib, x + w / 2, ty, value, g_theme.text,
                  255);
}

int wg_stepper_at(int x, int w, int px)
{
  return px >= x && px < x + WG_STEP ? -1 :
         px >= x + w - WG_STEP && px < x + w ? 1 : 0;
}

void wg_swatch(struct desk_ops_s *o, int cx, int cy, uint16_t c, bool sel,
               bool hot)
{
  ops_disc(o, cx, cy + 1, LUT_R12, WG_SWATCH_R, C_BLACK, hot ? 60 : 30);
  ops_disc(o, cx, cy, LUT_R12, WG_SWATCH_R, c, 255);
  if (sel)
    {
      ops_disc(o, cx, cy, LUT_R3, 3, C_WHITE, 255);
    }
}

void wg_sidebar_row(struct desk_ops_s *o, int x, int y, int w, int h,
                    const char *label, uint16_t tile, bool sel, bool hot)
{
  const struct desk_font_s *f = &g_desk_font_ui;

  if (sel)
    {
      ops_rrect(o, x, y, w, h, LUT_R6, 6, C_ACCENT, 255);
    }
  else if (hot)
    {
      ops_rrect(o, x, y, w, h, LUT_R6, 6, C_HILITE, THEMED(20, 14));
    }

  if (sel)
    {
      ops_rrect(o, x + 7, y + (h - 22) / 2, 22, 22, LUT_R6, 6, C_WHITE,
                200);
    }

  ops_rrect(o, x + 8, y + (h - 20) / 2, 20, 20, LUT_R6, 6, tile, 255);
  ops_blend(o, x + 12, y + (h - 20) / 2, 12, 1, C_WHITE, 70);
  ops_text(o, sel ? &g_desk_font_uib : f, x + 38,
           y + (h - text_height(f)) / 2, label,
           sel ? C_ACCENT_TEXT : g_theme.text, 255);
}

void wg_card(struct desk_ops_s *o, int x, int y, int w, int nrows)
{
  int i;

  if (g_theme.dark)
    {
      ops_rrect(o, x, y, w, nrows * WG_ROW_H, LUT_R12, 12, g_theme.card,
                255);
    }
  else
    {
      /* White on light grey: a hairline shows where it ends */

      ops_rrect(o, x, y, w, nrows * WG_ROW_H, LUT_R12, 12,
                RGB(0xde, 0xde, 0xe3), 255);
      ops_rrect(o, x + 1, y + 1, w - 2, nrows * WG_ROW_H - 2, LUT_R12, 11,
                g_theme.card, 255);
    }

  for (i = 1; i < nrows; i++)
    {
      ops_fill(o, x + 14, y + i * WG_ROW_H, w - 28, 1,
               THEMED(RGB(0x3c, 0x3c, 0x42), RGB(0xe6, 0xe6, 0xea)));
    }
}

void wg_row_label(struct desk_ops_s *o, int x, int y, const char *label,
                  const char *caption)
{
  const struct desk_font_s *f = &g_desk_font_ui;

  if (caption == NULL)
    {
      ops_text(o, f, x + 14, y + (WG_ROW_H - text_height(f)) / 2, label,
               g_theme.text, 255);
    }
  else
    {
      ops_text(o, f, x + 14, y + 6, label, g_theme.text, 255);
      ops_text(o, &g_desk_font_small, x + 14, y + 6 + text_height(f) + 1,
               caption, g_theme.text_dim, 255);
    }
}
