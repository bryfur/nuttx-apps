/****************************************************************************
 * apps/graphics/fpcgame/fg_3d.c
 *
 * 3D effects for 2D games, from the GPU's textured rows (protocol v10's
 * TSPAN, docs/gpu.md): an image drawn as any quadrilateral in perspective
 * (as fpc_gpu_quad() draws them, but with exact row ends), turned in 3D
 * about its centre, and a Mode 7 floor.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "fg_internal.h"

#include "fpc_gpu.h"

#define FG_FOCAL 256.0f                   /* fg_quad3d()'s lens, canvas pixels */

static float fg_cosf(int angle)
{
  return (float)fpc_gpu_cos(angle) / 32768.0f;
}

static float fg_sinf(int angle)
{
  return (float)fpc_gpu_cos(angle - 1024) / 32768.0f;
}

/* The inverse map of a quadrilateral, the screen to the image's (u, v):
 * fpc_gpu_quad_map() (fpc_quad.c), as fpc_gpu_quad() draws them
 */

/* The texel (12.4) at pixel centre (x, y), kept within 0..umax, 0..vmax,
 * or false past the horizon
 */

static bool quad_uv(FAR const struct fpc_gpu_quad_s *q,
                    FAR const struct fpc_gpu_quad_map_s *m, float wsign,
                    float x, float y, int32_t umax, int32_t vmax,
                    FAR int32_t *pu, FAR int32_t *pv)
{
  float wn = m->g * x + m->h * y + m->i;
  float r;
  float u;
  float v;

  if (wn * wsign <= 1e-9f)
    {
      return false;
    }

  r = 16.0f / wn;
  u = (m->a * x + m->b * y + m->c) * r * q->sw;
  v = (m->d * x + m->e * y + m->f) * r * q->sh;
  *pu = u <= 0.0f ? 0 : u >= (float)umax ? umax : (int32_t)u;
  *pv = v <= 0.0f ? 0 : v >= (float)vmax ? vmax : (int32_t)v;
  return true;
}

/* A point after the one before it: TSPAN's step between two is 13 bits,
 * signed (rtl/blitter.v), so a larger one is cut to that (what the GPU's
 * model does with it then agrees with the GPU)
 */

static uint32_t quad_pt(int32_t u, int32_t v, int32_t pu, int32_t pv)
{
  u = u - pu > 4095 ? pu + 4095 : u - pu < -4095 ? pu - 4095 : u;
  v = v - pv > 4095 ? pv + 4095 : v - pv < -4095 ? pv - 4095 : v;
  u = u < 0 ? 0 : u > 65535 ? 65535 : u;
  v = v < 0 ? 0 : v > 65535 ? 65535 : v;
  return (uint32_t)u | (uint32_t)v << 16;
}

/* One TSPAN of a quadrilateral's row: n pixels from (x, y) of the
 * canvas; its points the inverse map every 16 pixels, the last one past
 * the row's end where it puts the row's last pixel (x + n - 1) at that
 * pixel's own texel (only 0..65535 as TSPAN has them)
 */

