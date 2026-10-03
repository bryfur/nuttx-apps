/****************************************************************************
 * apps/graphics/fpcgame/fg_3d.c
 *
 * 3D effects for 2D games, from the GPU's textured rows (protocol v10's
 * TSPAN, docs/gpu.md): an image drawn as any quadrilateral in perspective
 * (fpc_gpu_quad()), turned in 3D about its centre, and a Mode 7 floor.
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
      q.x[k] = x[k] + g_fg.ox * 16;
      q.y[k] = y[k] + g_fg.oy * 16;
    }

  q.cx0   = g_fg.ox;
  q.cy0   = g_fg.oy;
  q.cx1   = g_fg.ox + g_fg.w;
  q.cy1   = g_fg.oy + g_fg.h;
  q.flags = (flags & FG_OPAQUE) ? 0 : FPC_GPU_JOB_KEY;
  q.key   = FG_KEY;
  if (fpc_gpu_quad(&q) > 0)
    {
      g_fg.sprites++;
    }
}

bool fg_quad3d(int img, int sx, int sy, int w, int h, int cx, int cy,
               uint32_t flags, int yaw, int pitch, int roll, int scale)
{
  static const float cu[4] = { -0.5f, 0.5f, 0.5f, -0.5f };
  static const float cv[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
  float cyw = fg_cosf(yaw), syw = fg_sinf(yaw);
  float cp = fg_cosf(pitch), sp = fg_sinf(pitch);
  float cr = fg_cosf(roll), sr = fg_sinf(roll);
  float s = (float)scale / 256.0f;
  int32_t x[4];
  int32_t y[4];
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

  if ((int64_t)(x[1] - x[0]) * (y[3] - y[0]) -
      (int64_t)(y[1] - y[0]) * (x[3] - x[0]) < 0)
    {
      return false;
    }

  fg_quad(img, sx, sy, w, h, x, y, flags);
  return true;
}

void fg_mode7(int img, int top, int bottom, int32_t camx, int32_t camy,
              int angle, int height, int horizon, int focal, uint32_t flags)
{
  FAR const struct fg_rect_s *r = fg_image_rect(img);
  float fx = fg_sinf(angle), fy = -fg_cosf(angle);   /* Forward, right */
  float rx = -fy, ry = fx;
  float cu = (float)camx / 16.0f;         /* (8.8 to 12.4) */
  float cv = (float)camy / 16.0f;
  float umax;
  float vmax;
  int y;

  if (r == NULL || height <= 0 || focal <= 0)
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
      uint32_t *j;
      int ia;
      int ib;
      int w;
      int np;
      int k;

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
      w = ib - ia;
      if (w <= 0)
        {
          continue;
        }

      np = (w + 15) / 16 + 1;
      j = fpc_gpu_jobs_room(5 + np);
      j[0] = FPC_GPU_JOB_TSPAN | ((flags & FG_OPAQUE) ? 0 : FPC_GPU_JOB_KEY);
      j[1] = (uint32_t)(g_fg.ox + ia) | (uint32_t)(g_fg.oy + y) << 16;
      j[2] = (uint32_t)w | 1u << 16;
      j[3] = (uint32_t)r->x | (uint32_t)r->y << 16;
      j[4] = 255 | (uint32_t)FG_KEY << 16;
      for (k = 0; k < np; k++)
        {
          float px = (float)(ia + 16 * k);
          float u = u0 + du * px;
          float v = v0 + dv * px;
          int32_t iu = u <= 0.0f ? 0 : u >= umax ? (int32_t)umax : (int32_t)u;
          int32_t iv = v <= 0.0f ? 0 : v >= vmax ? (int32_t)vmax : (int32_t)v;

          j[5 + k] = (uint32_t)iu | (uint32_t)iv << 16;
        }
    }

  fpc_gpu_jobs_unlock();
  g_fg.sprites++;
}
