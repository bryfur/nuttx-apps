/****************************************************************************
 * apps/graphics/fpcgl/fgl_core.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fpcgl's context, state and vertex pipeline: vertices transformed into
 * clip space, assembled into polygons, clipped against the view volume's
 * six planes, culled, projected into the picture and set up as the 3D
 * GPU's TRI jobs (fgl_setup.c), with a STATE job whenever the render state
 * they need changes.  The display: two buffers flipped (SET_SCALE shows
 * the picture scaled up) and a depth buffer, in the space fpcgl has to
 * itself while the 3D GPU runs.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <math.h>
#include <string.h>

#include "fgl_internal.h"
#include "fpc_gpu.h"

struct fgl_s g_fgl;

/****************************************************************************
 * Helpers
 ****************************************************************************/

void fgl_error(GLenum e)
{
  if (g_fgl.err == GL_NO_ERROR)
    {
      g_fgl.err = e;
    }
}

void fgl_lock(void)
{
  if (!g_fgl.locked)
    {
      fpc_gpu_jobs_lock();
      g_fgl.locked = true;
    }
}

void fgl_unlock(void)
{
  if (g_fgl.locked)
    {
      fpc_gpu_jobs_unlock();
      g_fgl.locked = false;
    }
}

static void job(const uint32_t *w, int n)
{
  uint32_t *j;

  fgl_lock();
  j = fpc_gpu_jobs_room(n);
  memcpy(j, w, (size_t)n * 4);
}

static uint16_t rgb565(float r, float g, float b)
{
  int ir = (int)(r * 31.0f + 0.5f);
  int ig = (int)(g * 63.0f + 0.5f);
  int ib = (int)(b * 31.0f + 0.5f);

  ir = ir < 0 ? 0 : ir > 31 ? 31 : ir;
  ig = ig < 0 ? 0 : ig > 63 ? 63 : ig;
  ib = ib < 0 ? 0 : ib > 31 ? 31 : ib;
  return (uint16_t)(ir << 11 | ig << 5 | ib);
}

/* The depth buffer's value for a reversed depth zr (1 - GL's depth, 0..1):
 * TRI's z = zr 65535 << 16, stored as ZFLOAT's 16-bit floating point
 */

static uint16_t zfloat(float zr)
{
  uint32_t z = (uint32_t)(zr * 65535.0f) << 16;
  uint32_t l;

  if (z < (1u << 17))
    {
      return (uint16_t)(z >> 5);
    }

  l = (uint32_t)__builtin_clz(z);
  return (uint16_t)((15 - l) << 12 | (((z << (l + 1)) >> 20) & 4095));
}

static void mat_mul(float *out, const float *a, const float *b)
{
  float t[16];
  int c;
  int r;

  for (c = 0; c < 4; c++)
    {
      for (r = 0; r < 4; r++)
        {
          t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                         a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
        }
    }

  memcpy(out, t, sizeof(t));
}

static float *mat_top(void)
{
  return g_fgl.matmode == GL_PROJECTION ? g_fgl.pr[g_fgl.prsp] :
                                          g_fgl.mv[g_fgl.mvsp];
}

