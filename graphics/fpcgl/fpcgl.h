/****************************************************************************
 * apps/graphics/fpcgl/fpcgl.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fpcgl: an OpenGL 1.x subset on the FPC's 3D GPU (docs/gpu3d.md): the
 * GL calls a fixed-function renderer uses (immediate mode, the matrix
 * stacks, the depth test, culling, flat and smooth shading, textures
 * repeated or clamped), drawn by the GPU as triangles with a depth buffer
 * and perspective-correct texturing.  The CPU transforms, clips and sets
 * up each triangle (fgl_setup.c); the GPU does every pixel.  fglInit()
 * switches the FPGA to the 3D GPU and takes the display, fglShutdown()
 * gives both back.  Nearest-texel sampling, RGB565 colour, 16-bit depth;
 * no blending (GL_ALPHA_TEST is a colour key: fglColorKey()).
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCGL_FPCGL_H
#define __APPS_GRAPHICS_FPCGL_FPCGL_H

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
#define GL_ALPHA_TEST             0x0BC0    /* The colour key (fglColorKey()) */
#define GL_TEXTURE_2D             0x0DE1

/* Depth functions (the GPU has these three) */

#define GL_LESS                   0x0201
#define GL_LEQUAL                 0x0203
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

#define GL_TEXTURE_MAG_FILTER     0x2800    /* (always nearest) */
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_WRAP_S         0x2802
#define GL_TEXTURE_WRAP_T         0x2803
#define GL_NEAREST                0x2600
#define GL_LINEAR                 0x2601
#define GL_CLAMP                  0x2900
#define GL_REPEAT                 0x2901
#define GL_CLAMP_TO_EDGE          0x812F
#define GL_COLOR_INDEX            0x1900    /* 8-bit, through fglColorTable() */
#define GL_RGB                    0x1907
#define GL_RGBA                   0x1908    /* (alpha < 128: the colour key) */
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
