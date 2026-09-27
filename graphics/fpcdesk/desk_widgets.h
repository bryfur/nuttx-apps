/****************************************************************************
 * apps/graphics/fpcdesk/desk_widgets.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small controls drawn with GPU operations and driven by the pointer: a
 * slider, a switch, a level meter, a speaker icon.  Each is a struct the
 * caller places (its position is in the caller's coordinates) and a draw
 * and a mouse function; they know nothing of what they control.  (To go
 * into the shared widget toolkit when there is one.)
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H
#define __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H

#include "desk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WDG_SLIDER_H     20             /* A slider's height (the knob's) */
#define WDG_SLIDER_MAX   1000           /* Its values: 0..WDG_SLIDER_MAX */
#define WDG_SWITCH_W     44
#define WDG_SWITCH_H     24

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* A horizontal slider: a track across w at (x, y), WDG_SLIDER_H tall */

struct wdg_slider_s
{
  int16_t  x;
  int16_t  y;
  int16_t  w;
  uint16_t value;                       /* 0..WDG_SLIDER_MAX */
  bool     hot;                         /* The pointer is over it */
  bool     drag;                        /* Being dragged */
  bool     dim;                         /* Drawn faded (muted, disabled) */
};

/* An on/off switch, WDG_SWITCH_W x WDG_SWITCH_H at (x, y) */

struct wdg_switch_s
{
  int16_t  x;
  int16_t  y;
  bool     on;
  bool     hot;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* The slider.  wdg_slider_mouse() takes every pointer event (x, y in the
 * caller's coordinates) and the buttons of the one before; true when the
 * value has changed (a press on the track jumps there, a drag follows).
 */

void wdg_slider_draw(struct desk_ops_s *o, const struct wdg_slider_s *s);
bool wdg_slider_mouse(struct wdg_slider_s *s, int x, int y, uint8_t buttons,
                      uint8_t last);
bool wdg_slider_over(const struct wdg_slider_s *s, int x, int y);

/* The switch: true when a click has turned it over */

void wdg_switch_draw(struct desk_ops_s *o, const struct wdg_switch_s *s);
bool wdg_switch_mouse(struct wdg_switch_s *s, int x, int y, uint8_t buttons,
                      uint8_t last);

/* A level meter: a bar w x h whose lit part shows peak (0..32767) on a
 * 48 dB scale, green, then amber, then red near full scale
 */

void wdg_meter(struct desk_ops_s *o, int x, int y, int w, int h, int peak,
               uint8_t alpha);

/* A speaker of size s (its height) at (x, y): waves 0..3, or muted (a
 * cross); returns its width
 */

int  wdg_speaker(struct desk_ops_s *o, int x, int y, int s, int waves,
                 bool muted, uint16_t c, uint8_t alpha);

/* How many waves a volume (0..1000) gets */

int  wdg_speaker_waves(int volume);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H */
