/****************************************************************************
 * apps/graphics/fpcgl/fgl_demo.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fgldemo: fpcgl on the 3D GPU.  A checkered floor in perspective (a
 * repeated texture, its texels dithered) fading into the fog, a cube
 * turning (textured, its corners' colours smooth), a pyramid shaded from
 * its corners (untextured), a sign with a keyed texture (GL_ALPHA_TEST)
 * from an 8-bit image (GL_COLOR_INDEX), a pane of tinted glass (blended
 * by alpha) and a glow (added).
 * "fgldemo N" ends after N frames (else 600); "fgldemo N hold" shows frame
 * N for three seconds first (the tests' picture), then goes back to the
 * desktop.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fpcgl.h"
#include "fpc_gpu.h"

#define TEX 64

static GLuint g_tex[3];

static void make_textures(void)
{
  static uint8_t rgb[TEX * TEX * 3];
  static uint8_t idx[TEX * TEX];
  static uint16_t pal[256];
  int x;
  int y;
  int i;

  glGenTextures(3, g_tex);

  /* The floor: a checker, a line where it repeats */

  for (y = 0; y < TEX; y++)
    {
      for (x = 0; x < TEX; x++)
        {
          uint8_t *p = &rgb[(y * TEX + x) * 3];
          int on = ((x >> 3) ^ (y >> 3)) & 1;

          p[0] = on ? 230 : 40;
          p[1] = on ? 220 : 110;
          p[2] = on ? 60 : 50;
          if (x == 0 || y == 0)
            {
              p[0] = 200; p[1] = 30; p[2] = 30;
            }
        }
    }

  glBindTexture(GL_TEXTURE_2D, g_tex[0]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, TEX, TEX, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);

  /* The cube: a crate's planks and frame */

  for (y = 0; y < TEX; y++)
    {
      for (x = 0; x < TEX; x++)
        {
          uint8_t *p = &rgb[(y * TEX + x) * 3];
          int frame = x < 6 || y < 6 || x >= TEX - 6 || y >= TEX - 6 ||
                      abs(x - y) < 4 || abs(x + y - TEX) < 4;
          int grain = ((x * 7 + (y >> 2) * 13) & 15);

          p[0] = (uint8_t)(frame ? 120 : 170 + grain);
          p[1] = (uint8_t)(frame ? 70 : 110 + grain);
          p[2] = (uint8_t)(frame ? 30 : 50);
        }
    }

  glBindTexture(GL_TEXTURE_2D, g_tex[1]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, TEX, TEX, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

  /* The sign: rings in 8 bits, index 0 the key */

  for (i = 0; i < 256; i++)
    {
      pal[i] = (uint16_t)(((i * 3) & 31) << 11 | ((i * 5) & 63) << 5 | (31 - (i & 31)));
    }

  pal[0] = FGL_KEY;
  fglColorTable(pal);
  for (y = 0; y < TEX; y++)
    {
      for (x = 0; x < TEX; x++)
        {
          int dx = x * 2 - TEX + 1;
          int dy = y * 2 - TEX + 1;
          int d2 = dx * dx + dy * dy;

          idx[y * TEX + x] = d2 > TEX * TEX ? 0 : (uint8_t)(1 + (d2 >> 7) % 200);
        }
    }

  glBindTexture(GL_TEXTURE_2D, g_tex[2]);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX, TEX, TEX, 0, GL_COLOR_INDEX,
               GL_UNSIGNED_BYTE, idx);
}

/* The floor in 8 x 8 quads (the fog is per vertex), its texels dithered */