static void mat_ident(float *m)
{
  memset(m, 0, 16 * sizeof(float));
  m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/****************************************************************************
 * fpcgl's own
 ****************************************************************************/

int fglInit(int w, int h, int mode)
{
  int mw;
  int mh;
  int s;
  int ret;
  int k;

  if (g_fgl.on || w < 16 || h < 16 || w > 1024 || h > 1024 ||
      fpc_gpu_modesize(mode, &mw, &mh) < 0)
    {
      return -EINVAL;
    }

  ret = fpc_gpu_game_display(true);
  if (ret < 0)
    {
      return ret;
    }

  ret = fpc_gpu_switch(1);
  if (ret < 0)
    {
      fpc_gpu_game_display(false);
      return -ENODEV;
    }

  memset(&g_fgl, 0, sizeof(g_fgl));
  s = mw / w < mh / h ? mw / w : mh / h;
  s = s < 1 ? 1 : s > 4 ? 4 : s;
  g_fgl.w  = w;
  g_fgl.h  = h;
  g_fgl.dw = (mw + s - 1) / s;
  g_fgl.dh = (mh + s - 1) / s;
  g_fgl.ox = (g_fgl.dw - w) / 2;
  g_fgl.oy = (g_fgl.dh - h) / 2;

  fgl_space_reset();
  if (fgl_space_alloc(g_fgl.dw, g_fgl.dh, &g_fgl.bx[0], &g_fgl.by[0]) < 0 ||
      fgl_space_alloc(g_fgl.dw, g_fgl.dh, &g_fgl.bx[1], &g_fgl.by[1]) < 0 ||
      fgl_space_alloc(w, h, &g_fgl.zx, &g_fgl.zy) < 0)
    {
      fpc_gpu_switch(0);
      fpc_gpu_game_display(false);
      return -ENOMEM;
    }

  g_fgl.on = true;
  g_fgl.back = 1;
  g_fgl.vw = w;
  g_fgl.vh = h;
  g_fgl.dnear = 0.0f;
  g_fgl.dfar = 1.0f;
  g_fgl.depth_mask = true;
  g_fgl.depth_func = GL_LESS;
  g_fgl.cull_face = GL_BACK;
  g_fgl.front_face = GL_CCW;
  g_fgl.shade_model = GL_SMOOTH;
  g_fgl.clear_depth = zfloat(0.0f);       /* (GL's 1.0) */
  g_fgl.dither = true;
  g_fgl.sfactor = GL_ONE;
  g_fgl.dfactor = GL_ZERO;
  g_fgl.alpha_func = GL_ALWAYS;
  g_fgl.fog_mode = GL_EXP;
  g_fgl.fog_density = 1.0f;
  g_fgl.fog_end = 1.0f;
  g_fgl.ca = 1.0f;
  g_fgl.key = FGL_KEY;
  g_fgl.matmode = GL_MODELVIEW;
  mat_ident(g_fgl.mv[0]);
  mat_ident(g_fgl.pr[0]);
  g_fgl.mvp_dirty = true;
  g_fgl.cr = g_fgl.cg = g_fgl.cb = 1.0f;
  for (k = 0; k < 256; k++)
    {
      g_fgl.pal[k] = (uint16_t)((k >> 3) << 11 | (k >> 2) << 5 | (k >> 3));
    }

  /* Both buffers black (their margins stay so), the first shown */

  fpc_gpu_game_mode(mode);
  fpc_gpu_set_scale(s, s);
  for (k = 0; k < 2; k++)
    {
      uint32_t f[4] =
      {
        1, (uint32_t)g_fgl.bx[k] | (uint32_t)g_fgl.by[k] << 16,
        (uint32_t)g_fgl.dw | (uint32_t)g_fgl.dh << 16, 0
      };

      job(f, 4);
    }

  fgl_unlock();
  fpc_gpu_game_flip(g_fgl.bx[0], g_fgl.by[0]);
  fpc_gpu_flush();
  return OK;
}

void fglShutdown(void)
{
  if (!g_fgl.on)
    {
      return;
    }

  fgl_unlock();
  fpc_gpu_flush();
  g_fgl.on = false;
  fpc_gpu_switch(0);
  fpc_gpu_game_display(false);
}

void fglSwapBuffers(void)
{
  if (!g_fgl.on)
    {
      return;
    }

  fgl_unlock();
  fpc_gpu_game_flip(g_fgl.bx[g_fgl.back], g_fgl.by[g_fgl.back]);
  fpc_gpu_flush();
  g_fgl.back ^= 1;
  g_fgl.st_ok = false;                    /* (its target moved) */
}

int fglWidth(void)
{
  return g_fgl.w;
}

int fglHeight(void)
{
  return g_fgl.h;
}

void fglColorKey(uint16_t key)
{
  g_fgl.key = key;
}

void fglColorTable(const uint16_t pal[256])
{
  memcpy(g_fgl.pal, pal, sizeof(g_fgl.pal));
}

uint32_t fglTriangles(void)
{
  return g_fgl.ntri;
}

/****************************************************************************
 * State
 ****************************************************************************/

GLenum glGetError(void)
{
  GLenum e = g_fgl.err;

  g_fgl.err = GL_NO_ERROR;
  return e;
}

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
  if (w < 0 || h < 0)
    {
      fgl_error(GL_INVALID_VALUE);
      return;
    }

  g_fgl.vx = x;
  g_fgl.vy = y;
  g_fgl.vw = w;
  g_fgl.vh = h;
}

