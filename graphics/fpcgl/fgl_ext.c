/****************************************************************************
 * apps/graphics/fpcgl/fgl_ext.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fpcgl's own beyond GL: the picture's place (a game's 2D parts over it),
 * a colour over the picture (BLEND), and the 3D GPU's surface builder
 * (BUILD, BTAB: docs/gpu3d.md) for Quake's surface cache.
 ****************************************************************************/

#include <nuttx/config.h>

#include <string.h>

#include "fgl_internal.h"
#include "fpc_gpu.h"

void fglBackBuffer(int *x, int *y)
{
  *x = g_fgl.bx[g_fgl.back] + g_fgl.ox;
  *y = g_fgl.by[g_fgl.back] + g_fgl.oy;
}

void fglBlendView(uint16_t colour, int alpha)
{
  uint32_t *j;

  if (!g_fgl.on || alpha <= 0)
    {
      return;
    }

  alpha = alpha > 255 ? 255 : alpha;
  fgl_lock();
  j = fpc_gpu_jobs_room(7);
  j[0] = 4 | (uint32_t)colour << 16;      /* BLEND from a colour, no shape */
  j[1] = (uint32_t)(g_fgl.bx[g_fgl.back] + g_fgl.ox) |
         (uint32_t)(g_fgl.by[g_fgl.back] + g_fgl.oy) << 16;
  j[2] = (uint32_t)g_fgl.w | (uint32_t)g_fgl.h << 16;
  j[3] = 0;
  j[4] = (uint32_t)alpha;
  j[5] = 0;
  j[6] = 0;
}

int fglImage8(struct fgl_image8_s *img, int w, int h, const uint8_t *px)
{
  int j;

  if (w < 2 || (w & 1) || h < 1 || w > 2048 || h > 2048 ||
      fgl_space_alloc(w / 2, h, &img->x, &img->y) < 0)
    {
      return -1;
    }

  img->w = (int16_t)w;
  img->h = (int16_t)h;
  fgl_unlock();                           /* (PUTs are commands) */
  for (j = 0; j < h; j++)
    {
      /* Two texels a pixel, the lower byte first: the bytes as they are */

      fpc_gpu_put(img->x, img->y + j, w / 2, 1,
                  (const uint16_t *)(px + (size_t)j * w), (size_t)w);
    }

  return 0;
}

void fglImage8Free(struct fgl_image8_s *img)
{
  if (img->w > 0)
    {
      fgl_space_free(img->x, img->y, img->w / 2, img->h);
      img->w = 0;
    }
}

void fglBuildTable(const uint16_t pal[256], const uint8_t factor[64])
{
  uint32_t *j;
  int i;

  fgl_lock();
  j = fpc_gpu_jobs_room(322);
  j[0] = 9;
  j[1] = 320;
  for (i = 0; i < 256; i++)
    {
      j[2 + i] = pal[i];
    }

  for (i = 0; i < 64; i++)
    {
      j[258 + i] = factor[i];
    }
}

void fglReset(void)
{
  int16_t x;
  int16_t y;
  int k;

  if (!g_fgl.on)
    {
      return;
    }

  fgl_unlock();
  memset(g_fgl.tex, 0, sizeof(g_fgl.tex));
  g_fgl.bound = 0;
  g_fgl.st_ok = false;

  /* The space again from its start: the buffers made again (the same
   * sizes in the same order: the same places)
   */

  fgl_space_reset();
  for (k = 0; k < 2; k++)
    {
      fgl_space_alloc(g_fgl.dw, g_fgl.dh, &x, &y);
    }

  fgl_space_alloc(g_fgl.w, g_fgl.h, &x, &y);
}

int fglSpaceAlloc(int w, int h, int16_t *x, int16_t *y)
{
  return fgl_space_alloc(w, h, x, y);
}

void fglSpaceFree(int x, int y, int w, int h)
{
  fgl_space_free(x, y, w, h);
}

void fglBindRect(int x, int y, int w, int h, GLboolean repeat)
{
  struct fgl_tex_s *t = &g_fgl.tex[FGL_RECT];

  t->named  = true;
  t->alloc  = true;
  t->x      = (int16_t)x;
  t->y      = (int16_t)y;
  t->w      = (int16_t)w;
  t->h      = (int16_t)h;
  t->aw     = 0;                          /* (not fpcgl's: never freed) */
  t->ah     = 0;
  t->wrap_s = repeat ? GL_REPEAT : GL_CLAMP;
  t->wrap_t = t->wrap_s;
  g_fgl.bound = FGL_RECT;
}

void fglBuildSurface(int dx, int dy, int w, int h,
                     const struct fgl_image8_s *img, int texw, int texh,
                     int soff, int toff, const uint16_t *light, int lw,
                     int bsh)
{
  int rows = ((h - 1) >> bsh) + 2;
  int wpr = (lw + 1) / 2;
  uint32_t *j;
  int r;
  int c;

  if (w < 1 || h < 1 || w > 1024 || lw < 2 || lw > 32 || bsh < 0 ||
      bsh > 4 || w > (lw - 1) << bsh || texw < 2 || (texw & 1) ||
      texw > 1024 || texh < 1 || 7 + rows * wpr > 448)
    {
      fgl_error(GL_INVALID_VALUE);
      return;
    }

  fgl_lock();
  j = fpc_gpu_jobs_room(7 + rows * wpr);
  j[0] = 8;
  j[1] = (uint32_t)dx | (uint32_t)dy << 16;
  j[2] = (uint32_t)w | (uint32_t)h << 16;
  j[3] = (uint32_t)img->x | (uint32_t)img->y << 16;
  j[4] = (uint32_t)texw | (uint32_t)texh << 16;
  j[5] = (uint32_t)soff | (uint32_t)toff << 16;
  j[6] = (uint32_t)lw | (uint32_t)bsh << 16;
  j += 7;
  for (r = 0; r < rows; r++)
    {
      for (c = 0; c < lw; c += 2)
        {
          *j++ = light[r * lw + c] |
                 (uint32_t)(c + 1 < lw ? light[r * lw + c + 1] : 0) << 16;
        }
    }
}
