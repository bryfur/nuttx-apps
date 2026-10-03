/****************************************************************************
 * apps/graphics/fpcgl/fgl_internal.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCGL_FGL_INTERNAL_H
#define __APPS_GRAPHICS_FPCGL_FGL_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "fpcgl.h"
#include "fgl_setup.h"

#define FGL_TEXTURES   512
#define FGL_RECT       (FGL_TEXTURES - 1) /* fglBindRect()'s */
#define FGL_STACK      16                 /* Each matrix stack's depth */

/* A texture: its image in the space (w x h at x, y; allocated aw x ah) */

struct fgl_tex_s
{
  bool    named;                          /* glGenTextures() gave it */
  bool    alloc;                          /* It has an image */
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  int16_t aw;
  int16_t ah;
  GLenum  wrap_s;
  GLenum  wrap_t;
  uint8_t fmt;                            /* STATE's TFMT: 0 RGB565, 1 ARGB4444, 2 ARGB1555 */
};

/* A vertex in clip space with its attributes: GL texture coordinates,
 * the colour and alpha, the fog amount (0 none .. 1)
 */

struct fgl_v_s
{
  float x;
  float y;
  float z;
  float w;
  float s;
  float t;
  float r;
  float g;
  float b;
  float a;
  float f;
};

#define FGL_V_FLOATS   11                 /* (clipping interpolates them all) */

struct fgl_s
{
  bool     on;                            /* fglInit() done */
  GLenum   err;
  int      w;                             /* The picture */
  int      h;
  int      dw;                            /* A display buffer (the mode / scale) */
  int      dh;
  int      ox;                            /* The picture in it */
  int      oy;
  int16_t  bx[2];                         /* The display buffers */
  int16_t  by[2];
  int      back;                          /* The one being drawn */
  int16_t  zx;                            /* The depth buffer */
  int16_t  zy;

  /* The viewport (GL: from the bottom left), the depth range */

  int      vx;
  int      vy;
  int      vw;
  int      vh;
  float    dnear;
  float    dfar;

  /* State */

  bool     depth_test;
  bool     depth_mask;
  bool     cull;
  bool     tex2d;
  bool     alpha_test;
  bool     blend;
  bool     fog;
  bool     dither;
  bool     tdither;                       /* FGL_TEXTURE_DITHER */
  GLenum   depth_func;
  GLenum   sfactor;
  GLenum   dfactor;
  GLenum   alpha_func;
  float    alpha_ref;
  GLenum   fog_mode;
  float    fog_start;
  float    fog_end;
  float    fog_density;
  uint16_t fog_colour;
  GLenum   cull_face;
  GLenum   front_face;
  GLenum   shade_model;
  uint16_t clear_colour;
  uint16_t clear_depth;
  uint16_t key;

  /* The matrices (column major, as GL's) */

  GLenum   matmode;
  float    mv[FGL_STACK][16];
  float    pr[FGL_STACK][16];
  int      mvsp;
  int      prsp;
  float    mvp[16];
  bool     mvp_dirty;

  /* The vertex being made, the primitive being assembled */

  float    cs;
  float    ct;
  float    cr;
  float    cg;
  float    cb;
  float    ca;
  GLenum   prim;
  bool     inprim;
  int      nv;                            /* Vertices of the primitive so far */
  struct fgl_v_s pv[4];                   /* What assembly keeps */
  struct fgl_v_s poly[64];                /* GL_POLYGON's */
  int      npoly;

  /* Textures; the STATE last sent */

  struct fgl_tex_s tex[FGL_TEXTURES];
  GLuint   bound;
  uint16_t pal[256];
  bool     locked;                        /* Between fpc_gpu_jobs_lock() and unlock */
  bool     st_ok;
  uint32_t st[8];
  uint32_t ntri;
};

extern struct fgl_s g_fgl;

void fgl_error(GLenum e);
void fgl_lock(void);
void fgl_unlock(void);

/* The space (fgl_tex.c): rectangles for the buffers and the textures */

void fgl_space_reset(void);
int  fgl_space_alloc(int w, int h, int16_t *x, int16_t *y);
void fgl_space_free(int x, int y, int w, int h);

#endif /* __APPS_GRAPHICS_FPCGL_FGL_INTERNAL_H */