void glDepthRange(GLclampf znear, GLclampf zfar)
{
  g_fgl.dnear = znear < 0.0f ? 0.0f : znear > 1.0f ? 1.0f : znear;
  g_fgl.dfar = zfar < 0.0f ? 0.0f : zfar > 1.0f ? 1.0f : zfar;
}

void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
  (void)a;
  g_fgl.clear_colour = rgb565(r, g, b);
}

void glClearDepth(GLclampf d)
{
  d = d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
  g_fgl.clear_depth = zfloat(1.0f - d);
}

void glClear(GLbitfield mask)
{
  if (!g_fgl.on)
    {
      return;
    }

  if (mask & GL_COLOR_BUFFER_BIT)
    {
      uint32_t f[4] =
      {
        1 | (uint32_t)g_fgl.clear_colour << 16,
        (uint32_t)(g_fgl.bx[g_fgl.back] + g_fgl.ox) |
        (uint32_t)(g_fgl.by[g_fgl.back] + g_fgl.oy) << 16,
        (uint32_t)g_fgl.w | (uint32_t)g_fgl.h << 16, 0
      };

      job(f, 4);
    }

  if (mask & GL_DEPTH_BUFFER_BIT)
    {
      uint32_t f[4] =
      {
        1 | (uint32_t)g_fgl.clear_depth << 16,
        (uint32_t)g_fgl.zx | (uint32_t)g_fgl.zy << 16,
        (uint32_t)g_fgl.w | (uint32_t)g_fgl.h << 16, 0
      };

      job(f, 4);
    }
}

void glFlush(void)
{
  fgl_unlock();
  fpc_gpu_flush();
}

void glFinish(void)
{
  glFlush();
}

static bool *cap_flag(GLenum cap)
{
  switch (cap)
    {
      case GL_DEPTH_TEST: return &g_fgl.depth_test;
      case GL_CULL_FACE:  return &g_fgl.cull;
      case GL_TEXTURE_2D: return &g_fgl.tex2d;
      case GL_ALPHA_TEST: return &g_fgl.alpha_test;
      case GL_BLEND:      return &g_fgl.blend;
      case GL_FOG:        return &g_fgl.fog;
      case GL_DITHER:     return &g_fgl.dither;
      case FGL_TEXTURE_DITHER: return &g_fgl.tdither;
      default:            return NULL;
    }
}

void glEnable(GLenum cap)
{
  bool *f = cap_flag(cap);

  if (f == NULL)
    {
      fgl_error(GL_INVALID_ENUM);
      return;
    }

  *f = true;
}

void glDisable(GLenum cap)
{
  bool *f = cap_flag(cap);

  if (f == NULL)
    {
      fgl_error(GL_INVALID_ENUM);
      return;
    }

  *f = false;
}

void glDepthFunc(GLenum func)
{
  if (func != GL_LESS && func != GL_LEQUAL && func != GL_ALWAYS)
    {
      fgl_error(GL_INVALID_ENUM);
      return;
    }

  g_fgl.depth_func = func;
}

void glDepthMask(GLboolean flag)
{
  g_fgl.depth_mask = flag != GL_FALSE;
}

void glCullFace(GLenum mode)
{
  g_fgl.cull_face = mode;
}

void glFrontFace(GLenum mode)
{
  g_fgl.front_face = mode;
}

void glShadeModel(GLenum mode)
{
  g_fgl.shade_model = mode;
}

static int blend_code(GLenum f)
{
  switch (f)
    {
      case GL_ZERO:                return 0;
      case GL_ONE:                 return 1;
      case GL_SRC_COLOR:           return 2;
      case GL_ONE_MINUS_SRC_COLOR: return 3;
      case GL_DST_COLOR:           return 4;
      case GL_ONE_MINUS_DST_COLOR: return 5;
      case GL_SRC_ALPHA:           return 6;
      case GL_ONE_MINUS_SRC_ALPHA: return 7;
      case GL_DST_ALPHA:           return 1;    /* (the buffer's alpha is 1) */
      case GL_ONE_MINUS_DST_ALPHA: return 0;
      default:                     return -1;
    }
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
  if (blend_code(sfactor) < 0 || blend_code(dfactor) < 0)
    {
      fgl_error(GL_INVALID_ENUM);
      return;
    }

  g_fgl.sfactor = sfactor;
  g_fgl.dfactor = dfactor;
}

