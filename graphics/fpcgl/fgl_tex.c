/****************************************************************************
 * apps/graphics/fpcgl/fgl_tex.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fpcgl's textures and its space: while the 3D GPU runs, fpcgl has the
 * whole 2048 x 2048 space (the desktop's is lost anyway), handed out in
 * shelves (rows of rectangles of about one height), freed rectangles kept
 * for reuse at their size or smaller.  A texture is an RGB565 image there,
 * sent with PUT (RGB, RGBA: alpha below 128 becomes the colour key,
 * luminance, RGB565) or PUT8 through fglColorTable()'s palette
 * (GL_COLOR_INDEX).
 ****************************************************************************/

#include <nuttx/config.h>

#include <string.h>

#include "fgl_internal.h"
#include "fpc_gpu.h"

#define SPACE          2048
#define SHELVES        64
#define FREED          128

static struct
{
  int16_t y;
  int16_t h;
  int16_t x;                              /* Used up to here */
} g_shelf[SHELVES];

static struct
{
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
} g_freed[FREED];

static int g_nshelf;
static int g_nfreed;
static int g_ytop;                        /* The shelves end here */

void fgl_space_reset(void)
{
  g_nshelf = 0;
  g_nfreed = 0;
  g_ytop = 0;
}

/* w x h (w even: PUT8's x and w), on a shelf of its height or a little
 * more, or in a freed place as big; 0, or < 0 if there is no room
 */

int fgl_space_alloc(int w, int h, int16_t *x, int16_t *y)
{
  int best = -1;
  int i;

  w = (w + 1) & ~1;
  if (w <= 0 || h <= 0 || w > SPACE || h > SPACE)
    {
      return -1;
    }

  for (i = 0; i < g_nfreed; i++)
    {
      if (g_freed[i].w >= w && g_freed[i].h >= h &&
          (best < 0 || g_freed[i].w * g_freed[i].h <
                       g_freed[best].w * g_freed[best].h))
        {
          best = i;
        }
    }

  if (best >= 0 && g_freed[best].w * g_freed[best].h <= 2 * w * h)
    {
      *x = g_freed[best].x;
      *y = g_freed[best].y;
      g_freed[best] = g_freed[--g_nfreed];
      return 0;
    }

  for (i = 0; i < g_nshelf; i++)
    {
      if (g_shelf[i].h >= h && g_shelf[i].h <= h + h / 2 + 4 &&
          SPACE - g_shelf[i].x >= w)
        {
          *x = g_shelf[i].x;
          *y = g_shelf[i].y;
          g_shelf[i].x += w;
          return 0;
        }
    }

  if (g_nshelf < SHELVES && SPACE - g_ytop >= h)
    {
      g_shelf[g_nshelf].y = g_ytop;
      g_shelf[g_nshelf].h = h;
      g_shelf[g_nshelf].x = w;
      g_nshelf++;
      *x = 0;
      *y = g_ytop;
      g_ytop += h;
      return 0;
    }

  if (best >= 0)                          /* (a bigger freed place, then) */
    {
      *x = g_freed[best].x;
      *y = g_freed[best].y;
      g_freed[best] = g_freed[--g_nfreed];
      return 0;
    }

  return -1;
}

void fgl_space_free(int x, int y, int w, int h)
{
  if (g_nfreed < FREED)
    {
      g_freed[g_nfreed].x = x;
      g_freed[g_nfreed].y = y;
      g_freed[g_nfreed].w = (w + 1) & ~1;
      g_freed[g_nfreed].h = h;
      g_nfreed++;
    }
}

/****************************************************************************
 * Textures
 ****************************************************************************/

static struct fgl_tex_s *bound(GLenum target)
{
  if (target != GL_TEXTURE_2D)
    {
      fgl_error(GL_INVALID_ENUM);
      return NULL;
    }

