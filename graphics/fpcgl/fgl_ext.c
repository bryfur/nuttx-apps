/****************************************************************************
 * apps/graphics/fpcgl/fgl_ext.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fpcgl's own beyond GL: the picture's place (a game's 2D parts over it),
 * a colour blended over the picture, the space for a game's own images and
 * a rectangle of it as the texture.
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

/* A STATE of its own (untextured, no depth, blended by alpha) and one TRI
 * over the picture, its alpha constant
 */

void fglBlendView(uint16_t colour, int alpha)
{
  struct fgl_svert_s v[4];
  uint32_t j[FGL_TRI_MAX];
  uint32_t st[8];
  int nw;
  int i;

  if (!g_fgl.on || alpha <= 0)
    {
      return;
    }

  alpha = alpha > 255 ? 255 : alpha;
  st[0] = 10 | (1u << 7 | (uint32_t)g_fgl.dither << 9) << 8;   /* BLEND, DITHER */
  st[1] = (uint32_t)(g_fgl.bx[g_fgl.back] + g_fgl.ox) |
          (uint32_t)(g_fgl.by[g_fgl.back] + g_fgl.oy) << 16;
  st[2] = (uint32_t)g_fgl.zx | (uint32_t)g_fgl.zy << 16;
  st[3] = 0;
  st[4] = 1 | 1 << 16;
  st[5] = (uint32_t)colour << 16;
  st[6] = (uint32_t)g_fgl.w | (uint32_t)g_fgl.h << 16;
  st[7] = 6 | 7 << 4;                     /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA */

  memset(v, 0, sizeof(v));
  for (i = 0; i < 4; i++)
    {
      v[i].x = (i == 1 || i == 2) ? (float)g_fgl.w : 0.0f;
      v[i].y = i >= 2 ? (float)g_fgl.h : 0.0f;
      v[i].q = 1.0f;
      v[i].a = (float)alpha / 255.0f;
    }

  nw = fgl_setup_poly(v, 4, FGL_TRI_A, 0, 0, j);
  if (nw > 0)
    {
      uint32_t *o;

      fgl_lock();
      o = fpc_gpu_jobs_room(8 + nw);
      memcpy(o, st, sizeof(st));
      memcpy(o + 8, j, (size_t)nw * 4);
      g_fgl.st_ok = false;                /* (the next polygon sends its own) */
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

void fglPut(int x, int y, int w, int h, const uint16_t *px, size_t stride)
{
  if (!g_fgl.on || w < 1 || h < 1)
    {
      return;
    }

  fgl_unlock();                           /* (PUT is a command, after the jobs) */
  fpc_gpu_put(x, y, w, h, px, stride);
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
  t->fmt    = 0;                          /* (RGB565) */
  g_fgl.bound = FGL_RECT;
}
