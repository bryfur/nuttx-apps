/****************************************************************************
 * apps/graphics/fpcgame/tri/tri_fpc3d.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FPC's extension of TRI's SDK (tri_fpc3d.h): sprites turned in 3D or
 * drawn as any quadrilateral, and a layer drawn as a Mode 7 floor.  The
 * calls become command words in the processors' lists, as TRI's own do
 * (op codes TRI leaves unused); tri_fpc.c draws them.
 ****************************************************************************/

#include <stdint.h>

#include "tri_sdk_internal.h"
#include "tri_fpc3d.h"

static inline uint32_t pack(int lo, int hi)
{
  return ((uint32_t)lo & 0xffff) | ((uint32_t)hi << 16);
}

static bool sprite_ok(int sheet, int pal, int w, int h, const char *fn)
{
  return tri_check(sheet >= 0 && sheet < 16 && pal >= 0 && pal < 16, fn,
                   "sheet or palette out of range") &&
         tri_check(w > 0 && h > 0 && w <= 0x7fff && h <= 0x7fff, fn,
                   "bad cell size");
}

/* TRI_SPR_ATTR, its blend left out (the textured rows have none) */

static uint32_t attr(int sheet, int pal, unsigned flags)
{
  return TRI_SPR_ATTR(sheet, pal, (flags & TRI_SPRITE_HFLIP) ? 1 : 0,
                      (flags & TRI_SPRITE_VFLIP) ? 1 : 0, TRI_BLEND_NONE);
}

void tri_sprite_draw_3d(int cx, int cy, int sheet, int sx, int sy, int w,
                        int h, int pal, unsigned flags, int yaw, int pitch,
                        int roll, int scale)
{
  uint32_t c[7];

  if (!sprite_ok(sheet, pal, w, h, "tri_sprite_draw_3d"))
    {
      return;
    }

  scale = tri_clamp(scale, 1, 0x7fff);
  c[0] = TRI_HDR(TRI_OP_FPC_SPRITE_3D, 6, 0);
  c[1] = pack(cx, cy);
  c[2] = pack(sx, sy);
  c[3] = pack(w, h);
  c[4] = attr(sheet, pal, flags) |
         ((flags & TRI_SPRITE_BACK_MASK) >> 4) << 12;
  c[5] = pack(yaw & 4095, pitch & 4095);
  c[6] = pack(roll & 4095, scale);
  tri_emit(TRI_DEST_VDP1, c, 7);
}

void tri_sprite_draw_quad(const int x[4], const int y[4], int sheet, int sx,
                          int sy, int w, int h, int pal, unsigned flags)
{
  uint32_t c[9];
  int k;

  if (!sprite_ok(sheet, pal, w, h, "tri_sprite_draw_quad"))
    {
      return;
    }

  c[0] = TRI_HDR(TRI_OP_FPC_QUAD, 8, 0);
  c[1] = 0;
  c[2] = pack(sx, sy);
  c[3] = pack(w, h);
  c[4] = attr(sheet, pal, flags);
  for (k = 0; k < 4; k++)
    {
      c[5 + k] = pack(tri_clamp(x[k], -32768, 32767),
                      tri_clamp(y[k], -32768, 32767));
    }

  tri_emit(TRI_DEST_VDP1, c, 9);
}

void tri_layer_mode7(int layer, int top, int bottom, int32_t camx,
                     int32_t camy, int angle, int height, int horizon,
                     int focal)
{
  uint32_t c[6];

  if (!tri_check(layer >= 0 && layer < 4, "tri_layer_mode7",
                 "layer must be 0..3 (4 is the overlay's)") ||
      !tri_check(height > 0 && height <= 0x7fff && focal > 0 &&
                 focal <= 0x7fff, "tri_layer_mode7",
                 "height and focal must be 1..32767"))
    {
      return;
    }

  c[0] = TRI_HDR(TRI_OP_FPC_LAYER_MODE7, 5, layer);
  c[1] = pack(tri_clamp(top, -32768, 32767), tri_clamp(bottom, -32768, 32767));
  c[2] = (uint32_t)camx;
  c[3] = (uint32_t)camy;
  c[4] = pack(angle & 4095, height);
  c[5] = pack(tri_clamp(horizon, -32768, 32767), focal);
  tri_emit(TRI_DEST_VDP2, c, 6);
}

void tri_layer_flat(int layer)
{
  uint32_t c;

  if (!tri_check(layer >= 0 && layer < 4, "tri_layer_flat",
                 "layer must be 0..3 (4 is the overlay's)"))
    {
      return;
    }

  c = TRI_HDR(TRI_OP_FPC_LAYER_MODE7, 0, layer);
  tri_emit(TRI_DEST_VDP2, &c, 1);
}