static void quad_span(FAR const struct fpc_gpu_quad_s *q,
                      FAR const struct fpc_gpu_quad_map_s *m, float wsign,
                      int x, int y, int n, int32_t umax, int32_t vmax)
{
  float yc = (float)y + 0.5f;
  int np = (n + 15) / 16 + 1;
  uint32_t *j = fpc_gpu_jobs_room(5 + np);
  int32_t pu = 0;
  int32_t pv = 0;
  int32_t lu;
  int32_t lv;
  int r;
  int k;

  j[0] = FPC_GPU_JOB_TSPAN | (q->flags & FPC_GPU_JOB_KEY);
  j[1] = (uint32_t)(g_fg.ox + x) | (uint32_t)(g_fg.oy + y) << 16;
  j[2] = (uint32_t)n | 1u << 16;
  j[3] = (uint32_t)q->sx | (uint32_t)q->sy << 16;
  j[4] = 255 | (uint32_t)q->key << 16;
  for (k = 0; k < np - 1; k++)
    {
      int32_t u = pu;
      int32_t v = pv;

      quad_uv(q, m, wsign, (float)(x + 16 * k) + 0.5f, yc, umax, vmax, &u,
              &v);                        /* (past the horizon: as the last) */
      j[5 + k] = k == 0 ? (uint32_t)u | (uint32_t)v << 16 :
                          quad_pt(u, v, pu, pv);
      pu = (int32_t)(j[5 + k] & 0xffff);
      pv = (int32_t)(j[5 + k] >> 16);
    }

  r = (n - 1) & 15;                       /* (the last pixel's, after np - 2) */
  lu = pu;
  lv = pv;
  if (r > 0 && quad_uv(q, m, wsign, (float)(x + n - 1) + 0.5f, yc, umax,
                       vmax, &lu, &lv))
    {
      lu = pu + (lu - pu) * 16 / r;
      lv = pv + (lv - pv) * 16 / r;
    }

  j[5 + np - 1] = quad_pt(lu, lv, pu, pv);
}

/* fpc_gpu_quad()'s rows, each from TSPANs whose points are the inverse map
 * every 16 pixels, but their ends exact.  TSPAN interpolates towards a
 * point past the row's end, which fpc_gpu_quad() keeps inside the image:
 * that squeezes up to 15 pixels at the row's end (a ragged right edge when
 * the rows' lengths differ).  Here a row is a TSPAN of its whole groups of
 * 16 pixels (its last point inside the row, or just past it) and one of
 * its last 16 pixels (again over the ones before them), its last point the
 * one that puts its last pixel at that pixel's texel: exact, unless that
 * point would be below 0 (a row ending on the image's left or top edge),
 * when it is still within a pixel's step of it.  Returns the rows drawn
 */

static int quad_rows(FAR const struct fpc_gpu_quad_s *q)
{
  struct fpc_gpu_quad_map_s m;
  float ymin;
  float ymax;
  float wsign;
  int32_t umax;
  int32_t vmax;
  int rows = 0;
  int y;
  int ya;
  int yb;
  int k;

  if (q->sw <= 0 || q->sh <= 0 || q->sx < 0 || q->sy < 0 ||
      q->sx + q->sw > FPC_GPU_SPACE_W || q->sy + q->sh > FPC_GPU_SPACE_H ||
      !fpc_gpu_quad_map(q, &m))
    {
      return -1;
    }

  ymin = ymax = q->y[0] / 16.0f;
  for (k = 1; k < 4; k++)
    {
      float yk = q->y[k] / 16.0f;

      ymin = yk < ymin ? yk : ymin;
      ymax = yk > ymax ? yk : ymax;
    }

  ya = (int)(ymin + 0.5f);
  ya += (float)ya + 0.5f < ymin;
  yb = (int)(ymax + 0.5f);
  yb += (float)yb + 0.5f < ymax;
  ya = ya < q->cy0 ? q->cy0 : ya;
  yb = yb > q->cy1 ? q->cy1 : yb;

  {
    float xc = (q->x[0] + q->x[1] + q->x[2] + q->x[3]) / 64.0f;
    float yc = (q->y[0] + q->y[1] + q->y[2] + q->y[3]) / 64.0f;

    wsign = m.g * xc + m.h * yc + m.i < 0 ? -1.0f : 1.0f;
  }

  umax = (int32_t)q->sw * 16 - 1;
  vmax = (int32_t)q->sh * 16 - 1;

  fpc_gpu_jobs_lock();
  for (y = ya; y < yb; y++)
    {
      float yc = (float)y + 0.5f;
      float xmin = 1e9f;
      float xmax = -1e9f;
      int xa;
      int xb;
      int w;

      /* Where the row's centre line crosses the edges */

      for (k = 0; k < 4; k++)
        {
          float px = q->x[k] / 16.0f, py = q->y[k] / 16.0f;
          float qx = q->x[(k + 1) & 3] / 16.0f;
          float qy = q->y[(k + 1) & 3] / 16.0f;

          if ((py <= yc && yc < qy) || (qy <= yc && yc < py))
            {
              float x = px + (yc - py) * (qx - px) / (qy - py);

              xmin = x < xmin ? x : xmin;
              xmax = x > xmax ? x : xmax;
            }
        }

      if (xmax < xmin)
        {
          continue;
        }

      xa = (int)(xmin + 0.5f + 4096.0f) - 4096;
      xa += (float)xa + 0.5f < xmin;
      xb = (int)(xmax + 0.5f + 4096.0f) - 4096;
      xb += (float)xb + 0.5f < xmax;
      xa = xa < q->cx0 ? q->cx0 : xa;
      xb = xb > q->cx1 ? q->cx1 : xb;
      w = xb - xa;
      if (w <= 0)
        {
          continue;
        }

      if (w > 16 && (w & 15) != 0)
        {
          quad_span(q, &m, wsign, xa, y, w & ~15, umax, vmax);
          quad_span(q, &m, wsign, xb - 16, y, 16, umax, vmax);
        }
      else
        {
          quad_span(q, &m, wsign, xa, y, w, umax, vmax);
        }

      rows++;
    }

  fpc_gpu_jobs_unlock();
  return rows;
}

