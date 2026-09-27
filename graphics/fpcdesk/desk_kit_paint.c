/****************************************************************************
 * apps/graphics/fpcdesk/desk_kit_paint.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The widget kit's controls, painted into a list of operations where the
 * caller says (desk_kit.h, kit_paint_*()), in the theme's colours: what
 * each control looks like in each state, and nothing else.  Round shapes
 * come from the client shape tables, so their radii are those of LUT_R3,
 * R6 and R12.
 ****************************************************************************/

#include <nuttx/config.h>

#include <string.h>

#include "desk_kit.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* A colour a little lighter (hovered) or darker (pressed) */

static uint16_t lighter(uint16_t c)
{
  return colour_mix(c, C_WHITE, 36);
}

static uint16_t darker(uint16_t c)
{
  return colour_mix(c, C_BLACK, 56);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* A focus ring around (x, y, w, h), KIT_RING outside it, on ground: a
 * band of the focus colour, then the ground again up to the control
 */

void kit_paint_ring(struct desk_ops_s *o, int x, int y, int w, int h,
                    int r, uint16_t ground)
{
  int slot = r >= 12 ? LUT_R12 : r >= 6 ? LUT_R6 : LUT_R3;
  int rr = r >= 12 ? 12 : r >= 6 ? 6 : 3;

  ops_rrect(o, x - KIT_RING, y - KIT_RING, w + 2 * KIT_RING,
            h + 2 * KIT_RING, slot, rr, g_theme.focus, 255);
  ops_rrect(o, x - 1, y - 1, w + 2, h + 2, slot, rr, ground, 255);
}

/* Icons: KIT_ICON square at (x, y) in c (cut-outs in bg) */

void kit_paint_icon(struct desk_ops_s *o, int icon, int x, int y,
                    uint16_t c, uint16_t bg)
{
  int i;

  switch (icon)
    {
      case KIT_ICON_BACK:
        ops_fill(o, x + 5, y + 7, 10, 2, c);
        ui_triangle(o, x + 3, y + 8, 5, 2, c, 255);
        break;

      case KIT_ICON_FORWARD:
        ops_fill(o, x + 1, y + 7, 10, 2, c);
        ui_triangle(o, x + 12, y + 8, 5, 3, c, 255);
        break;

      case KIT_ICON_UP:
        ops_fill(o, x + 7, y + 5, 2, 10, c);
        ui_triangle(o, x + 8, y + 3, 5, 0, c, 255);
        break;

      case KIT_ICON_DOWN:
        ops_fill(o, x + 7, y + 1, 2, 10, c);
        ui_triangle(o, x + 8, y + 12, 5, 1, c, 255);
        break;

      case KIT_ICON_REFRESH:

        /* A ring with a gap and an arrowhead */

        ops_disc(o, x + 8, y + 8, LUT_R6, 6, c, 255);
        ops_disc(o, x + 8, y + 8, LUT_R6, 4, bg, 255);
        ops_fill(o, x + 9, y + 1, 6, 7, bg);
        ui_triangle(o, x + 10, y + 4, 4, 3, c, 255);
        break;

      case KIT_ICON_CLOSE:
        ops_text_centre(o, KIT_FONT_BOLD, x + 8,
                        y + 8 - text_height(KIT_FONT_BOLD) / 2 - 1,
                        DESK_CH_TIMES, c, 255);
        break;

      case KIT_ICON_PLUS:
        ops_fill(o, x + 7, y + 3, 2, 10, c);

        /* Fall through */

      case KIT_ICON_MINUS:
        ops_fill(o, x + 3, y + 7, 10, 2, c);
        break;

      case KIT_ICON_CHECK:
        for (i = 0; i < 4; i++)
          {
            ops_fill(o, x + 3 + i, y + 7 + i, 2, 2, c);
          }

        for (i = 0; i < 7; i++)
          {
            ops_fill(o, x + 6 + i, y + 10 - i, 2, 2, c);
          }
        break;

      case KIT_ICON_CHEVRON:
        ui_triangle(o, x + 8, y + 8, 4, 1, c, 255);
        break;

      case KIT_ICON_MENU:
        for (i = 0; i < 3; i++)
          {
            ops_fill(o, x + 2, y + 3 + i * 5, 12, 2, c);
          }
        break;

      case KIT_ICON_FOLDER:
        {
          uint16_t f = RGB(0x5a, 0xa8, 0xf0);

          ops_rrect(o, x, y + 1, 8, 6, LUT_R3, 3, colour_mix(f, C_BLACK, 50),
                    255);
          ops_rrect(o, x, y + 3, 16, 12, LUT_R3, 3, f, 255);
          ops_blend(o, x + 2, y + 5, 12, 1, C_WHITE, 70);
        }
        break;

      case KIT_ICON_FILE:
        ops_rrect(o, x + 2, y, 12, 16, LUT_R3, 3, RGB(0xdc, 0xdc, 0xe2), 255);
        ops_blend(o, x + 10, y, 4, 4, C_BLACK, 70);
        for (i = 0; i < 3; i++)
          {
            ops_blend(o, x + 5, y + 6 + i * 3, i == 2 ? 4 : 6, 1,
                      RGB(0x60, 0x60, 0x68), 255);
          }
        break;

      case KIT_ICON_HOME:
        ui_triangle(o, x + 8, y + 4, 7, 0, c, 255);
        ops_fill(o, x + 3, y + 8, 10, 7, c);
        ops_fill(o, x + 7, y + 11, 3, 4, bg);
        break;

      case KIT_ICON_INFO:
      case KIT_ICON_WARNING:
        ops_disc(o, x + 8, y + 8, LUT_R6, 6, icon == KIT_ICON_INFO ?
                 g_theme.accent : g_theme.warning, 255);
        ops_text_centre(o, KIT_FONT_BOLD, x + 8,
                        y + 8 - text_height(KIT_FONT_BOLD) / 2,
                        icon == KIT_ICON_INFO ? "i" : "!", C_WHITE, 255);
        break;

      default:
        break;
    }
}

void kit_paint_button(struct desk_ops_s *o, int x, int y, int w, int h,
                      const char *label, int icon, int style, int look,
                      bool on)
{
  const struct desk_font_s *f = KIT_FONT;
  int state = look & KIT_LOOK_MASK;
  bool fill = true;
  uint16_t bg = g_theme.control;
  uint16_t fg = g_theme.control_text;
  uint8_t alpha = 255;

  if (style == KIT_BTN_TOGGLE && on)
    {
      style = KIT_BTN_DEFAULT;
    }

  switch (style)
    {
      case KIT_BTN_DEFAULT:
      case KIT_BTN_DANGER:
        bg = style == KIT_BTN_DEFAULT ? g_theme.accent : g_theme.destructive;
        fg = g_theme.accent_text;
        bg = state == KIT_LOOK_HOT ? lighter(bg) :
             state == KIT_LOOK_DOWN ? darker(bg) : bg;
        break;

      case KIT_BTN_FLAT:
        fill = state == KIT_LOOK_HOT || state == KIT_LOOK_DOWN;
        bg = state == KIT_LOOK_DOWN ? g_theme.control_pressed :
             g_theme.control_hover;
        break;

      default:
        bg = state == KIT_LOOK_HOT ? g_theme.control_hover :
             state == KIT_LOOK_DOWN ? g_theme.control_pressed : g_theme.control;
        break;
    }

  if (state == KIT_LOOK_DISABLED)
    {
      bg = g_theme.control;
      fg = g_theme.text_dim;
      alpha = 160;
      fill = style != KIT_BTN_FLAT;
    }

  if (fill)
    {
      ops_rrect(o, x, y, w, h, KIT_LUT_CONTROL, KIT_R_CONTROL, bg, 255);
    }

  if (icon != KIT_ICON_NONE && icon != KIT_ICON_CUSTOM)
    {
      int iw = KIT_ICON;
      int tw = label != NULL && label[0] != '\0' ?
               text_width(f, label) + 6 : 0;
      int ix = x + (w - iw - tw) / 2;

      kit_paint_icon(o, icon, ix, y + (h - KIT_ICON) / 2, fg,
                     fill ? bg : g_theme.window_bg);
      x = ix + iw + 6;
      if (tw > 0)
        {
          ops_text(o, f, x, y + (h - text_height(f)) / 2, label, fg, alpha);
        }
    }
  else if (label != NULL && label[0] != '\0')
    {
      ops_text_centre(o, f, x + w / 2, y + (h - text_height(f)) / 2, label,
                      fg, alpha);
    }
}

void kit_paint_check(struct desk_ops_s *o, int x, int y, bool on, int look)
{
  int state = look & KIT_LOOK_MASK;
  uint8_t a = state == KIT_LOOK_DISABLED ? 110 : 255;

  if (!on)
    {
      ops_rrect(o, x, y, KIT_CHECK, KIT_CHECK, LUT_R3, 3,
                state == KIT_LOOK_HOT ? g_theme.text_dim : g_theme.text_faint, a);
      ops_rrect(o, x + 1, y + 1, KIT_CHECK - 2, KIT_CHECK - 2, LUT_R3, 3,
                state == KIT_LOOK_DOWN ? g_theme.control_pressed :
                state == KIT_LOOK_HOT ? g_theme.control_hover : g_theme.field, 255);
      return;
    }

  ops_rrect(o, x, y, KIT_CHECK, KIT_CHECK, LUT_R3, 3,
            state == KIT_LOOK_DOWN ? darker(g_theme.accent) :
            state == KIT_LOOK_HOT ? lighter(g_theme.accent) : g_theme.accent, a);
  kit_paint_icon(o, KIT_ICON_CHECK, x + 1, y + 1, g_theme.accent_text, 0);
}

void kit_paint_radio(struct desk_ops_s *o, int x, int y, bool on, int look)
{
  int state = look & KIT_LOOK_MASK;
  uint8_t a = state == KIT_LOOK_DISABLED ? 110 : 255;
  int cx = x + KIT_RADIO / 2;
  int cy = y + KIT_RADIO / 2;
  int r = KIT_RADIO / 2;

  /* A disc of the kit's radio size: the round rectangle of the nearest
   * table, as round as it will go
   */

  if (!on)
    {
      ops_rrect(o, cx - r, cy - r, 2 * r, 2 * r, LUT_R6, 6,
                state == KIT_LOOK_HOT ? g_theme.text_dim : g_theme.text_faint, a);
      ops_rrect(o, cx - r + 1, cy - r + 1, 2 * r - 2, 2 * r - 2, LUT_R6, 6,
                state == KIT_LOOK_DOWN ? g_theme.control_pressed :
                state == KIT_LOOK_HOT ? g_theme.control_hover : g_theme.field, 255);
      return;
    }

  ops_rrect(o, cx - r, cy - r, 2 * r, 2 * r, LUT_R6, 6,
            state == KIT_LOOK_DOWN ? darker(g_theme.accent) :
            state == KIT_LOOK_HOT ? lighter(g_theme.accent) : g_theme.accent, a);
  ops_disc(o, cx, cy, LUT_R3, 3, g_theme.accent_text, 255);
}

void kit_paint_switch(struct desk_ops_s *o, int x, int y, bool on,
                      int look)
{
  int state = look & KIT_LOOK_MASK;
  uint16_t track = on ? g_theme.accent : state == KIT_LOOK_HOT ?
                   g_theme.control_hover : g_theme.control;
  int kx = on ? x + KIT_SWITCH_W - 14 : x + 14;
  uint8_t a = state == KIT_LOOK_DISABLED ? 110 : 255;

  if (on && state == KIT_LOOK_HOT)
    {
      track = lighter(track);
    }

  if (state == KIT_LOOK_DOWN)
    {
      track = darker(track);
    }

  ops_rrect(o, x, y, KIT_SWITCH_W, KIT_SWITCH_H, LUT_R12, 12, track, a);
  ops_disc(o, kx, y + KIT_SWITCH_H / 2 + 1, LUT_R12, 12, C_BLACK, 40);
  ops_disc(o, kx, y + KIT_SWITCH_H / 2, LUT_R12, 12,
           state == KIT_LOOK_DISABLED ? g_theme.text_dim : C_WHITE, 255);
}

void kit_paint_slider(struct desk_ops_s *o, int x, int y, int w, int v,
                      int min, int max, int look)
{
  int state = look & KIT_LOOK_MASK;
  int r = KIT_SLIDER_H / 2;
  int span = w - 2 * r;
  int kx;
  int ty = y + KIT_SLIDER_H / 2 - 3;

  v  = v < min ? min : v > max ? max : v;
  kx = x + r + (max > min ? (v - min) * span / (max - min) : 0);
  ops_rrect(o, x + r - 3, ty, span + 6, 6, LUT_R3, 3, g_theme.control_hover,
            255);
  ops_rrect(o, x + r - 3, ty, kx - x - r + 6, 6, LUT_R3, 3,
            state == KIT_LOOK_DISABLED ? g_theme.text_faint : g_theme.accent, 255);
  ops_disc(o, kx, y + r + 1, LUT_R12, 12, C_BLACK,
           state == KIT_LOOK_HOT || state == KIT_LOOK_DOWN ? 70 : 45);
  ops_disc(o, kx, y + r, LUT_R12, 12,
           state == KIT_LOOK_DISABLED ? g_theme.text_dim :
           state == KIT_LOOK_DOWN ? RGB(0xe0, 0xe0, 0xe4) : C_WHITE, 255);
}

int kit_slider_value(int x, int w, int min, int max, int px)
{
  int r = KIT_SLIDER_H / 2;
  int span = w - 2 * r;
  int v;

  if (span <= 0)
    {
      return min;
    }

  v = min + ((px - x - r) * (max - min) + span / 2) / span;
  return v < min ? min : v > max ? max : v;
}

void kit_paint_progress(struct desk_ops_s *o, int x, int y, int w, int v,
                        int min, int max)
{
  int fw;

  v  = v < min ? min : v > max ? max : v;
  fw = max > min ? (v - min) * w / (max - min) : 0;
  ops_rrect(o, x, y, w, KIT_PROGRESS_H, LUT_R3, 3, g_theme.control_hover, 255);
  if (fw > 0)
    {
      ops_rrect(o, x, y, fw < 6 ? 6 : fw, KIT_PROGRESS_H, LUT_R3, 3,
                g_theme.accent, 255);
    }
}

/* A text field: where its text starts so that the cursor shows */

static int field_from(struct ui_field_s *f, int room)
{
  const struct desk_font_s *font = KIT_FONT;
  int from = 0;
  int i;

  for (; ; )
    {
      int cw = 0;

      for (i = from; i < f->cur; i++)
        {
          char c[2] =
            {
              f->text[i], '\0'
            };

          cw += text_width(font, c);
        }

      if (cw <= room || from >= f->cur)
        {
          return from;
        }

      from++;
    }
}

void kit_paint_field(struct desk_ops_s *o, struct ui_field_s *f, int x,
                     int y, int w, int h, const char *placeholder,
                     bool focus, bool caret, bool disabled, bool selected)
{
  const struct desk_font_s *font = KIT_FONT;
  int room = w - 16;
  int tx = x + 8;
  int ty = y + (h - text_height(font)) / 2;
  int cx = tx;
  int start;
  int from;
  int i;

  ops_rrect(o, x, y, w, h, LUT_R6, 6, focus ? g_theme.focus : g_theme.control_hover,
            255);
  ops_rrect(o, x + 1, y + 1, w - 2, h - 2, LUT_R6, 5, g_theme.field, 255);
  if (focus)
    {
      /* A second pixel of the ring, inside */

      ops_rrect(o, x + 1, y + 1, w - 2, h - 2, LUT_R6, 5, g_theme.focus, 90);
      ops_rrect(o, x + 2, y + 2, w - 4, h - 4, LUT_R6, 4, g_theme.field, 255);
    }

  start = o->n;
  if (f->len == 0 && placeholder != NULL)
    {
      ops_text(o, font, tx, ty, placeholder, g_theme.text_faint, 255);
    }

  from = field_from(f, room);
  if (selected && f->len > 0)
    {
      int sw = text_width(font, f->text + from);

      ops_rrect(o, tx - 2, y + 5, (sw < room ? sw : room) + 4, h - 10,
                LUT_R3, 3, g_theme.accent, focus ? THEMED(110, 70) : 50);
    }

  for (i = from; i < f->len; i++)
    {
      char c[2] =
        {
          f->text[i], '\0'
        };

      if (i == f->cur)
        {
          cx = tx;
        }

      if (tx - (x + 8) > room)
        {
          break;
        }

      tx += ops_text(o, font, tx, ty, c,
                     disabled ? g_theme.text_dim : g_theme.text, 255);
    }

  if (f->cur >= i)
    {
      cx = tx;
    }

  ui_clip(o, start, x + 4, y, x + w - 4, y + h);
  if (focus && caret && !disabled)
    {
      ops_fill(o, cx, y + 6, 1, h - 12, g_theme.text);
    }
}

/* The position in a field of width w at x nearest px */

int kit_field_pos(struct ui_field_s *f, int x, int w, int px)
{
  const struct desk_font_s *font = KIT_FONT;
  int tx = x + 8;
  int i;

  for (i = field_from(f, w - 16); i < f->len; i++)
    {
      char c[2] =
        {
          f->text[i], '\0'
        };

      int cw = text_width(font, c);

      if (px < tx + cw / 2)
        {
          return i;
        }

      tx += cw;
    }

  return f->len;
}

void kit_paint_dropdown(struct desk_ops_s *o, int x, int y, int w, int h,
                        const char *label, int look)
{
  const struct desk_font_s *f = KIT_FONT;
  int state = look & KIT_LOOK_MASK;
  char fit[64];

  kit_paint_button(o, x, y, w, h, NULL, KIT_ICON_NONE, KIT_BTN_NORMAL, look,
                   false);
  if (label != NULL)
    {
      ui_fit(f, label, w - 40, fit, sizeof(fit));
      ops_text(o, f, x + 10, y + (h - text_height(f)) / 2, fit,
               state == KIT_LOOK_DISABLED ? g_theme.text_dim : g_theme.control_text,
               state == KIT_LOOK_DISABLED ? 160 : 255);
    }

  ops_fill(o, x + w - 28, y + 6, 1, h - 12, g_theme.window_bg);
  kit_paint_icon(o, KIT_ICON_CHEVRON, x + w - 22, y + (h - KIT_ICON) / 2,
                 state == KIT_LOOK_DISABLED ? g_theme.text_faint : g_theme.text_dim,
                 0);
}

/* A scroll bar's thumb, in the bar at x (KIT_SB_W wide) */

void kit_paint_thumb(struct desk_ops_s *o, int x, int y, int h, bool hot,
                     bool drag)
{
  ops_rrect(o, x + (hot || drag ? 2 : 3), y + 2, hot || drag ? 8 : 6, h - 4,
            LUT_R3, 3, g_theme.thumb, drag ? 255 : hot ? 220 : 170);
}

int kit_tooltip_size(const char *text, int *h)
{
  *h = text_height(KIT_FONT_SMALL) + 10;
  return text_width(KIT_FONT_SMALL, text) + 16;
}

void kit_paint_tooltip(struct desk_ops_s *o, int x, int y, int w, int h,
                       const char *text)
{
  ops_rrect(o, x, y, w, h, LUT_R6, 6, g_theme.separator, 255);
  ops_rrect(o, x + 1, y + 1, w - 2, h - 2, LUT_R6, 5, g_theme.popup, 255);
  ops_text(o, KIT_FONT_SMALL, x + 8, y + 5, text, g_theme.popup_text, 255);
}

/* A stepper: [-] value [+] across w; hot: -1 minus, 1 plus, 0 none */

void kit_paint_stepper(struct desk_ops_s *o, int x, int y, int w,
                       const char *value, int hot)
{
  const struct desk_font_s *f = KIT_FONT;
  int ty = y + (KIT_STEP - text_height(f)) / 2;

  ops_rrect(o, x, y, KIT_STEP, KIT_STEP, LUT_R6, 6,
            hot < 0 ? g_theme.control_hover : g_theme.control, 255);
  ops_rrect(o, x + w - KIT_STEP, y, KIT_STEP, KIT_STEP, LUT_R6, 6,
            hot > 0 ? g_theme.control_hover : g_theme.control, 255);
  ops_text_centre(o, KIT_FONT_BOLD, x + KIT_STEP / 2, ty - 1,
                  DESK_CH_MINUS, g_theme.control_text, 255);
  ops_text_centre(o, KIT_FONT_BOLD, x + w - KIT_STEP / 2, ty - 1, "+",
                  g_theme.control_text, 255);
  if (value != NULL)
    {
      ops_text_centre(o, KIT_FONT_BOLD, x + w / 2, ty, value, g_theme.text,
                      255);
    }
}

int kit_stepper_at(int x, int w, int px)
{
  return px >= x && px < x + KIT_STEP ? -1 :
         px >= x + w - KIT_STEP && px < x + w ? 1 : 0;
}

/* A colour swatch centred at (cx, cy), a dot in it when chosen */

void kit_paint_swatch(struct desk_ops_s *o, int cx, int cy, uint16_t c,
                      bool sel, bool hot)
{
  ops_disc(o, cx, cy + 1, LUT_R12, KIT_SWATCH_R, C_BLACK, hot ? 60 : 30);
  ops_disc(o, cx, cy, LUT_R12, KIT_SWATCH_R, c, 255);
  if (sel)
    {
      ops_disc(o, cx, cy, LUT_R3, 3, C_WHITE, 255);
    }
}

/* A segmented control of n labels across w: sel lit, hot under the
 * pointer (pressed: held down there), -1: none
 */

void kit_paint_segments(struct desk_ops_s *o, int x, int y, int w, int h,
                        const char * const *labels, int n, int sel,
                        int hot, bool pressed, bool disabled)
{
  const struct desk_font_s *f = KIT_FONT;
  int i;

  ops_rrect(o, x, y, w, h, LUT_R6, 6, g_theme.control, 255);
  for (i = 0; i < n; i++)
    {
      int sx = x + w * i / n;
      int sw = x + w * (i + 1) / n - sx;
      uint16_t fg = g_theme.control_text;

      if (i == sel)
        {
          ops_rrect(o, sx + 2, y + 2, sw - 4, h - 4, LUT_R6, 5,
                    disabled ? g_theme.text_faint : g_theme.accent, 255);
          fg = g_theme.accent_text;
        }
      else if (i == hot && !disabled)
        {
          ops_rrect(o, sx + 2, y + 2, sw - 4, h - 4, LUT_R6, 5,
                    pressed ? g_theme.control_pressed :
                    g_theme.control_hover, 255);
        }
      else if (i > 0 && i - 1 != sel && i - 1 != hot)
        {
          ops_blend(o, sx, y + 7, 1, h - 14, g_theme.text, 50);
        }

      ops_text_centre(o, i == sel ? KIT_FONT_BOLD : f, sx + sw / 2,
                      y + (h - text_height(f)) / 2, labels[i],
                      disabled && i != sel ? g_theme.text_dim : fg, 255);
    }
}

int kit_segment_at(int x, int w, int n, int px)
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

/* A card: round, the theme's card colour (on a light theme a hairline
 * shows where it ends); its rows' hairlines
 */

void kit_paint_card(struct desk_ops_s *o, int x, int y, int w, int h)
{
  if (g_theme.dark)
    {
      ops_rrect(o, x, y, w, h, LUT_R12, 12, g_theme.card, 255);
    }
  else
    {
      ops_rrect(o, x, y, w, h, LUT_R12, 12, RGB(0xde, 0xde, 0xe3), 255);
      ops_rrect(o, x + 1, y + 1, w - 2, h - 2, LUT_R12, 11, g_theme.card,
                255);
    }
}

void kit_paint_card_lines(struct desk_ops_s *o, int x, int y, int w)
{
  ops_fill(o, x + 14, y, w - 28, 1,
           THEMED(RGB(0x3c, 0x3c, 0x42), RGB(0xe6, 0xe6, 0xea)));
}

/* A row's label (and a caption under it, or NULL), h high, at the left */

void kit_paint_row_label(struct desk_ops_s *o, int x, int y, int h,
                         const char *label, const char *caption)
{
  const struct desk_font_s *f = KIT_FONT;

  if (caption == NULL)
    {
      ops_text(o, f, x + 14, y + (h - text_height(f)) / 2, label,
               g_theme.text, 255);
    }
  else
    {
      int th = text_height(f) + 1 + text_height(KIT_FONT_SMALL);
      int ty = y + (h - th) / 2;

      ops_text(o, f, x + 14, ty, label, g_theme.text, 255);
      ops_text(o, KIT_FONT_SMALL, x + 14, ty + text_height(f) + 1, caption,
               g_theme.text_dim, 255);
    }
}

/* A row of a sidebar: a small coloured tile, the label */

void kit_paint_sidebar_row(struct desk_ops_s *o, int x, int y, int w,
                           int h, const char *label, uint16_t tile,
                           bool sel, bool hot)
{
  const struct desk_font_s *f = KIT_FONT;

  if (sel)
    {
      ops_rrect(o, x, y, w, h, LUT_R6, 6, g_theme.accent, 255);
    }
  else if (hot)
    {
      ops_rrect(o, x, y, w, h, LUT_R6, 6, g_theme.hilite, THEMED(20, 14));
    }

  ops_rrect(o, x + 8, y + (h - 20) / 2, 20, 20, LUT_R6, 6, tile, 255);
  ops_blend(o, x + 12, y + (h - 20) / 2, 12, 1, C_WHITE, 70);
  ops_text(o, sel ? KIT_FONT_BOLD : f, x + 38, y + (h - text_height(f)) / 2,
           label, sel ? g_theme.accent_text : g_theme.text, 255);
}
