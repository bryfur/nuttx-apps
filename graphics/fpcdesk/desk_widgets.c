/****************************************************************************
 * apps/graphics/fpcdesk/desk_widgets.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small controls (desk_widgets.h): a slider, a switch, a level meter and a
 * speaker, drawn with GPU operations (round shapes are BLENDs through the
 * desktop's disc tables).
 ****************************************************************************/

#include <nuttx/config.h>

#include "desk_widgets.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KNOB_R          6               /* The slider's knob (LUT_R6) */
#define TRACK_H         6               /* Its track (LUT_R3) */

#define C_TRACK         RGB(0x60, 0x60, 0x68)
#define C_METER_OK      RGB(0x34, 0xc7, 0x59)
#define C_METER_HOT     RGB(0xff, 0xb3, 0x40)
#define C_METER_CLIP    RGB(0xff, 0x45, 0x3a)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Where the knob's centre is for the slider's value */

static int slider_kx(const struct wdg_slider_s *s)
{
  return s->x + KNOB_R + (s->w - 2 * KNOB_R) * s->value / WDG_SLIDER_MAX;
}

/* The value for a pointer at x */

static int slider_at(const struct wdg_slider_s *s, int x)
{
  int span = s->w - 2 * KNOB_R;
  int v = span > 0 ? (x - s->x - KNOB_R) * WDG_SLIDER_MAX / span : 0;

  return v < 0 ? 0 : v > WDG_SLIDER_MAX ? WDG_SLIDER_MAX : v;
}

/* 256 x log2(v) for v >= 1 (the fraction from the next bits, linearly) */

static int log2_256(uint32_t v)
{
  int e = 0;

  while (v >= 512)
    {
      v >>= 1;
      e++;
    }

  while (v < 256)
    {
      v <<= 1;
      e--;
    }

  return (e + 8) * 256 + (int)(v - 256);
}

/****************************************************************************
 * Public Functions: the slider
 ****************************************************************************/

void wdg_slider_draw(struct desk_ops_s *o, const struct wdg_slider_s *s)
{
  int cy = s->y + WDG_SLIDER_H / 2;
  int kx = slider_kx(s);
  uint16_t fill = s->dim ? C_TEXT_DIM : C_ACCENT;

  ops_rrect(o, s->x, cy - TRACK_H / 2, s->w, TRACK_H, LUT_R3, 3, C_TRACK,
            255);
  if (kx - s->x > 0)
    {
      ops_rrect(o, s->x, cy - TRACK_H / 2, kx - s->x + 3, TRACK_H, LUT_R3, 3,
                fill, 255);
    }

  /* The knob: a halo when the pointer is on it, a soft shadow, white */

  if (s->hot || s->drag)
    {
      ops_disc(o, kx, cy, LUT_R12, 12, C_ACCENT, s->drag ? 70 : 45);
    }

  ops_disc(o, kx, cy + 1, LUT_R6, KNOB_R, C_BLACK, 90);
  ops_disc(o, kx, cy, LUT_R6, KNOB_R, s->dim ? C_TEXT_DIM : C_WHITE, 255);
}

bool wdg_slider_over(const struct wdg_slider_s *s, int x, int y)
{
  return x >= s->x - 4 && x < s->x + s->w + 4 && y >= s->y &&
         y < s->y + WDG_SLIDER_H;
}

bool wdg_slider_mouse(struct wdg_slider_s *s, int x, int y, uint8_t buttons,
                      uint8_t last)
{
  bool down = (buttons & NX_MOUSE_LEFTBUTTON) != 0;
  bool press = down && (last & NX_MOUSE_LEFTBUTTON) == 0;
  int v = s->value;

  s->hot = wdg_slider_over(s, x, y);
  if (press && s->hot)
    {
      s->drag = true;
    }
  else if (!down)
    {
      s->drag = false;
    }

  if (s->drag)
    {
      s->value = slider_at(s, x);
    }

  return s->value != v;
}

/****************************************************************************
 * Public Functions: the switch
 ****************************************************************************/