void fg_quad(int img, int sx, int sy, int w, int h,
             FAR const int32_t x[4], FAR const int32_t y[4], uint32_t flags)
{
  FAR const struct fg_rect_s *r = fg_image_rect(img);
  struct fpc_gpu_quad_s q;
  int k;

  if (r == NULL || w <= 0 || h <= 0 || sx < 0 || sy < 0 ||
      sx + w > r->w || sy + h > r->h)
    {
      return;
    }

  q.sx = r->x + sx;
  q.sy = r->y + sy;
  q.sw = w;
  q.sh = h;
  for (k = 0; k < 4; k++)
    {
      /* Mirrored: the image's corners where their neighbours across (or
       * down) would go
       */

      int c = (flags & FG_HFLIP) ? k ^ 1 : k;

      c = (flags & FG_VFLIP) ? 3 - c : c;
      q.x[k] = x[c];
      q.y[k] = y[c];
    }

  /* The map from the canvas's own coordinates, so that where the canvas
   * lies in the space (the board's display is not the simulator's)
   * changes no rounding: the canvas's offset only goes into the rows'
   * jobs (quad_span())
   */

  q.cx0   = 0;
  q.cy0   = 0;
  q.cx1   = g_fg.w;
  q.cy1   = g_fg.h;
  q.flags = (flags & FG_OPAQUE) ? 0 : FPC_GPU_JOB_KEY;
  q.key   = FG_KEY;
  if (quad_rows(&q) > 0)
    {
      g_fg.sprites++;
    }
}