  if (g_fgl.bound >= FGL_TEXTURES || !g_fgl.tex[g_fgl.bound].named)
    {
      fgl_error(GL_INVALID_OPERATION);
      return NULL;
    }

  return &g_fgl.tex[g_fgl.bound];
}

void glGenTextures(GLsizei n, GLuint *textures)
{
  GLuint t = 1;                           /* (0 is no texture) */
  int i;

  for (i = 0; i < n; i++)
    {
      while (t < FGL_RECT && g_fgl.tex[t].named)
        {
          t++;
        }

      if (t >= FGL_RECT)
        {
          fgl_error(GL_OUT_OF_MEMORY);
          textures[i] = 0;
          continue;
        }

      memset(&g_fgl.tex[t], 0, sizeof(g_fgl.tex[t]));
      g_fgl.tex[t].named = true;
      g_fgl.tex[t].wrap_s = GL_REPEAT;
      g_fgl.tex[t].wrap_t = GL_REPEAT;
      textures[i] = t;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
  int i;

  for (i = 0; i < n; i++)
    {
      GLuint t = textures[i];

      if (t == 0 || t >= FGL_RECT || !g_fgl.tex[t].named)
        {
          continue;
        }

      if (g_fgl.tex[t].alloc)
        {
          fgl_space_free(g_fgl.tex[t].x, g_fgl.tex[t].y, g_fgl.tex[t].aw,
                         g_fgl.tex[t].ah);
        }

      memset(&g_fgl.tex[t], 0, sizeof(g_fgl.tex[t]));
      if (g_fgl.bound == t)
        {
          g_fgl.bound = 0;
        }
    }
}

void glBindTexture(GLenum target, GLuint texture)
{
  if (target != GL_TEXTURE_2D)
    {
      fgl_error(GL_INVALID_ENUM);
      return;
    }

  if (texture >= FGL_RECT)                /* (FGL_RECT: fglBindRect()'s) */
    {
      fgl_error(GL_INVALID_VALUE);
      return;
    }

  if (texture != 0 && !g_fgl.tex[texture].named)
    {
      memset(&g_fgl.tex[texture], 0, sizeof(g_fgl.tex[texture]));
      g_fgl.tex[texture].named = true;
      g_fgl.tex[texture].wrap_s = GL_REPEAT;
      g_fgl.tex[texture].wrap_t = GL_REPEAT;
    }

  g_fgl.bound = texture;
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
  struct fgl_tex_s *t = bound(target);

  if (t == NULL)
    {
      return;
    }

  switch (pname)
    {
      case GL_TEXTURE_WRAP_S:
        t->wrap_s = param == GL_REPEAT ? GL_REPEAT : GL_CLAMP;
        break;

      case GL_TEXTURE_WRAP_T:
        t->wrap_t = param == GL_REPEAT ? GL_REPEAT : GL_CLAMP;
        break;

      case GL_TEXTURE_MAG_FILTER:
      case GL_TEXTURE_MIN_FILTER:         /* (always the nearest texel) */
        break;

      default:
        fgl_error(GL_INVALID_ENUM);
        break;
    }
}

/* One row of w texels of the format into RGB565 */

static void row565(uint16_t *o, const uint8_t *p, int w, GLenum format,
                   GLenum type)
{
  int i;

  for (i = 0; i < w; i++)
    {
      if (type == GL_UNSIGNED_SHORT_5_6_5)
        {
          o[i] = ((const uint16_t *)p)[i];
        }
      else if (format == GL_RGB)
        {
          o[i] = (uint16_t)((p[3 * i] >> 3) << 11 | (p[3 * i + 1] >> 2) << 5 |
                            p[3 * i + 2] >> 3);
        }
      else if (format == GL_RGBA)
        {
          o[i] = p[4 * i + 3] < 128 ? g_fgl.key :
                 (uint16_t)((p[4 * i] >> 3) << 11 | (p[4 * i + 1] >> 2) << 5 |
                            p[4 * i + 2] >> 3);
        }
      else                                /* GL_LUMINANCE */
        {
          o[i] = (uint16_t)((p[i] >> 3) << 11 | (p[i] >> 2) << 5 | p[i] >> 3);
        }
    }
}

static int bytes_per(GLenum format, GLenum type)
{
  return type == GL_UNSIGNED_SHORT_5_6_5 ? 2 : format == GL_RGB ? 3 :
         format == GL_RGBA ? 4 : 1;
}

static bool format_ok(GLenum format, GLenum type)
{
  return (type == GL_UNSIGNED_SHORT_5_6_5 && format == GL_RGB) ||
         (type == GL_UNSIGNED_BYTE && (format == GL_RGB || format == GL_RGBA ||
                                       format == GL_LUMINANCE ||
                                       format == GL_COLOR_INDEX));
}

/* Rows of the image into the texture's place at (x, y) of it */

static void upload(struct fgl_tex_s *t, int x, int y, int w, int h,
                   GLenum format, GLenum type, const uint8_t *p)
{
  static uint16_t row[SPACE];
  size_t stride = (size_t)w * bytes_per(format, type);
  int j;

  fgl_unlock();                           /* (PUTs are commands, not jobs) */
  if (format == GL_COLOR_INDEX && !(x & 1) && !(w & 1))
    {
      fpc_gpu_put8(t->x + x, t->y + y, w, h, p, stride, g_fgl.pal);
      return;
    }

  for (j = 0; j < h; j++)
    {
      if (format == GL_COLOR_INDEX)
        {
          int i;

          for (i = 0; i < w; i++)
            {
              row[i] = g_fgl.pal[p[(size_t)j * stride + i]];
            }
        }
      else
        {
          row565(row, p + (size_t)j * stride, w, format, type);
        }

      fpc_gpu_put(t->x + x, t->y + y + j, w, 1, row, (size_t)w * 2);
    }
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei w, GLsizei h, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels)
{
  struct fgl_tex_s *t = bound(target);

  (void)internalformat;
  if (t == NULL || level != 0)            /* (mipmaps: only the base level) */
    {
      return;
    }

  if (w < 1 || h < 1 || w > SPACE || h > SPACE || border != 0)
    {
      fgl_error(GL_INVALID_VALUE);
      return;
    }

  if (!format_ok(format, type))
    {
      fgl_error(GL_INVALID_ENUM);
      return;
    }

  if (!t->alloc || t->aw < ((w + 1) & ~1) || t->ah < h)
    {
      if (t->alloc)
        {
          fgl_space_free(t->x, t->y, t->aw, t->ah);
          t->alloc = false;
        }

      if (fgl_space_alloc(w, h, &t->x, &t->y) < 0)
        {
          fgl_error(GL_OUT_OF_MEMORY);
          return;
        }

      t->aw = (int16_t)((w + 1) & ~1);
      t->ah = (int16_t)h;
      t->alloc = true;
    }

  t->w = (int16_t)w;
  t->h = (int16_t)h;
  g_fgl.st_ok = false;                    /* (its place may have changed) */
  if (pixels != NULL)
    {
      upload(t, 0, 0, w, h, format, type, pixels);
    }
}

void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                     GLsizei w, GLsizei h, GLenum format, GLenum type,
                     const GLvoid *pixels)
{
  struct fgl_tex_s *t = bound(target);

  if (t == NULL || level != 0 || !t->alloc)
    {
      return;
    }

  if (x < 0 || y < 0 || w < 0 || h < 0 || x + w > t->w || y + h > t->h)
    {
      fgl_error(GL_INVALID_VALUE);
      return;
    }

  if (!format_ok(format, type))
    {
      fgl_error(GL_INVALID_ENUM);
      return;
    }

  if (w > 0 && h > 0 && pixels != NULL)
    {
      upload(t, x, y, w, h, format, type, pixels);
    }
}
