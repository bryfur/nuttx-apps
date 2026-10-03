/****************************************************************************
 * apps/graphics/fpcgl/fgl_setup.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Triangle setup (docs/gpu3d.md "TRI"): the rows a triangle covers, its
 * edges' x at their first rows and their slopes (16.16), and for each
 * attribute its plane: the value at the centre of pixel (xr, y0) and its
 * change per pixel across and down, in the GPU's fixed point.  Texture
 * coordinates go as s/w and t/w with q = 1/w, all scaled per triangle so
 * the GPU's 32-bit arithmetic keeps their precision.
 ****************************************************************************/

#include <math.h>
#include <stdint.h>

#include "fgl_setup.h"

#define TWO29   536870912.0f

static int32_t fix(float v)                /* (modulo 2^32, as the GPU) */
{
  return (int32_t)(uint32_t)(int64_t)rintf(v);
}

static int ceil_c(float v)                 /* The first row or column whose
                                            * centre is at or past v */
{
  return (int)ceilf(v - 0.5f);
}

int fgl_setup(const struct fgl_svert_s *a, const struct fgl_svert_s *b,
              const struct fgl_svert_s *c, int flags, int tw, int th,
              uint32_t *j)
{
  const struct fgl_svert_s *v0 = a;
  const struct fgl_svert_s *v1 = b;
  const struct fgl_svert_s *v2 = c;
  const struct fgl_svert_s *t;
  float att[3][7];                         /* z; q, sq, tq; r, g, b */
  float cross;
  float dxl;
  float dxs1;
  float dxs2;
  float xmin;
  float yc;
  float qmax;
  float uvmax;
  float du = 0.0f;
  float dv = 0.0f;
  int y0;
  int ym;
  int ye;
  int xr;
  int right;
  int S = 0;
  int n = 9;
  int k;
  int i;

  /* Top to bottom */

  if (v1->y < v0->y)
    {
      t = v0; v0 = v1; v1 = t;
    }

  if (v2->y < v1->y)
    {
      t = v1; v1 = v2; v2 = t;
    }

  if (v1->y < v0->y)
    {
      t = v0; v0 = v1; v1 = t;
    }

  cross = (v1->x - v0->x) * (v2->y - v0->y) -
          (v2->x - v0->x) * (v1->y - v0->y);
  y0 = ceil_c(v0->y);
  ym = ceil_c(v1->y);
  ye = ceil_c(v2->y);
  if (cross == 0.0f || ye <= y0)
    {
      return 0;
    }

  /* The long edge v0 v2 is on the right when v1 is left of it (y down:
   * cross < 0 then)
   */

  right = cross < 0.0f;
  dxl  = (v2->x - v0->x) / (v2->y - v0->y);
  dxs1 = v1->y > v0->y ? (v1->x - v0->x) / (v1->y - v0->y) : 0.0f;
  dxs2 = v2->y > v1->y ? (v2->x - v1->x) / (v2->y - v1->y) : 0.0f;

  yc = (float)y0 + 0.5f;
  j[1] = (uint32_t)(y0 & 0xffff) | (uint32_t)(ym - y0) << 16;
  xmin = fminf(v0->x, fminf(v1->x, v2->x));
  xr = (int)floorf(xmin);
  j[2] = (uint32_t)(ye - ym) | (uint32_t)(xr & 0xffff) << 16;
  j[3] = (uint32_t)fix((v0->x + (yc - v0->y) * dxl) * 65536.0f);
  j[4] = (uint32_t)fix(dxl * 65536.0f);
  j[5] = (uint32_t)fix((v0->x + (yc - v0->y) * dxs1) * 65536.0f);
  j[6] = (uint32_t)fix(dxs1 * 65536.0f);
  j[7] = (uint32_t)fix((v1->x + ((float)ym + 0.5f - v1->y) * dxs2) *
                       65536.0f);
  j[8] = (uint32_t)fix(dxs2 * 65536.0f);

  /* The attributes at the vertices, in the GPU's units */

  qmax = fmaxf(v0->q, fmaxf(v1->q, v2->q));
  if ((flags & FGL_TRI_UV) && tw > 0 && th > 0)
    {
      /* Repeating: moved by whole textures to near 0 */

      du = floorf(fminf(v0->u, fminf(v1->u, v2->u)) / tw) * tw;
      dv = floorf(fminf(v0->v, fminf(v1->v, v2->v)) / th) * th;
    }

  uvmax = 0.0f;
  for (i = 0; i < 3; i++)
    {
      const struct fgl_svert_s *p = i == 0 ? v0 : i == 1 ? v1 : v2;
      float q = p->q * (TWO29 / qmax);

      att[i][0] = p->z * 65536.0f;
      att[i][1] = q;
      att[i][2] = (p->u - du) * 16.0f;
      att[i][3] = (p->v - dv) * 16.0f;
      att[i][4] = p->r * (128.0f * 65536.0f);
      att[i][5] = p->g * (128.0f * 65536.0f);
      att[i][6] = p->b * (128.0f * 65536.0f);
      uvmax = fmaxf(uvmax, fmaxf(fabsf(att[i][2]), fabsf(att[i][3])));
    }

  /* S: |u16| < 2^(S + 1) keeps s/w (u16 q / 2^S) below 2^30 */

  while (S < 31 && uvmax >= (float)(1u << (S + 1)))
    {
      S++;
    }

  for (i = 0; i < 3; i++)
    {
      float sc = ldexpf(att[i][1], -S);

      att[i][2] *= sc;
      att[i][3] *= sc;
    }

  /* Each attribute's plane through the three, at pixel (xr, y0)'s centre */

  for (k = 0; k < 7; k++)
    {
      float d1;
      float d2;
      float dadx;
      float dady;

      if (k == 0 ? !(flags & FGL_TRI_Z) :
          k <= 3 ? !(flags & FGL_TRI_UV) :
          k == 4 ? !(flags & (FGL_TRI_RGB | FGL_TRI_MONO)) :
          !(flags & FGL_TRI_RGB))
        {
          continue;
        }

      d1 = att[1][k] - att[0][k];
      d2 = att[2][k] - att[0][k];
      dadx = (d1 * (v2->y - v0->y) - d2 * (v1->y - v0->y)) / cross;
      dady = (d2 * (v1->x - v0->x) - d1 * (v2->x - v0->x)) / cross;
      j[n++] = (uint32_t)fix(att[0][k] + dadx * ((float)xr + 0.5f - v0->x) +
                             dady * (yc - v0->y));
      j[n++] = (uint32_t)fix(dadx);
      j[n++] = (uint32_t)fix(dady);
    }

  j[0] = 11 | (uint32_t)(right | (flags & 0x1e)) << 8 | (uint32_t)S << 16;
  return n;
}