bool fg_quad3d_corners(int w, int h, int cx, int cy, int yaw, int pitch,
                       int roll, int scale, FAR int32_t x[4],
                       FAR int32_t y[4], FAR bool *back)
{
  static const float cu[4] = { -0.5f, 0.5f, 0.5f, -0.5f };
  static const float cv[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
  float cyw = fg_cosf(yaw), syw = fg_sinf(yaw);
  float cp = fg_cosf(pitch), sp = fg_sinf(pitch);
  float cr = fg_cosf(roll), sr = fg_sinf(roll);
  float s = (float)scale / 256.0f;
  int k;

  for (k = 0; k < 4; k++)
    {
      /* The corner in the image's plane, scaled, rolled (about the view
       * axis), pitched (about x), turned (about y), then projected from
       * FG_FOCAL in front of it
       */

      float px = cu[k] * w * s, py = cv[k] * h * s, pz;
      float t;

      t  = px * cr - py * sr;
      py = px * sr + py * cr;
      px = t;
      pz = py * sp;
      py = py * cp;
      t  = px * cyw + pz * syw;
      pz = pz * cyw - px * syw;
      px = t;
      pz += FG_FOCAL;
      if (pz < 1.0f)
        {
          return false;                   /* Through the viewer */
        }

      x[k] = (int32_t)(((float)cx + px * FG_FOCAL / pz) * 16.0f);
      y[k] = (int32_t)(((float)cy + py * FG_FOCAL / pz) * 16.0f);
    }

  /* Its back faces the viewer when the corners turn the other way */

  *back = (int64_t)(x[1] - x[0]) * (y[3] - y[0]) -
          (int64_t)(y[1] - y[0]) * (x[3] - x[0]) < 0;
  return true;
}

bool fg_quad3d(int img, int sx, int sy, int w, int h, int cx, int cy,
               uint32_t flags, int yaw, int pitch, int roll, int scale)
{
  int32_t x[4];
  int32_t y[4];
  bool back;

  if (!fg_quad3d_corners(w, h, cx, cy, yaw, pitch, roll, scale, x, y,
                         &back) || back)
    {
      return false;
    }

  fg_quad(img, sx, sy, w, h, x, y, flags);
  return true;
}

/* One row of a floor: w pixels from (x, y) of the canvas, the texel at
 * pixel p at (u0 + du p - ou, v0 + dv p - ov) of the image at r (12.4),
 * the points as TSPAN wants them (every 16 pixels, the last past the
 * row's end), kept within 0..umax, 0..vmax
 */

static void mode7_row(FAR const struct fg_rect_s *r, int x, int y, int w,
                      float u0, float v0, float du, float dv, float ou,
                      float ov, float umax, float vmax, uint32_t flags)
{
  int np = (w + 15) / 16 + 1;
  uint32_t *j = fpc_gpu_jobs_room(5 + np);
  int k;

  j[0] = FPC_GPU_JOB_TSPAN | ((flags & FG_OPAQUE) ? 0 : FPC_GPU_JOB_KEY);
  j[1] = (uint32_t)(g_fg.ox + x) | (uint32_t)(g_fg.oy + y) << 16;
  j[2] = (uint32_t)w | 1u << 16;
  j[3] = (uint32_t)r->x | (uint32_t)r->y << 16;
  j[4] = 255 | (uint32_t)FG_KEY << 16;
  for (k = 0; k < np; k++)
    {
      float px = (float)(x + 16 * k);
      float u = u0 + du * px - ou;
      float v = v0 + dv * px - ov;
      int32_t iu = u <= 0.0f ? 0 : u >= umax ? (int32_t)umax : (int32_t)u;
      int32_t iv = v <= 0.0f ? 0 : v >= vmax ? (int32_t)vmax : (int32_t)v;

      j[5 + k] = k == 0 ? (uint32_t)iu | (uint32_t)iv << 16 :
                 quad_pt(iu, iv, (int32_t)(j[4 + k] & 0xffff),
                         (int32_t)(j[4 + k] >> 16));
    }
}

static float fg_floorf(float f)
{
  int32_t i = (int32_t)f;

  return (float)(i - (f < (float)i));
}

void fg_mode7_rect(FAR const struct fg_rect_s *r, int top, int bottom,
                   int32_t camx, int32_t camy, int angle, int height,
                   int horizon, int focal, uint32_t flags)
{
  float fx = fg_sinf(angle), fy = -fg_cosf(angle);   /* Forward, right */
  float rx = -fy, ry = fx;
  float cu = (float)camx / 16.0f;         /* (8.8 to 12.4) */
  float cv = (float)camy / 16.0f;
  float iw = (float)(r->w * 16);          /* The image, 12.4 */
  float ih = (float)(r->h * 16);
  float umax;
  float vmax;
  int y;

  if (height <= 0 || focal <= 0 || r->w <= 0 || r->h <= 0)
    {
      return;
    }

  top = top < 0 ? 0 : top;
  top = top <= horizon ? horizon + 1 : top;
  bottom = bottom > g_fg.h ? g_fg.h : bottom;
  umax = (float)(r->w * 16 - 1);
  vmax = (float)(r->h * 16 - 1);

  fpc_gpu_jobs_lock();
  for (y = top; y < bottom; y++)
    {
      /* The row's line on the plane, u = u0 + du x (the same for v), in
       * the image's 12.4 texels
       */

      float dy = (float)y + 0.5f - (float)horizon;
      float z = (float)height * (float)focal / dy * 16.0f;
      float lat = (float)height / dy * 16.0f;     /* Per pixel across */
      float xc = (float)g_fg.w / 2.0f - 0.5f;     /* (the centre pixel's x) */
      float u0 = cu + z * fx - xc * lat * rx;
      float v0 = cv + z * fy - xc * lat * ry;
      float du = lat * rx;
      float dv = lat * ry;
      float xa = 0.0f;
      float xb = (float)g_fg.w;
      int ia;
      int ib;

      if (flags & FG_WRAP)
        {
          int n;

          /* The image again and again: the row in pieces, each within
           * one copy of it (where u and v cross no edge), drawn from
           * the image as if that copy (its points from the line itself,
           * so the pieces meet exactly; only 0..65535 as TSPAN has them)
           */

          ia = 0;
          for (n = 0; ia < g_fg.w && n < 64; n++)
            {
              float u = u0 + du * (float)ia;
              float v = v0 + dv * (float)ia;
              float ku = fg_floorf(u / iw);
              float kv = fg_floorf(v / ih);
              float e = (float)g_fg.w;

              if (du > 1e-6f)
                {
                  float b = ((ku + 1.0f) * iw - u0) / du;
                  e = b < e ? b : e;
                }
              else if (du < -1e-6f)
                {
                  float b = (ku * iw - u0) / du;
                  e = b < e ? b : e;
                }

              if (dv > 1e-6f)
                {
                  float b = ((kv + 1.0f) * ih - v0) / dv;
                  e = b < e ? b : e;
                }
              else if (dv < -1e-6f)
                {
                  float b = (kv * ih - v0) / dv;
                  e = b < e ? b : e;
                }

              ib = (int)e;
              ib += (float)ib < e;                /* The pixels before e */
              ib = ib <= ia ? ia + 1 : ib > g_fg.w ? g_fg.w : ib;
              mode7_row(r, ia, y, ib - ia, u0, v0, du, dv, ku * iw, kv * ih,
                        65535.0f, 65535.0f, flags);
              ia = ib;
            }

          continue;
        }

      /* Only the part on the image: 0 <= u <= umax, the same for v */

      if (du > 1e-6f || du < -1e-6f)
        {
          float a = (0.0f - u0) / du, b = (umax - u0) / du;

          if (a > b)
            {
              float t = a;
              a = b;
              b = t;
            }

          xa = a > xa ? a : xa;
          xb = b < xb ? b : xb;
        }
      else if (u0 < 0.0f || u0 > umax)
        {
          continue;
        }

      if (dv > 1e-6f || dv < -1e-6f)
        {
          float a = (0.0f - v0) / dv, b = (vmax - v0) / dv;

          if (a > b)
            {
              float t = a;
              a = b;
              b = t;
            }

          xa = a > xa ? a : xa;
          xb = b < xb ? b : xb;
        }
      else if (v0 < 0.0f || v0 > vmax)
        {
          continue;
        }

      ia = (int)(xa + 0.999f);            /* (centres: within a hair) */
      ib = (int)(xb + 0.999f);
      ia = ia < 0 ? 0 : ia;
      ib = ib > g_fg.w ? g_fg.w : ib;
      if (ib - ia <= 0)
        {
          continue;
        }

      mode7_row(r, ia, y, ib - ia, u0, v0, du, dv, 0.0f, 0.0f, umax, vmax,
                flags);
    }

  fpc_gpu_jobs_unlock();
  g_fg.sprites++;
}

void fg_mode7(int img, int top, int bottom, int32_t camx, int32_t camy,
              int angle, int height, int horizon, int focal, uint32_t flags)
{
  FAR const struct fg_rect_s *r = fg_image_rect(img);

  if (r != NULL)
    {
      fg_mode7_rect(r, top, bottom, camx, camy, angle, height, horizon,
                    focal, flags);
    }
}