static void floor_quad(void)
{
  int i;
  int k;

  glEnable(GL_TEXTURE_2D);
  glEnable(FGL_TEXTURE_DITHER);
  glBindTexture(GL_TEXTURE_2D, g_tex[0]);
  glColor3f(1.0f, 1.0f, 1.0f);
  glBegin(GL_QUADS);
  for (i = 0; i < 8; i++)
    {
      for (k = 0; k < 8; k++)
        {
          float x0 = -40.0f + 10.0f * k;
          float z0 = -40.0f + 10.0f * i;

          glTexCoord2f(2.5f * k, 2.5f * i);
          glVertex3f(x0, 0.0f, z0);
          glTexCoord2f(2.5f * k, 2.5f * (i + 1));
          glVertex3f(x0, 0.0f, z0 + 10.0f);
          glTexCoord2f(2.5f * (k + 1), 2.5f * (i + 1));
          glVertex3f(x0 + 10.0f, 0.0f, z0 + 10.0f);
          glTexCoord2f(2.5f * (k + 1), 2.5f * i);
          glVertex3f(x0 + 10.0f, 0.0f, z0);
        }
    }

  glEnd();
  glDisable(FGL_TEXTURE_DITHER);
}

/* A pane of tinted glass (alpha) and a glow added on, after the rest
 * (their depth tested, not written)
 */

static void glass(void)
{
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_CULL_FACE);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBegin(GL_QUADS);
  glColor4f(0.3f, 0.9f, 1.0f, 0.25f);  glVertex3f(-1.6f, 0.0f, 0.0f);
  glColor4f(0.3f, 0.9f, 1.0f, 0.25f);  glVertex3f(1.6f, 0.0f, 0.0f);
  glColor4f(0.3f, 0.9f, 1.0f, 0.6f);   glVertex3f(1.6f, 2.8f, 0.0f);
  glColor4f(0.3f, 0.9f, 1.0f, 0.6f);   glVertex3f(-1.6f, 2.8f, 0.0f);
  glEnd();
  glBlendFunc(GL_ONE, GL_ONE);
  glBegin(GL_TRIANGLES);
  glColor3f(0.6f, 0.3f, 0.0f);  glVertex3f(2.2f, 0.2f, 0.5f);
  glColor3f(0.6f, 0.3f, 0.0f);  glVertex3f(3.8f, 0.2f, 0.5f);
  glColor3f(0.0f, 0.0f, 0.0f);  glVertex3f(3.0f, 2.4f, 0.5f);
  glEnd();
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glEnable(GL_CULL_FACE);
}

static void cube(void)
{
  static const float v[8][3] =
  {
    { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 },
    { -1, -1, 1 },  { 1, -1, 1 },  { 1, 1, 1 },  { -1, 1, 1 }
  };

  static const int f[6][4] =
  {
    { 4, 5, 6, 7 }, { 1, 0, 3, 2 }, { 5, 1, 2, 6 },
    { 0, 4, 7, 3 }, { 7, 6, 2, 3 }, { 0, 1, 5, 4 }
  };

  static const float c[8][3] =
  {
    { 0.4f, 0.4f, 0.4f }, { 1.6f, 0.6f, 0.6f }, { 1.0f, 1.0f, 1.0f }, { 0.6f, 1.4f, 0.6f },
    { 0.6f, 0.6f, 1.6f }, { 1.0f, 1.0f, 1.0f }, { 1.5f, 1.5f, 1.5f }, { 0.8f, 0.8f, 0.8f }
  };

  static const float tc[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
  int i;
  int k;

  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, g_tex[1]);
  glBegin(GL_QUADS);
  for (i = 0; i < 6; i++)
    {
      for (k = 0; k < 4; k++)
        {
          const int n = f[i][k];

          glColor3f(c[n][0], c[n][1], c[n][2]);
          glTexCoord2f(tc[k][0], tc[k][1]);
          glVertex3fv(v[n]);
        }
    }

  glEnd();
}