void glAlphaFunc(GLenum func, GLclampf ref)
{
  if (func != GL_GREATER && func != GL_GEQUAL && func != GL_ALWAYS)
    {
      fgl_error(GL_INVALID_ENUM);
      return;
    }

  g_fgl.alpha_func = func;
  g_fgl.alpha_ref = ref < 0.0f ? 0.0f : ref > 1.0f ? 1.0f : ref;
}

void glFogf(GLenum pname, GLfloat param)
{
  switch (pname)
    {
      case GL_FOG_MODE:
        glFogi(pname, (GLint)param);
        break;

      case GL_FOG_DENSITY:
        g_fgl.fog_density = param;
        break;

      case GL_FOG_START:
        g_fgl.fog_start = param;
        break;

      case GL_FOG_END:
        g_fgl.fog_end = param;
        break;

      default:
        fgl_error(GL_INVALID_ENUM);
        break;
    }
}

void glFogi(GLenum pname, GLint param)
{
  if (pname != GL_FOG_MODE)
    {
      glFogf(pname, (GLfloat)param);
      return;
    }

  if (param != GL_LINEAR && param != GL_EXP && param != GL_EXP2)
    {
      fgl_error(GL_INVALID_ENUM);
      return;
    }

  g_fgl.fog_mode = (GLenum)param;
}

void glFogfv(GLenum pname, const GLfloat *params)
{
  if (pname == GL_FOG_COLOR)
    {
      g_fgl.fog_colour = rgb565(params[0], params[1], params[2]);
    }
  else
    {
      glFogf(pname, params[0]);
    }
}

/* A vertex's fog amount (0 none .. 1) at eye distance d (GL's fog factor
 * is 1 - it)
 */

static float fog_amount(float d)
{
  float f;

  switch (g_fgl.fog_mode)
    {
      case GL_LINEAR:
        f = g_fgl.fog_end != g_fgl.fog_start ?
            (g_fgl.fog_end - d) / (g_fgl.fog_end - g_fgl.fog_start) : 1.0f;
        break;

      case GL_EXP:
        f = expf(-g_fgl.fog_density * d);
        break;

      default:
        f = expf(-(g_fgl.fog_density * d) * (g_fgl.fog_density * d));
        break;
    }

  f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
  return 1.0f - f;
}

/****************************************************************************
 * Matrices
 ****************************************************************************/

void glMatrixMode(GLenum mode)
{
  if (mode != GL_MODELVIEW && mode != GL_PROJECTION)
    {
      fgl_error(GL_INVALID_ENUM);
      return;
    }

  g_fgl.matmode = mode;
}

void glLoadIdentity(void)
{
  mat_ident(mat_top());
  g_fgl.mvp_dirty = true;
}

void glLoadMatrixf(const GLfloat *m)
{
  memcpy(mat_top(), m, 16 * sizeof(float));
  g_fgl.mvp_dirty = true;
}

void glMultMatrixf(const GLfloat *m)
{
  float *t = mat_top();

  mat_mul(t, t, m);
  g_fgl.mvp_dirty = true;
}

void glPushMatrix(void)
{
  int *sp = g_fgl.matmode == GL_PROJECTION ? &g_fgl.prsp : &g_fgl.mvsp;
  float (*st)[16] = g_fgl.matmode == GL_PROJECTION ? g_fgl.pr : g_fgl.mv;

  if (*sp + 1 >= FGL_STACK)
    {
      fgl_error(GL_STACK_OVERFLOW);
      return;
    }

  memcpy(st[*sp + 1], st[*sp], 16 * sizeof(float));
  (*sp)++;
}

