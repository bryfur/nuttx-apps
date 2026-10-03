/****************************************************************************
 * apps/graphics/fpcdesk/desk_widgets.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Short names for the widget kit's painters (desk_kit.h), for code that
 * draws its own controls (the volume popover, Music).  New code uses the
 * kit itself.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H
#define __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H

#include "desk_kit.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WG_SLIDER_H     KIT_SLIDER_H

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

static inline int wg_look(bool hot)
{
  return hot ? KIT_LOOK_HOT : KIT_LOOK_NORMAL;
}

static inline void wg_slider(struct desk_ops_s *o, int x, int y, int w,
                             int v, int min, int max, bool hot)
{
  kit_paint_slider(o, x, y, w, v, min, max, wg_look(hot));
}

static inline int wg_slider_value(int x, int w, int min, int max, int px)
{
  return kit_slider_value(x, w, min, max, px);
}

/* A level meter: peak 0..32767 on a 48 dB scale */

static inline void wg_meter(struct desk_ops_s *o, int x, int y, int w,
                            int h, int peak)
{
  kit_paint_meter(o, x, y, w, h, peak);
}

/* A speaker icon s high with 0..3 waves (or a cross): its width.
 * wg_speaker_waves(): the waves for a volume of 0..1000.
 */

static inline int wg_speaker(struct desk_ops_s *o, int x, int y, int s,
                             int waves, bool muted, uint16_t c,
                             uint8_t alpha)
{
  return kit_paint_speaker(o, x, y, s, waves, muted, c, alpha);
}

static inline int wg_speaker_waves(int volume)
{
  return kit_speaker_waves(volume);
}

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H */
