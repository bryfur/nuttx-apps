/****************************************************************************
 * apps/graphics/fpcgl/fgl_setup.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Polygon setup for the 3D GPU (docs/gpu3d.md "TRI"): a convex polygon in
 * screen space becomes one TRI job.  Portable C (the host's tests build
 * it with the GPU's reference model).
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCGL_FGL_SETUP_H
#define __APPS_GRAPHICS_FPCGL_FGL_SETUP_H

#include <stdint.h>

/* A vertex after projection: x, y in target pixels (pixel (i, j) covers
 * [i, i + 1) x [j, j + 1)), z the depth (0..65535: z << 16 is TRI's z), q
 * = 1/w, u, v in texels, r, g, b the shade (1.0: as the texel is; up to
 * 2), a the alpha (0..1), f the fog amount (0: none, 1: the fog's colour)
 */

struct fgl_svert_s
{
  float x;
  float y;
  float z;
  float q;
  float u;
  float v;
  float r;
  float g;
  float b;
  float a;
  float f;
};

#define FGL_TRI_Z      0x02               /* TRI flags (docs/gpu3d.md) */
#define FGL_TRI_UV     0x04
#define FGL_TRI_RGB    0x08
#define FGL_TRI_MONO   0x10
#define FGL_TRI_A      0x20
#define FGL_TRI_F      0x40

#define FGL_POLY_MAX   16                 /* A polygon's vertices at most */
#define FGL_SEG_MAX    15                 /* ... its segments (TRI's nseg) */
#define FGL_TRI_MAX    (2 + 27 + 5 * FGL_SEG_MAX)   /* Words of the longest TRI */

/* The TRI job for the triangle (any winding) with the attributes flags
 * asks for (FGL_TRI_*), into j[]; wrap: tw, th, a power of two each, by
 * which u, v may be moved (repeating textures), or 0.  Returns its words,
 * 0 if it covers no pixel centre
 */

int fgl_setup(const struct fgl_svert_s *a, const struct fgl_svert_s *b,
              const struct fgl_svert_s *c, int flags, int tw, int th,
              uint32_t *j);

/* The same for a convex, flat polygon of n vertices (3..FGL_POLY_MAX, in
 * order, either winding): one TRI of up to FGL_SEG_MAX segments; 0 if it
 * covers no pixel centre, -1 if it needs more segments or its colours,
 * alpha or fog are not planar (the caller draws it as triangles).  Its attributes are the
 * plane through three of its vertices
 */

int fgl_setup_poly(const struct fgl_svert_s *v, int n, int flags, int tw,
                   int th, uint32_t *j);

#endif /* __APPS_GRAPHICS_FPCGL_FGL_SETUP_H */