void glPopMatrix(void)
{
  int *sp = g_fgl.matmode == GL_PROJECTION ? &g_fgl.prsp : &g_fgl.mvsp;

  if (*sp == 0)
    {
      fgl_error(GL_STACK_UNDERFLOW);
      return;
    }

  (*sp)--;
  g_fgl.mvp_dirty = true;
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
  float m[16];

  mat_ident(m);
  m[12] = x;
  m[13] = y;
  m[14] = z;
  glMultMatrixf(m);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
  float m[16];

  mat_ident(m);
  m[0] = x;
  m[5] = y;
  m[10] = z;
  glMultMatrixf(m);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
  float m[16];
  float len = sqrtf(x * x + y * y + z * z);
  float a = angle * (float)M_PI / 180.0f;
  float c = cosf(a);
  float s = sinf(a);
  float ic = 1.0f - c;

  if (len == 0.0f)
    {
      return;
    }

  x /= len;
  y /= len;
  z /= len;
  mat_ident(m);
  m[0] = x * x * ic + c;     m[4] = x * y * ic - z * s; m[8]  = x * z * ic + y * s;
  m[1] = y * x * ic + z * s; m[5] = y * y * ic + c;     m[9]  = y * z * ic - x * s;
  m[2] = x * z * ic - y * s; m[6] = y * z * ic + x * s; m[10] = z * z * ic + c;
  glMultMatrixf(m);
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
               GLdouble n, GLdouble f)
{
  float m[16];

  if (n <= 0.0 || f <= 0.0 || l == r || b == t || n == f)
    {
      fgl_error(GL_INVALID_VALUE);
      return;
    }

  memset(m, 0, sizeof(m));
  m[0]  = (float)(2 * n / (r - l));
  m[5]  = (float)(2 * n / (t - b));
  m[8]  = (float)((r + l) / (r - l));
  m[9]  = (float)((t + b) / (t - b));
  m[10] = (float)(-(f + n) / (f - n));
  m[11] = -1.0f;
  m[14] = (float)(-2 * f * n / (f - n));
  glMultMatrixf(m);
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
             GLdouble n, GLdouble f)
{
  float m[16];

  if (l == r || b == t || n == f)
    {
      fgl_error(GL_INVALID_VALUE);
      return;
    }

  mat_ident(m);
  m[0]  = (float)(2 / (r - l));
  m[5]  = (float)(2 / (t - b));
  m[10] = (float)(-2 / (f - n));
  m[12] = (float)(-(r + l) / (r - l));
  m[13] = (float)(-(t + b) / (t - b));
  m[14] = (float)(-(f + n) / (f - n));
  glMultMatrixf(m);
}

/****************************************************************************
 * The pipeline
 ****************************************************************************/

/* The render state a polygon needs, as STATE's words: sent if it is not
 * what the GPU has.  Returns the TRI flags it goes with.
 */

static int state_for(bool shade, bool mono, bool alpha)
{
  struct fgl_tex_s *t = g_fgl.bound < FGL_TEXTURES ? &g_fgl.tex[g_fgl.bound] : NULL;
  bool tex = g_fgl.tex2d && t != NULL && t->alloc;
  bool ztest = g_fgl.depth_test && g_fgl.depth_func != GL_ALWAYS;
  bool zwrite = g_fgl.depth_test && g_fgl.depth_mask;
  bool clampm = tex && (t->wrap_s != GL_REPEAT || t->wrap_t != GL_REPEAT ||
                        (t->w & (t->w - 1)) || (t->h & (t->h - 1)));
  bool fmt_alpha = tex && t->fmt != 0;
  bool key = tex && !fmt_alpha && g_fgl.alpha_test;
  bool atest = g_fgl.alpha_test && g_fgl.alpha_func != GL_ALWAYS &&
               (fmt_alpha || alpha);
  int aref = 0;
  uint32_t mode;
  uint32_t st[8];
  int fl = 0;

  if (atest)
    {
      /* alpha > ref: at least floor(ref 255) + 1; >= ref: ceil(ref 255) */

      float r = g_fgl.alpha_ref * 255.0f;

      aref = g_fgl.alpha_func == GL_GREATER ? (int)r + 1 : (int)ceilf(r);
      aref = aref > 255 ? 255 : aref;
    }

  shade |= !tex;                          /* (untextured: white times the colour) */

  /* The depth reversed and floating point (ZREV, ZFLOAT): GL's < is > */

  mode = (uint32_t)(ztest | (g_fgl.depth_func == GL_LEQUAL) << 1 | zwrite << 2 |
                    tex << 3 | shade << 4 | key << 5 | clampm << 6 |
                    g_fgl.blend << 7 | g_fgl.fog << 8 | g_fgl.dither << 9 |
                    (tex && g_fgl.tdither) << 10 | atest << 11 | 1 << 12 |
                    1 << 13 | (tex ? t->fmt : 0) << 14);
  st[0] = 10 | mode << 8;
  st[1] = (uint32_t)(g_fgl.bx[g_fgl.back] + g_fgl.ox) |
          (uint32_t)(g_fgl.by[g_fgl.back] + g_fgl.oy) << 16;
  st[2] = (uint32_t)g_fgl.zx | (uint32_t)g_fgl.zy << 16;
  st[3] = tex ? (uint32_t)t->x | (uint32_t)t->y << 16 : 0;
  st[4] = tex ? (uint32_t)t->w | (uint32_t)t->h << 16 : 1 | 1 << 16;
  st[5] = g_fgl.key;
  st[6] = (uint32_t)g_fgl.w | (uint32_t)g_fgl.h << 16;
  st[7] = (uint32_t)blend_code(g_fgl.sfactor) |
          (uint32_t)blend_code(g_fgl.dfactor) << 4 | (uint32_t)aref << 8 |
          (uint32_t)g_fgl.fog_colour << 16;
  if (!g_fgl.st_ok || memcmp(st, g_fgl.st, sizeof(st)) != 0)
    {
      job(st, 8);
      memcpy(g_fgl.st, st, sizeof(st));
      g_fgl.st_ok = true;
    }

  if (ztest || zwrite)
    {
      fl |= FGL_TRI_Z;
    }

  if (tex)
    {
      fl |= FGL_TRI_UV;
    }

  if (shade)
    {
      fl |= mono ? FGL_TRI_MONO : FGL_TRI_RGB;
    }

  if (alpha)
    {
      fl |= FGL_TRI_A;
    }

  if (g_fgl.fog)
    {
      fl |= FGL_TRI_F;
    }

  return fl;
}