void wdg_switch_draw(struct desk_ops_s *o, const struct wdg_switch_s *s)
{
  int kx = s->on ? s->x + WDG_SWITCH_W - 3 - 18 : s->x + 3;

  ops_rrect(o, s->x, s->y, WDG_SWITCH_W, WDG_SWITCH_H, LUT_R12, 12,
            s->on ? C_ACCENT : C_TRACK, 255);
  if (s->hot)
    {
      ops_rrect(o, s->x, s->y, WDG_SWITCH_W, WDG_SWITCH_H, LUT_R12, 12,
                C_WHITE, 30);
    }

  ops_rrect(o, kx, s->y + 4, 18, 18, LUT_R6, 6, C_BLACK, 70);
  ops_rrect(o, kx, s->y + 3, 18, 18, LUT_R6, 6, C_WHITE, 255);
}

bool wdg_switch_mouse(struct wdg_switch_s *s, int x, int y, uint8_t buttons,
                      uint8_t last)
{
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) != 0 &&
               (last & NX_MOUSE_LEFTBUTTON) == 0;

  s->hot = x >= s->x && x < s->x + WDG_SWITCH_W && y >= s->y &&
           y < s->y + WDG_SWITCH_H;
  if (press && s->hot)
    {
      s->on = !s->on;
      return true;
    }

  return false;
}

/****************************************************************************
 * Public Functions: the meter and the speaker
 ****************************************************************************/

void wdg_meter(struct desk_ops_s *o, int x, int y, int w, int h, int peak,
               uint8_t alpha)
{
  int lit = 0;
  int ok = w * 36 / 48;                 /* -12 dB */
  int hot = w * 45 / 48;                /* -3 dB */

  ops_rrect(o, x, y, w, h, LUT_R3, h / 2 < 3 ? h / 2 : 3, C_TRACK, alpha);

  if (peak > 0)
    {
      /* dB = 6.02 log2(peak / 32767): the bar is 48 dB long */

      int db256 = (log2_256(peak) - log2_256(32767)) * 602 / 100;

      lit = w + db256 * w / (48 * 256);
      lit = lit < 0 ? 0 : lit > w ? w : lit;
    }

  if (lit > 0)
    {
      ops_blend(o, x, y, lit < ok ? lit : ok, h, C_METER_OK, alpha);
    }

  if (lit > ok)
    {
      ops_blend(o, x + ok, y, (lit < hot ? lit : hot) - ok, h, C_METER_HOT,
                alpha);
    }

  if (lit > hot)
    {
      ops_blend(o, x + hot, y, lit - hot, h, C_METER_CLIP, alpha);
    }
}

int wdg_speaker_waves(int volume)
{
  return volume <= 0 ? 0 : volume < 400 ? 1 : volume < 750 ? 2 : 3;
}

int wdg_speaker(struct desk_ops_s *o, int x, int y, int s, int waves,
                bool muted, uint16_t c, uint8_t alpha)
{
  static const struct desk_font_s * const wave[3] =
  {
    &g_desk_font_small, &g_desk_font_ui, &g_desk_font_title
  };

  int mw = s * 3 / 10;                  /* The magnet */
  int mh = s * 4 / 10;
  int cw = s * 3 / 10;                  /* The cone */
  int cy = y + s / 2;
  int wx;
  int i;

  ops_blend(o, x, cy - mh / 2, mw, mh, c, alpha);
  for (i = 0; i < cw; i++)
    {
      int h = mh + (s * 9 / 10 - mh) * (i + 1) / cw;

      ops_blend(o, x + mw + i, cy - h / 2, 1, h, c, alpha);
    }

  wx = x + mw + cw + s / 10;
  if (muted)
    {
      const struct desk_font_s *f = s >= 20 ? &g_desk_font_title :
                                              &g_desk_font_uib;

      ops_text(o, f, wx, cy - text_height(f) / 2 - 1, DESK_CH_TIMES, c,
               alpha);
      return wx - x + text_width(f, DESK_CH_TIMES);
    }

  /* The waves: closing parentheses, bigger and further out */

  for (i = 0; i < waves && i < 3; i++)
    {
      const struct desk_font_s *f = wave[s >= 20 && i < 2 ? i + 1 : i];
      int gx = wx + i * s / 5;

      ops_text(o, f, gx, cy - f->ascent * 6 / 10 - 1, ")", c,
               (uint8_t)(alpha * (255 - i * 40) / 255));
    }

  return wx - x + 3 * s / 5;
}
