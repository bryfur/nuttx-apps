/****************************************************************************
 * apps/graphics/fpcgl/fgl_setup.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Polygon setup (docs/gpu3d.md "TRI"): a convex polygon in screen space as
 * one TRI job: its rows as segments between its vertices' rows, each with
 * its left and right edge (x at its first row, dx/dy: 16.16), and for each
 * attribute its plane: the value at the centre of pixel (xr, y0) and its
 * change per pixel across and down, in the GPU's fixed point.  Texture
 * coordinates go as s/w and t/w with q = 1/w, all scaled per polygon so
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

/* An edge's x at row r's centre (16.16) and its slope */

static void edge_at(const struct fgl_svert_s *a, const struct fgl_svert_s *b,
                    int r, uint32_t *x, uint32_t *dx)
{
  float d = b->y > a->y ? (b->x - a->x) / (b->y - a->y) : 0.0f;

  *x = (uint32_t)fix((a->x + ((float)r + 0.5f - a->y) * d) * 65536.0f);
  *dx = (uint32_t)fix(d * 65536.0f);
}

int fgl_setup_poly(const struct fgl_svert_s *v, int n, int flags, int tw,
                   int th, uint32_t *j)
{
  const struct fgl_svert_s *lc[FGL_POLY_MAX + 1];  /* The chains, top to bottom */
  const struct fgl_svert_s *rc[FGL_POLY_MAX + 1];
  const struct fgl_svert_s *p0;
  const struct fgl_svert_s *p1;
  const struct fgl_svert_s *p2;
  float att[3][9];                          /* z; q, sq, tq; r, g, b; a; f */
  float area = 0.0f;
  float cross;
  float xmin;
  float qmax;
  float uvmax;
  float best;
  float du = 0.0f;
  float dv = 0.0f;
  float yc;
  int it = 0;
  int ib = 0;
  int nl = 0;
  int nr = 0;
  int li = 0;
  int ri = 0;
  int y0;
  int ye;
  int r;
  int xr;
  int S = 0;
  int nseg = 0;
  int nw = 2;
  int i;
  int k;

  if (n < 3 || n > FGL_POLY_MAX)
    {
      return 0;
    }

  for (i = 0; i < n; i++)
    {
      const struct fgl_svert_s *a = &v[i];
      const struct fgl_svert_s *b = &v[(i + 1) % n];

      area += a->x * b->y - b->x * a->y;
      it = v[i].y < v[it].y ? i : it;
      ib = v[i].y > v[ib].y ? i : ib;
    }

  y0 = ceil_c(v[it].y);
  ye = ceil_c(v[ib].y);
  if (area == 0.0f || ye <= y0)
    {
      return 0;
    }

  /* The chains from the top vertex to the bottom one: going forward is the
   * right side when the polygon turns clockwise on the screen (y down:
   * area > 0)
   */

  for (i = it; ; i = (i + 1) % n)
    {
      (area > 0.0f ? rc : lc)[area > 0.0f ? nr++ : nl++] = &v[i];
      if (i == ib)
        {
          break;
        }
    }

  for (i = it; ; i = (i + n - 1) % n)
    {
      (area > 0.0f ? lc : rc)[area > 0.0f ? nl++ : nr++] = &v[i];
      if (i == ib)
        {
          break;
        }
    }

  /* The segments: a new one where either edge ends (from the row whose
   * centre is at or below the vertex, the next edge holds)
   */

  xmin = v[0].x;
  for (i = 1; i < n; i++)
    {
      xmin = v[i].x < xmin ? v[i].x : xmin;
    }

  xr = (int)floorf(xmin);
  j[1] = (uint32_t)(y0 & 0xffff) | (uint32_t)(xr & 0xffff) << 16;

  /* (the attributes go first: the segments are written after them, below,
   * once their count is known)
   */

  /* The attributes at the vertices, in the GPU's units */

  qmax = v[0].q;
  for (i = 1; i < n; i++)
    {
      qmax = v[i].q > qmax ? v[i].q : qmax;
    }

  if ((flags & FGL_TRI_UV) && tw > 0 && th > 0)
    {
      /* Repeating: moved by whole textures to near 0 */

      float umin = v[0].u;
      float vmin = v[0].v;

      for (i = 1; i < n; i++)
        {
          umin = v[i].u < umin ? v[i].u : umin;
          vmin = v[i].v < vmin ? v[i].v : vmin;
        }

      du = floorf(umin / tw) * tw;
      dv = floorf(vmin / th) * th;
    }

  uvmax = 0.0f;
  for (i = 0; i < n; i++)
    {
      uvmax = fmaxf(uvmax, fmaxf(fabsf((v[i].u - du) * 16.0f),
                                 fabsf((v[i].v - dv) * 16.0f)));
    }

  /* S: |u16| < 2^(S + 1) keeps s/w (u16 q / 2^S) below 2^30 */

  while (S < 31 && uvmax >= (float)(1u << (S + 1)))
    {
      S++;
    }

  /* The planes through the best-conditioned three: the top, the bottom and
   * the vertex farthest from the line between them
   */

  p0 = &v[it];
  p1 = &v[ib];
  p2 = &v[(it + 1) % n];
  best = -1.0f;
  for (i = 0; i < n; i++)
    {
      float c = fabsf((p1->x - p0->x) * (v[i].y - p0->y) -
                      (v[i].x - p0->x) * (p1->y - p0->y));

      if (c > best)
        {
          best = c;
          p2 = &v[i];
        }
    }

  cross = (p1->x - p0->x) * (p2->y - p0->y) - (p2->x - p0->x) * (p1->y - p0->y);
  if (cross == 0.0f)
    {
      return 0;
    }

  for (i = 0; i < 3; i++)
    {
      const struct fgl_svert_s *p = i == 0 ? p0 : i == 1 ? p1 : p2;
      float q = p->q * (TWO29 / qmax);
      float sc = ldexpf(q, -S);

      att[i][0] = p->z * 65536.0f;
      att[i][1] = q;
      att[i][2] = (p->u - du) * 16.0f * sc;
      att[i][3] = (p->v - dv) * 16.0f * sc;
      att[i][4] = p->r * (128.0f * 65536.0f);
      att[i][5] = p->g * (128.0f * 65536.0f);
      att[i][6] = p->b * (128.0f * 65536.0f);
      att[i][7] = p->a * (255.0f * 65536.0f);
      att[i][8] = p->f * (255.0f * 65536.0f);
    }

  yc = (float)y0 + 0.5f;
  for (k = 0; k < 9; k++)
    {
      float d1;
      float d2;
      float dadx;
      float dady;

      if (k == 0 ? !(flags & FGL_TRI_Z) :
          k <= 3 ? !(flags & FGL_TRI_UV) :
          k == 4 ? !(flags & (FGL_TRI_RGB | FGL_TRI_MONO)) :
          k <= 6 ? !(flags & FGL_TRI_RGB) :
          k == 7 ? !(flags & FGL_TRI_A) : !(flags & FGL_TRI_F))
        {
          continue;
        }

      d1 = att[1][k] - att[0][k];
      d2 = att[2][k] - att[0][k];
      dadx = (d1 * (p2->y - p0->y) - d2 * (p1->y - p0->y)) / cross;
      dady = (d2 * (p1->x - p0->x) - d1 * (p2->x - p0->x)) / cross;

      /* The colours, alpha and fog: on the plane at every vertex (within
       * 2/128 of the shade, 2/255 of alpha and fog), or the caller draws
       * triangles (corners' colours across a quad are not planar; depth
       * and texture coordinates are, the polygon being flat)
       */

      if (k >= 4)
        {
          for (i = 0; i < n; i++)
            {
              float c = k == 4 ? v[i].r * (128.0f * 65536.0f) :
                        k == 5 ? v[i].g * (128.0f * 65536.0f) :
                        k == 6 ? v[i].b * (128.0f * 65536.0f) :
                        k == 7 ? v[i].a * (255.0f * 65536.0f) :
                        v[i].f * (255.0f * 65536.0f);

              if (fabsf(att[0][k] + dadx * (v[i].x - p0->x) +
                        dady * (v[i].y - p0->y) - c) > 2.0f * 65536.0f)
                {
                  return -1;
                }
            }
        }
      j[nw++] = (uint32_t)fix(att[0][k] + dadx * ((float)xr + 0.5f - p0->x) +
                              dady * (yc - p0->y));
      j[nw++] = (uint32_t)fix(dadx);
      j[nw++] = (uint32_t)fix(dady);
    }

  /* The segments */

  for (r = y0; r < ye; )
    {
      float c = (float)r + 0.5f;
      int end;

      while (li + 2 < nl && lc[li + 1]->y <= c)
        {
          li++;
        }

      while (ri + 2 < nr && rc[ri + 1]->y <= c)
        {
          ri++;
        }

      end = ye;
      if (li + 2 < nl && ceil_c(lc[li + 1]->y) < end)
        {
          end = ceil_c(lc[li + 1]->y);
        }

      if (ri + 2 < nr && ceil_c(rc[ri + 1]->y) < end)
        {
          end = ceil_c(rc[ri + 1]->y);
        }

      if (end <= r)
        {
          end = r + 1;                        /* (cannot happen: a row at least) */
        }

      if (nseg == FGL_SEG_MAX)
        {
          return -1;                          /* (the caller splits it) */
        }

      j[nw] = (uint32_t)(end - r);
      edge_at(lc[li], lc[li + 1], r, &j[nw + 1], &j[nw + 2]);
      edge_at(rc[ri], rc[ri + 1], r, &j[nw + 3], &j[nw + 4]);
      nw += 5;
      nseg++;
      r = end;
    }

  j[0] = 11 | (uint32_t)(flags & 0x7e) << 8 | (uint32_t)S << 16 |
         (uint32_t)nseg << 24;
  return nw;
}

int fgl_setup(const struct fgl_svert_s *a, const struct fgl_svert_s *b,
              const struct fgl_svert_s *c, int flags, int tw, int th,
              uint32_t *j)
{
  struct fgl_svert_s v[3];

  v[0] = *a;
  v[1] = *b;
  v[2] = *c;
  return fgl_setup_poly(v, 3, flags, tw, th, j);
}