/* Sutherland-Hodgman against plane k (0-5: w + x, w - x, w + y, w - y,
 * w + z, w - z >= 0), in clip space
 */

static float plane_d(const struct fgl_v_s *v, int k)
{
  switch (k)
    {
      case 0:  return v->w + v->x;
      case 1:  return v->w - v->x;
      case 2:  return v->w + v->y;
      case 3:  return v->w - v->y;
      case 4:  return v->w + v->z;
      default: return v->w - v->z;
    }
}

static int clip_plane(const struct fgl_v_s *in, int n, struct fgl_v_s *out,
                      int k)
{
  int m = 0;
  int i;

  for (i = 0; i < n; i++)
    {
      const struct fgl_v_s *p = &in[i];
      const struct fgl_v_s *q = &in[(i + 1) % n];
      float dp = plane_d(p, k);
      float dq = plane_d(q, k);

      if (dp >= 0.0f)
        {
          out[m++] = *p;
        }

      if ((dp >= 0.0f) != (dq >= 0.0f))
        {
          const float *a = &p->x;
          const float *b = &q->x;
          float *o = &out[m].x;
          float t = dp / (dp - dq);
          int f;

          for (f = 0; f < FGL_V_FLOATS; f++)
            {
              o[f] = a[f] + (b[f] - a[f]) * t;
            }

          m++;
        }
    }

  return m;
}

/* A convex polygon in clip space (provoking: the vertex whose colour a
 * flat polygon has)
 */

