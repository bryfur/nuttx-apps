/****************************************************************************
 * apps/graphics/fpcdesk/desk_widgets.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Controls drawn into a list of operations, in the theme's colours:
 * switches, segmented controls, sliders, check boxes, steppers, colour
 * swatches, sidebar rows, cards with rows of settings.  Each is a drawing
 * function and, where a control has parts, a function that says which
 * part a point is on; the caller keeps where it put them and what they
 * mean (Settings, the System Log's toolbar).
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H
#define __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H

#include "desk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WG_SWITCH_W     48
#define WG_SWITCH_H     28
#define WG_SLIDER_H     24              /* The knob's size */
#define WG_CHECK        18
#define WG_STEP         24              /* A stepper's buttons */
#define WG_SWATCH_R     12
#define WG_ROW_H        44              /* A row of a card */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* A switch, WG_SWITCH_W x WG_SWITCH_H */

void wg_switch(struct desk_ops_s *o, int x, int y, bool on, bool hot);

/* A segmented control of n labels across w; sel lit, hot under the
 * pointer (-1: none); which segment x is on
 */

void wg_segments(struct desk_ops_s *o, int x, int y, int w, int h,
                 const char * const *labels, int n, int sel, int hot);
int  wg_segment_at(int x, int w, int n, int px);

/* A slider of width w for v in [min, max]; the value at px */

void wg_slider(struct desk_ops_s *o, int x, int y, int w, int v, int min,
               int max, bool hot);
int  wg_slider_value(int x, int w, int min, int max, int px);

/* A check box, WG_CHECK square */

void wg_check(struct desk_ops_s *o, int x, int y, bool on, bool hot);

/* A stepper: [-] value [+] across w; hot: -1 minus, 1 plus, 0 none; which
 * button px is on (-1, 1, or 0)
 */

void wg_stepper(struct desk_ops_s *o, int x, int y, int w,
                const char *value, int hot);
int  wg_stepper_at(int x, int w, int px);

/* A colour swatch centred at (cx, cy), a dot in it when selected */

void wg_swatch(struct desk_ops_s *o, int cx, int cy, uint16_t c, bool sel,
               bool hot);

/* A row of a sidebar: a small coloured tile, the label */

void wg_sidebar_row(struct desk_ops_s *o, int x, int y, int w, int h,
                    const char *label, uint16_t tile, bool sel, bool hot);

/* A card of nrows rows of WG_ROW_H, hairlines between them; a row's label
 * (and a caption under it, or NULL)
 */

void wg_card(struct desk_ops_s *o, int x, int y, int w, int nrows);
void wg_row_label(struct desk_ops_s *o, int x, int y, const char *label,
                  const char *caption);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_WIDGETS_H */
