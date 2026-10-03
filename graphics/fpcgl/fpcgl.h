/****************************************************************************
 * apps/graphics/fpcgl/fpcgl.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fpcgl: an OpenGL 1.x subset on the FPC's 3D GPU (docs/gpu3d.md): the
 * GL calls a fixed-function renderer uses (immediate mode, the matrix
 * stacks, the depth test, culling, flat and smooth shading, textures
 * repeated or clamped, with alpha, blending, fog, the alpha test,
 * dithering), drawn by the GPU as convex polygons with a depth buffer and
 * perspective-correct texturing.  The CPU transforms, clips and sets up
 * each polygon (fgl_setup.c); the GPU does every pixel.  fglInit()
 * switches the FPGA to the 3D GPU and takes the display, fglShutdown()
 * gives both back.  Bilinear filtering or the nearest texel
 * (GL_TEXTURE_MAG_FILTER; FGL_TEXTURE_DITHER smooths the nearest), RGB565
 * colour (8 bits a channel inside, dithered), a 16-bit floating-point
 * depth buffer; one texture a polygon (multitexturing is done in passes,
 * blended); colour and alpha interpolated across the screen.
 * GL_ALPHA_TEST on a texture without alpha (RGB, luminance,
 * GL_COLOR_INDEX) is a colour key: fglColorKey().
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCGL_FPCGL_H
#define __APPS_GRAPHICS_FPCGL_FPCGL_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned int   GLuint;
typedef unsigned char  GLubyte;
typedef float          GLfloat;
typedef float          GLclampf;
typedef double         GLdouble;
typedef void           GLvoid;

#define GL_FALSE                  0
#define GL_TRUE                   1

/* Errors (glGetError()) */

#define GL_NO_ERROR               0
#define GL_INVALID_ENUM           0x0500
#define GL_INVALID_VALUE          0x0501
#define GL_INVALID_OPERATION      0x0502
#define GL_STACK_OVERFLOW         0x0503
#define GL_STACK_UNDERFLOW        0x0504
#define GL_OUT_OF_MEMORY          0x0505

/* Primitives */

#define GL_POINTS                 0x0000    /* (not drawn) */
#define GL_LINES                  0x0001    /* (not drawn) */
#define GL_TRIANGLES              0x0004
#define GL_TRIANGLE_STRIP         0x0005
#define GL_TRIANGLE_FAN           0x0006
#define GL_QUADS                  0x0007
#define GL_QUAD_STRIP             0x0008
#define GL_POLYGON                0x0009

/* Matrices */

#define GL_MODELVIEW              0x1700
#define GL_PROJECTION             0x1701

/* glEnable() */

#define GL_CULL_FACE              0x0B44
#define GL_DEPTH_TEST             0x0B71
#define GL_ALPHA_TEST             0x0BC0    /* (no alpha in the texture: the colour key) */
#define GL_TEXTURE_2D             0x0DE1
#define GL_BLEND                  0x0BE2
#define GL_FOG                    0x0B60
#define GL_DITHER                 0x0BD0    /* (on at the start, as GL's) */
#define FGL_TEXTURE_DITHER        0x8FF0    /* fpcgl's: texel coordinates dithered */

/* Blending factors (GL_DST_ALPHA is 1: the buffer has no alpha) */

#define GL_ZERO                   0
#define GL_ONE                    1
#define GL_SRC_COLOR              0x0300
#define GL_ONE_MINUS_SRC_COLOR    0x0301
#define GL_SRC_ALPHA              0x0302
#define GL_ONE_MINUS_SRC_ALPHA    0x0303
#define GL_DST_ALPHA              0x0304
#define GL_ONE_MINUS_DST_ALPHA    0x0305
#define GL_DST_COLOR              0x0306
#define GL_ONE_MINUS_DST_COLOR    0x0307

/* Fog */

#define GL_FOG_DENSITY            0x0B62
#define GL_FOG_START              0x0B63
#define GL_FOG_END                0x0B64
#define GL_FOG_MODE               0x0B65
#define GL_FOG_COLOR              0x0B66
#define GL_EXP                    0x0800
#define GL_EXP2                   0x0801

/* Depth functions (the GPU has GL_LESS, GL_LEQUAL, GL_ALWAYS), the alpha
 * test's (GL_GREATER, GL_GEQUAL, GL_ALWAYS)
 */

#define GL_LESS                   0x0201
#define GL_LEQUAL                 0x0203
#define GL_GREATER                0x0204
#define GL_GEQUAL                 0x0206
#define GL_ALWAYS                 0x0207

/* glClear() */

#define GL_DEPTH_BUFFER_BIT       0x0100
#define GL_COLOR_BUFFER_BIT       0x4000

/* Faces, winding, shading */

#define GL_FRONT                  0x0404
#define GL_BACK                   0x0405
#define GL_FRONT_AND_BACK         0x0408
#define GL_CW                     0x0900
#define GL_CCW                    0x0901
#define GL_FLAT                   0x1D00
#define GL_SMOOTH                 0x1D01

