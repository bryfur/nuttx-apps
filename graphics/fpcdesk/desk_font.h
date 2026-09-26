/****************************************************************************
 * apps/graphics/fpcdesk/desk_font.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Anti-aliased fonts (desk_fonts.c, made by tools/mkfont.c from Roboto,
 * Apache-2.0, and IBM Plex Mono, OFL-1.1): characters 32-126, then the
 * DESK_CH_* extras.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_FONT_H
#define __APPS_GRAPHICS_FPCDESK_DESK_FONT_H

#include <stdint.h>
#include <stddef.h>

#define DESK_FONT_FIRST   32
#define DESK_CH_BULLET    "\x7f"
#define DESK_CH_TIMES     "\x80"
#define DESK_CH_MINUS     "\x81"
#define DESK_CH_DEGREE    "\x82"
#define DESK_CH_ELLIPSIS  "\x83"
#define DESK_CH_TRIANGLE  "\x84"
#define DESK_CH_MIDDOT    "\x85"

struct desk_glyph_s
{
  uint8_t  w;                     /* Bitmap size */
  uint8_t  h;
  int8_t   bx;                    /* Bitmap left of the pen */
  int8_t   by;                    /* Bitmap top above the baseline */
  uint8_t  adv;                   /* Advance */
  uint32_t off;                   /* Offset of the bitmap in data */
};

struct desk_font_s
{
  uint8_t  size;                  /* Pixels */
  uint8_t  ascent;
  uint8_t  descent;
  uint8_t  nglyphs;
  const struct desk_glyph_s *glyphs;
  const uint8_t *data;            /* Coverage, 0-255 */
};

extern const struct desk_font_s g_desk_font_ui;      /* Regular 13 */
extern const struct desk_font_s g_desk_font_uib;     /* Medium 13 */
extern const struct desk_font_s g_desk_font_small;   /* Regular 11 */
extern const struct desk_font_s g_desk_font_title;   /* Medium 20 */
extern const struct desk_font_s g_desk_font_big;     /* Light 44 */
extern const struct desk_font_s g_desk_font_mono;    /* IBM Plex Mono 15 */
extern const struct desk_font_s g_desk_font_monob;   /* ... SemiBold 15 */

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_FONT_H */