static void pyramid(void)
{
  glDisable(GL_TEXTURE_2D);
  glBegin(GL_TRIANGLE_FAN);
  glColor3f(1.0f, 1.0f, 0.2f);  glVertex3f(0.0f, 1.5f, 0.0f);
  glColor3f(1.0f, 0.1f, 0.1f);  glVertex3f(-1.0f, 0.0f, 1.0f);
  glColor3f(0.1f, 1.0f, 0.1f);  glVertex3f(1.0f, 0.0f, 1.0f);
  glColor3f(0.1f, 0.1f, 1.0f);  glVertex3f(1.0f, 0.0f, -1.0f);
  glColor3f(1.0f, 0.1f, 1.0f);  glVertex3f(-1.0f, 0.0f, -1.0f);
  glColor3f(1.0f, 0.1f, 0.1f);  glVertex3f(-1.0f, 0.0f, 1.0f);
  glEnd();
}

static void sign(void)
{
  glEnable(GL_TEXTURE_2D);
  glEnable(GL_ALPHA_TEST);
  glBindTexture(GL_TEXTURE_2D, g_tex[2]);
  glColor3f(1.0f, 1.0f, 1.0f);
  glBegin(GL_QUADS);
  glTexCoord2f(0.0f, 1.0f); glVertex3f(-1.0f, 0.0f, 0.0f);
  glTexCoord2f(1.0f, 1.0f); glVertex3f(1.0f, 0.0f, 0.0f);
  glTexCoord2f(1.0f, 0.0f); glVertex3f(1.0f, 2.0f, 0.0f);
  glTexCoord2f(0.0f, 0.0f); glVertex3f(-1.0f, 2.0f, 0.0f);
  glEnd();
  glDisable(GL_ALPHA_TEST);
}

int main(int argc, FAR char *argv[])
{
  int frames = argc > 1 ? atoi(argv[1]) : 600;
  bool hold = argc > 2 && strcmp(argv[2], "hold") == 0;
  int f;
  int ret;

  ret = fglInit(320, 240, FPC_GPU_MODE_1024x768);
  if (ret < 0)
    {
      fprintf(stderr, "fgldemo: no 3D GPU (%d)\n", ret);
      return EXIT_FAILURE;
    }

  make_textures();
  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glClearColor(0.45f, 0.6f, 0.9f, 1.0f);
  {
    static const float fogc[4] = { 0.45f, 0.6f, 0.9f, 1.0f };

    glEnable(GL_FOG);                     /* (into the sky's colour) */
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 8.0f);
    glFogf(GL_FOG_END, 45.0f);
    glFogfv(GL_FOG_COLOR, fogc);
  }

  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glFrustum(-0.8, 0.8, -0.6, 0.6, 1.0, 100.0);

  for (f = 0; f <= frames; f++)
    {
      float a = (float)f * 2.0f;

      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glMatrixMode(GL_MODELVIEW);
      glLoadIdentity();
      glTranslatef(0.0f, -1.6f, -7.0f);
      glRotatef(15.0f, 1.0f, 0.0f, 0.0f);
      glRotatef(a * 0.25f, 0.0f, 1.0f, 0.0f);

      floor_quad();

      glPushMatrix();
      glTranslatef(0.0f, 1.2f, 0.0f);
      glRotatef(a, 0.3f, 1.0f, 0.2f);
      cube();
      glPopMatrix();

      glPushMatrix();
      glTranslatef(-3.0f, 0.0f, -1.0f);
      glRotatef(-a * 1.5f, 0.0f, 1.0f, 0.0f);
      pyramid();
      glPopMatrix();

      glPushMatrix();
      glTranslatef(3.0f, 0.0f, -1.5f);
      glDisable(GL_CULL_FACE);
      sign();
      glEnable(GL_CULL_FACE);
      glPopMatrix();

      glPushMatrix();
      glTranslatef(0.0f, 0.0f, 2.2f);
      glass();
      glPopMatrix();

      fglSwapBuffers();
      usleep(16000);
    }

  if (hold)
    {
      sleep(3);
    }

  printf("fgldemo: %d frames, %u triangles\n", frames + 1, (unsigned)fglTriangles());
  fglShutdown();
  return EXIT_SUCCESS;
}