static void polygon(const struct fgl_v_s *v, int n, int provoking)
{
  static struct fgl_v_s a[72];
  static struct fgl_v_s b[72];
  static struct fgl_svert_s sv[72];
  struct fgl_tex_s *t = g_fgl.bound < FGL_TEXTURES ? &g_fgl.tex[g_fgl.bound] : NULL;
  float tw = t != NULL && t->alloc ? (float)t->w : 1.0f;
  float th = t != NULL && t->alloc ? (float)t->h : 1.0f;
  float pr = v[provoking].r;
  float pg = v[provoking].g;
  float pb = v[provoking].b;
  float pa = v[provoking].a;
  float area = 0.0f;
  bool shade = false;
  bool mono = true;
  bool alpha = false;
  bool wrap;
  int fl;
  int k;
  int i;

  if (n < 3 || n > 64)
    {
      return;
    }

  memcpy(a, v, (size_t)n * sizeof(a[0]));
  for (k = 0; k < 6 && n >= 3; k++)
    {
      n = clip_plane(a, n, b, k);
      memcpy(a, b, (size_t)n * sizeof(a[0]));
    }

  if (n < 3)
    {
      return;
    }

  /* Projected: the picture's pixels from its top left (GL's window y is
   * from the bottom), depth 0..65535
   */

  for (i = 0; i < n; i++)
    {
      float iw = 1.0f / a[i].w;
      float d = (a[i].z * iw * 0.5f + 0.5f) * (g_fgl.dfar - g_fgl.dnear) +
                g_fgl.dnear;

      sv[i].x = (float)g_fgl.vx + (a[i].x * iw * 0.5f + 0.5f) * (float)g_fgl.vw;
      sv[i].y = (float)g_fgl.h - ((float)g_fgl.vy +
                                  (a[i].y * iw * 0.5f + 0.5f) * (float)g_fgl.vh);
      d = d < 0.0f ? 0.0f : d > 1.0f ? 1.0f : d;
      sv[i].z = (1.0f - d) * 65535.0f;    /* (reversed: ZREV, ZFLOAT) */
      sv[i].q = iw;
      sv[i].u = a[i].s * tw;
      sv[i].v = a[i].t * th;
      if (g_fgl.shade_model == GL_FLAT)
        {
          a[i].r = pr;
          a[i].g = pg;
          a[i].b = pb;
          a[i].a = pa;
        }

      sv[i].r = a[i].r;
      sv[i].g = a[i].g;
      sv[i].b = a[i].b;
      sv[i].a = a[i].a;
      sv[i].f = a[i].f;
      alpha |= a[i].a != 1.0f;
      shade |= a[i].r != 1.0f || a[i].g != 1.0f || a[i].b != 1.0f;
      mono &= a[i].r == a[i].g && a[i].g == a[i].b;
    }

  /* Culled by its winding on the screen (y down: GL's counter-clockwise
   * is clockwise here, a negative area)
   */

  for (i = 0; i < n; i++)
    {
      const struct fgl_svert_s *p = &sv[i];
      const struct fgl_svert_s *q = &sv[(i + 1) % n];

      area += p->x * q->y - q->x * p->y;
    }

  if (g_fgl.cull)
    {
      bool front = g_fgl.front_face == GL_CCW ? area < 0.0f : area > 0.0f;

      if (g_fgl.cull_face == GL_FRONT_AND_BACK ||
          (g_fgl.cull_face == GL_BACK && !front) ||
          (g_fgl.cull_face == GL_FRONT && front))
        {
          return;
        }
    }

  fl = state_for(shade, mono, alpha);
  wrap = (fl & FGL_TRI_UV) && !(g_fgl.st[0] & (64 << 8));

  /* One TRI for each fan of up to FGL_POLY_MAX vertices from the first
   * (most polygons: one), by triangles where one needs more segments than
   * a TRI has
   */

  for (i = 1; i + 1 < n; )
    {
      struct fgl_svert_s c[FGL_POLY_MAX];
      uint32_t j[FGL_TRI_MAX];
      int m = n - i + 1 < FGL_POLY_MAX ? n - i + 1 : FGL_POLY_MAX;
      int nw;

      c[0] = sv[0];
      memcpy(&c[1], &sv[i], (size_t)(m - 1) * sizeof(c[0]));
      nw = fgl_setup_poly(c, m, fl, wrap ? (int)tw : 0, wrap ? (int)th : 0, j);
      if (nw > 0)
        {
          job(j, nw);
          g_fgl.ntri++;
        }
      else if (nw < 0)
        {
          for (k = 1; k + 1 < m; k++)
            {
              nw = fgl_setup(&c[0], &c[k], &c[k + 1], fl,
                             wrap ? (int)tw : 0, wrap ? (int)th : 0, j);
              if (nw > 0)
                {
                  job(j, nw);
                  g_fgl.ntri++;
                }
            }
        }

      i += m - 2;
    }
}

void glBegin(GLenum mode)
{
  if (g_fgl.inprim)
    {
      fgl_error(GL_INVALID_OPERATION);
      return;
    }

  if (g_fgl.mvp_dirty)
    {
      mat_mul(g_fgl.mvp, g_fgl.pr[g_fgl.prsp], g_fgl.mv[g_fgl.mvsp]);
      g_fgl.mvp_dirty = false;
    }

  g_fgl.prim = mode;
  g_fgl.inprim = true;
  g_fgl.nv = 0;
  g_fgl.npoly = 0;
}

