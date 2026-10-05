// FPC: 3D effects for TRI games, from the FPC GPU's textured rows (fg_3d.c,
// docs/gpu.md TSPAN). Not part of TRI: an extension of its SDK that only
// the FPC's runtime has (tri_fpc3d.c records the calls, tri_fpc.c draws
// them). A game that should still build on stock TRI includes it only
// where it exists, and keeps its effects behind TRI_FPC3D:
//
//     #if defined(__has_include)
//     #if __has_include("tri_fpc3d.h")
//     #include "tri_fpc3d.h"          // defines TRI_FPC3D
//     #endif
//     #endif
//     ...
//     #ifdef TRI_FPC3D
//         tri_sprite_draw_3d(...);
//     #else
//         tri_sprite_draw(...);
//     #endif
//
// The sprites are objects like any other (VDP1's list): drawn in call order
// with the sprites, rectangles and affine sprites of the frame. The Mode 7
// layer is drawn where the layer is (its number and priority), as a floor
// in perspective instead of a flat scrolled map.
#pragma once
#include "tri_sdk.h"

#define TRI_FPC3D 1

// Command words (tri_isa.h's format), at the top of each processor's range,
// which TRI leaves unused:
//   SPRITE_3D: w0 = cx | cy << 16, w1 = sx | sy << 16, w2 = w | h << 16,
//              w3 = TRI_SPR_ATTR | back << 12, w4 = yaw | pitch << 16, w5 = roll | scale << 16
//   QUAD:      w0 = 0, w1 = sx | sy << 16, w2 = w | h << 16, w3 = TRI_SPR_ATTR,
//              w4..w7 = x | y << 16 of corners 0..3 (1/16 pixel, int16)
//   LAYER_MODE7 (imm = layer): w0 = top | bottom << 16, w1 = camx, w2 = camy (8.8),
//              w3 = angle | height << 16, w4 = horizon | focal << 16; no payload: flat again
#define TRI_OP_FPC_QUAD        0x3E
#define TRI_OP_FPC_SPRITE_3D   0x3F
#define TRI_OP_FPC_LAYER_MODE7 0x5F

// ---- sprites turned in 3D ----------------------------------------------------------------------------
// The cell (sx, sy, w, h) of a sheet turned in 3D about its centre, which
// goes to (cx, cy): rolled by `roll` (about the view axis, clockwise), then
// pitched by `pitch` (about its horizontal axis, its top towards the
// viewer), then turned by `yaw` (about its vertical axis, its right side
// towards the viewer), each 0..4095 a turn, scaled by `scale` (8.8, 256 =
// 1.0), and seen in perspective from 256 pixels in front of it. Flags:
// TRI_SPRITE_HFLIP and VFLIP (the cell mirrored before it is turned); the
// blend flags are ignored (index 0 is transparent, the rest opaque). Its
// back is not drawn, unless one of these says how it looks:
#define TRI_SPRITE_BACK_MIRROR 0x10u   // the cell mirrored, as through glass: a silhouette's other side (a plane tumbling)
#define TRI_SPRITE_BACK_YAW    0x20u   // the cell the right way round, as on a card turned over sideways (by yaw)
#define TRI_SPRITE_BACK_PITCH  0x30u   // the cell the right way round, as on a card turned over top to bottom (by pitch)
#define TRI_SPRITE_BACK_MASK   0x30u
void tri_sprite_draw_3d(int cx, int cy, int sheet, int sx, int sy, int w, int h, int pal, unsigned flags,
                        int yaw, int pitch, int roll, int scale);

// The cell with its four corners anywhere, in true perspective (the corners
// of a rectangle turned in 3D and projected): corner k (0 top-left, 1
// top-right, 2 bottom-right, 3 bottom-left of the cell) at (x[k], y[k]) on
// the canvas in 1/16 pixel. The quadrilateral must be convex. Flags as
// tri_sprite_draw_3d's (TRI_SPRITE_HFLIP, VFLIP).
void tri_sprite_draw_quad(const int x[4], const int y[4], int sheet, int sx, int sy, int w, int h, int pal, unsigned flags);

// ---- Mode 7 ------------------------------------------------------------------------------------------------
// From now on draw `layer` as a floor in perspective on canvas rows
// top..bottom-1 below the horizon row `horizon`: its map's plane (which
// wraps round, so the floor has no end) seen from (camx, camy) of the map
// (pixels, 8.8 fixed), facing `angle` (0 up the map, clockwise, 0..4095 a
// turn), from `height` pixels above it, with a lens of `focal` pixels. The
// layer's scroll is ignored meanwhile; everything else (its tileset, map,
// palette, priority, enable, the tiles written) is as for a flat layer.
// Call it again each frame the camera moves.
void tri_layer_mode7(int layer, int top, int bottom, int32_t camx, int32_t camy, int angle, int height, int horizon, int focal);
void tri_layer_flat(int layer);   // a flat scrolled layer again
