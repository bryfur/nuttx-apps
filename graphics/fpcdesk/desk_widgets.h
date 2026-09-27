/****************************************************************************
 * apps/graphics/fpcdesk/desk_widgets.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The first controls of Settings (switches, segmented controls, sliders,
 * check boxes, steppers, colour swatches, sidebar rows, cards of rows),
 * kept by these names for code that draws them itself.  They are the
 * widget kit's (desk_kit.h, kit_paint_*()): new code builds its window
 * with the kit instead, which also finds the pointer, moves the focus and
 * draws only what changed.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H
#define __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H

#include "desk_kit.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WG_SWITCH_W     KIT_SWITCH_W
#define WG_SWITCH_H     KIT_SWITCH_H
#define WG_SLIDER_H     KIT_SLIDER_H
#define WG_CHECK        KIT_CHECK
#define WG_STEP         KIT_STEP
#define WG_SWATCH_R     KIT_SWATCH_R
#define WG_ROW_H        KIT_CARD_ROW_H

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

static inline int wg_look(bool hot)
{
  return hot ? KIT_LOOK_HOT : KIT_LOOK_NORMAL;
}

static inline void wg_switch(struct desk_ops_s *o, int x, int y, bool on,
                             bool hot)
{
  kit_paint_switch(o, x, y, on, wg_look(hot));
}

static inline void wg_segments(struct desk_ops_s *o, int x, int y, int w,
                               int h, const char * const *labels, int n,
                               int sel, int hot)
{
  kit_paint_segments(o, x, y, w, h, labels, n, sel, hot, false, false);
}

static inline int wg_segment_at(int x, int w, int n, int px)
{
  return kit_segment_at(x, w, n, px);
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

static inline void wg_check(struct desk_ops_s *o, int x, int y, bool on,
                            bool hot)
{
  kit_paint_check(o, x, y, on, wg_look(hot));
}

static inline void wg_stepper(struct desk_ops_s *o, int x, int y, int w,
                              const char *value, int hot)
{
  kit_paint_stepper(o, x, y, w, value, hot);
}

static inline int wg_stepper_at(int x, int w, int px)
{
  return kit_stepper_at(x, w, px);
}

static inline void wg_swatch(struct desk_ops_s *o, int cx, int cy,
                             uint16_t c, bool sel, bool hot)
{
  kit_paint_swatch(o, cx, cy, c, sel, hot);
}

static inline void wg_sidebar_row(struct desk_ops_s *o, int x, int y, int w,
                                  int h, const char *label, uint16_t tile,
                                  bool sel, bool hot)
{
  kit_paint_sidebar_row(o, x, y, w, h, label, tile, sel, hot);
}

static inline void wg_card(struct desk_ops_s *o, int x, int y, int w,
                           int nrows)
{
  int i;

  kit_paint_card(o, x, y, w, nrows * WG_ROW_H);
  for (i = 1; i < nrows; i++)
    {
      kit_paint_card_lines(o, x, y + i * WG_ROW_H, w);
    }
}

static inline void wg_row_label(struct desk_ops_s *o, int x, int y,
                                const char *label, const char *caption)
{
  kit_paint_row_label(o, x, y, WG_ROW_H, label, caption);
}

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H */