void glEnd(void)
{
  if (!g_fgl.inprim)
    {
      fgl_error(GL_INVALID_OPERATION);
      return;
    }

  if (g_fgl.prim == GL_POLYGON && g_fgl.npoly >= 3 && g_fgl.on)
    {
      polygon(g_fgl.poly, g_fgl.npoly, 0);
    }

  g_fgl.inprim = false;
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
  const float *m = g_fgl.mvp;
  struct fgl_v_s v;
  struct fgl_v_s *pv = g_fgl.pv;

  if (!g_fgl.inprim || !g_fgl.on)
    {
      return;
    }

  v.x = m[0] * x + m[4] * y + m[8]  * z + m[12];
  v.y = m[1] * x + m[5] * y + m[9]  * z + m[13];
  v.z = m[2] * x + m[6] * y + m[10] * z + m[14];
  v.w = m[3] * x + m[7] * y + m[11] * z + m[15];
  v.s = g_fgl.cs;
  v.t = g_fgl.ct;
  v.r = g_fgl.cr;
  v.g = g_fgl.cg;
  v.b = g_fgl.cb;
  v.a = g_fgl.ca;
  v.f = 0.0f;
  if (g_fgl.fog)
    {
      /* The eye's distance along its view: -z in eye space */

      const float *e = g_fgl.mv[g_fgl.mvsp];

      v.f = fog_amount(-(e[2] * x + e[6] * y + e[10] * z + e[14]));
    }

  switch (g_fgl.prim)
    {
      case GL_TRIANGLES:
        pv[g_fgl.nv++] = v;
        if (g_fgl.nv == 3)
          {
            polygon(pv, 3, 2);
            g_fgl.nv = 0;
          }
        break;

      case GL_TRIANGLE_STRIP:
        if (g_fgl.nv < 2)
          {
            pv[g_fgl.nv++] = v;
          }
        else
          {
            struct fgl_v_s t[3];

            /* (every other one turned round, so all keep one winding) */

            t[0] = (g_fgl.nv & 1) ? pv[1] : pv[0];
            t[1] = (g_fgl.nv & 1) ? pv[0] : pv[1];
            t[2] = v;
            polygon(t, 3, 2);
            pv[0] = pv[1];
            pv[1] = v;
            g_fgl.nv++;
          }
        break;

      case GL_TRIANGLE_FAN:
        if (g_fgl.nv < 2)
          {
            pv[g_fgl.nv++] = v;
          }
        else
          {
            struct fgl_v_s t[3];

            t[0] = pv[0];
            t[1] = pv[1];
            t[2] = v;
            polygon(t, 3, 2);
            pv[1] = v;
          }
        break;

      case GL_QUADS:
        pv[g_fgl.nv++] = v;
        if (g_fgl.nv == 4)
          {
            polygon(pv, 4, 3);
            g_fgl.nv = 0;
          }
        break;

      case GL_QUAD_STRIP:
        if (g_fgl.nv < 2)
          {
            pv[g_fgl.nv++] = v;
          }
        else if (g_fgl.nv == 2)
          {
            pv[2] = v;
            g_fgl.nv = 3;
          }
        else
          {
            struct fgl_v_s t[4];

            t[0] = pv[0];
            t[1] = pv[1];
            t[2] = v;
            t[3] = pv[2];
            polygon(t, 4, 2);
            pv[0] = pv[2];
            pv[1] = v;
            g_fgl.nv = 2;
          }
        break;

      case GL_POLYGON:
        if (g_fgl.npoly < 64)
          {
            g_fgl.poly[g_fgl.npoly++] = v;
          }
        break;

      default:                            /* (points and lines: not drawn) */
        break;
    }
}

void glVertex2f(GLfloat x, GLfloat y)
{
  glVertex3f(x, y, 0.0f);
}

void glVertex3fv(const GLfloat *v)
{
  glVertex3f(v[0], v[1], v[2]);
}

void glTexCoord2f(GLfloat s, GLfloat t)
{
  g_fgl.cs = s;
  g_fgl.ct = t;
}

void glTexCoord2fv(const GLfloat *v)
{
  glTexCoord2f(v[0], v[1]);
}

void glColor3f(GLfloat r, GLfloat g, GLfloat b)
{
  g_fgl.cr = r < 0.0f ? 0.0f : r > 2.0f ? 2.0f : r;   /* (up to 2: overbright) */
  g_fgl.cg = g < 0.0f ? 0.0f : g > 2.0f ? 2.0f : g;
  g_fgl.cb = b < 0.0f ? 0.0f : b > 2.0f ? 2.0f : b;
  g_fgl.ca = 1.0f;
}

void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
  glColor3f(r, g, b);
  g_fgl.ca = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

void glColor3ub(GLubyte r, GLubyte g, GLubyte b)
{
  glColor3f(r / 255.0f, g / 255.0f, b / 255.0f);
}

void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
  glColor4f(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}