/* Textures */

#define GL_TEXTURE_MAG_FILTER     0x2800    /* GL_LINEAR (bilinear) or GL_NEAREST */
#define GL_TEXTURE_MIN_FILTER     0x2801    /* (taken, as the magnified one) */
#define GL_TEXTURE_WRAP_S         0x2802
#define GL_TEXTURE_WRAP_T         0x2803
#define GL_NEAREST                0x2600
#define GL_LINEAR                 0x2601
#define GL_CLAMP                  0x2900
#define GL_REPEAT                 0x2901
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_COLOR_INDEX            0x1900    /* 8-bit, through fglColorTable() */
#define GL_RGB                    0x1907
#define GL_RGBA                   0x1908    /* (ARGB1555 if every alpha is 0 or 255, else ARGB4444) */
#define GL_LUMINANCE              0x1909
#define GL_UNSIGNED_BYTE          0x1401
#define GL_UNSIGNED_SHORT_5_6_5   0x8363

/****************************************************************************
 * fpcgl's own
 ****************************************************************************/

#define FGL_KEY                   0xf81f    /* The colour key's default */

/* The 3D GPU (the FPGA switched to it: what the desktop showed is redrawn
 * when it comes back) and the display in mode (FPC_GPU_MODE_*), a w x h
 * picture scaled up as far as it fits (SET_SCALE), centred, double
 * buffered, with a depth buffer.  0, or < 0: -EBUSY (another game has the
 * display), -ENODEV (no 3D GPU), -ENOMEM
 */

int  fglInit(int w, int h, int mode);
void fglShutdown(void);
void fglSwapBuffers(void);                /* Shown from the next frame */
int  fglWidth(void);
int  fglHeight(void);

void fglColorKey(uint16_t key);           /* GL_ALPHA_TEST's colour (RGB565) */
void fglColorTable(const uint16_t pal[256]);   /* GL_COLOR_INDEX's palette */

/* Triangles sent to the GPU since fglInit() (after clipping and culling) */

uint32_t fglTriangles(void);

/* The picture being drawn: its top-left in the space (a game's 2D parts
 * go there, as PUT8 with a key, after glFlush())
 */

void fglBackBuffer(int *x, int *y);

/* A colour blended over the whole picture (alpha 0-255), e.g. a game's
 * flash or tint, at the point it is called
 */

void fglBlendView(uint16_t colour, int alpha);

/* Everything but the display given back: every texture, every rectangle
 * of the space (a game's next level)
 */

void fglReset(void);

/* The space fpcgl hands out (rectangles of RGB565 pixels, w even), for a
 * game's own images: 0, or < 0 if there is no room
 */

int  fglSpaceAlloc(int w, int h, int16_t *x, int16_t *y);
void fglSpaceFree(int x, int y, int w, int h);

/* w x h RGB565 pixels (rows stride bytes apart) put at (x, y) of the space,
 * after what was drawn before it
 */

void fglPut(int x, int y, int w, int h, const uint16_t *px, size_t stride);

/* A rectangle of the space as the texture (as glBindTexture(), until the
 * next bind): w x h at x, y, repeated (w, h powers of two; filtered: x a
 * multiple of 16) or clamped; the nearest texel (glTexParameteri() may
 * filter it)
 */

void fglBindRect(int x, int y, int w, int h, GLboolean repeat);

/****************************************************************************
 * The GL subset
 ****************************************************************************/

GLenum glGetError(void);

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glDepthRange(GLclampf znear, GLclampf zfar);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glClearDepth(GLclampf d);
void glClear(GLbitfield mask);
void glFlush(void);
void glFinish(void);

void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glShadeModel(GLenum mode);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glAlphaFunc(GLenum func, GLclampf ref);
void glFogf(GLenum pname, GLfloat param);
void glFogi(GLenum pname, GLint param);
void glFogfv(GLenum pname, const GLfloat *params);

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat *m);
void glMultMatrixf(const GLfloat *m);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
               GLdouble n, GLdouble f);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t,
             GLdouble n, GLdouble f);

void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3fv(const GLfloat *v);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2fv(const GLfloat *v);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glColor3ub(GLubyte r, GLubyte g, GLubyte b);
void glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);

void glGenTextures(GLsizei n, GLuint *textures);
void glDeleteTextures(GLsizei n, const GLuint *textures);
void glBindTexture(GLenum target, GLuint texture);
void glTexParameteri(GLenum target, GLenum pname, GLint param);
void glTexImage2D(GLenum target, GLint level, GLint internalformat,
                  GLsizei w, GLsizei h, GLint border, GLenum format,
                  GLenum type, const GLvoid *pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y,
                     GLsizei w, GLsizei h, GLenum format, GLenum type,
                     const GLvoid *pixels);

#endif /* __APPS_GRAPHICS_FPCGL_FPCGL_H */
