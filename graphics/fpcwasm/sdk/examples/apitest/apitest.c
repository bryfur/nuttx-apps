/****************************************************************************
 * apps/graphics/fpcwasm/sdk/examples/apitest/apitest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fpc API's test (tests/desk/wasm.test): one frame that uses the
 * calls the other examples do not, each in a box of its own, and calls
 * with bad handles and values, which must change nothing:
 *
 *   - images from files of the app's directory (BMP: fpc_image_load) and
 *     from bytes in its memory (QOI, made here: fpc_image_decode);
 *   - 8-bit pixels through a short palette;
 *   - a quadrilateral in perspective, a sprite turned in 3D, a Mode 7
 *     floor, a sprite through a matrix;
 *   - a tile layer filled with fpc_layer_put (the background);
 *   - the text layer: key events (fpc_key_next) and the mouse as they
 *     come, the display and the statistics;
 *   - bad calls: unknown images and layers, sizes out of range, a
 *     voice out of range, text off the layer.
 *
 *   wasm run -s '10 z 20 +left 30 freeze' apitest
 ****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "fpc_wasm.h"

static int g_icon;                        /* icon.bmp */
static int g_qoi;                         /* Made as a QOI file */
static int g_pal8;                        /* 8-bit pixels */
static int g_tiles;
static char g_keys[48];                   /* The key events seen */
static int g_nkeys;

static void text(int col, int row, const char *s)
{
  fpc_text_print(col, row, s, (int32_t)strlen(s));
}

/* A QOI file of a 16 x 16 picture: rows of RGB and run chunks */

static int make_qoi(uint8_t *q)
{
  int n = 0;
  int y;
  int x;

  memcpy(q, "qoif", 4);
  q[4] = q[5] = q[6] = 0;
  q[7] = 16;
  q[8] = q[9] = q[10] = 0;
  q[11] = 16;
  q[12] = 4;                              /* RGBA */
  q[13] = 0;
  n = 14;
  for (y = 0; y < 16; y++)
    {
      for (x = 0; x < 16; x += 4)
        {
          q[n++] = 0xff;                  /* QOI_OP_RGBA */
          q[n++] = (uint8_t)(x * 16);
          q[n++] = (uint8_t)(y * 16);
          q[n++] = 200;
          q[n++] = (x == 12 && y < 4) ? 0 : 255;   /* A see-through corner */
          q[n++] = 0xc0 | 2;              /* QOI_OP_RUN: 3 more */
        }
    }

  memset(q + n, 0, 7);
  q[n + 7] = 1;
  return n + 8;
}

FPC_EXPORT(fpc_load) void fpc_load(void)
{
  static const char name[] = "icon.bmp";
  static uint8_t qoi[16 * 16 * 6 + 32];
  static uint8_t px8[32 * 16];
  static const uint16_t pal[3] =
  {
    0, FPC_RGB(255, 80, 80), FPC_RGB(80, 255, 80)
  };

  static uint16_t tiles[16 * 8];          /* Two 8 x 8 tiles */
  static uint16_t map[20 * 4];
  int i;

  g_icon = fpc_image_load(name, (int32_t)strlen(name));
  g_qoi  = fpc_image_decode(qoi, make_qoi(qoi));

  for (i = 0; i < 32 * 16; i++)
    {
      px8[i] = (uint8_t)((i / 4 + i / 128) % 4);   /* 3: past the palette */
    }

  g_pal8 = fpc_image_new(32, 16);
  fpc_image_put8(g_pal8, 0, 0, 32, 16, px8, 32, pal, 3);

  for (i = 0; i < 16 * 8; i++)
    {
      tiles[i] = (i % 16) < 8 ? FPC_KEY :
                 ((i / 16 + i) & 2) ? FPC_RGB(40, 40, 70) :
                                      FPC_RGB(60, 50, 40);
    }

  g_tiles = fpc_image_new(16, 8);
  fpc_image_put(g_tiles, 0, 0, 16, 8, tiles, 16);
  for (i = 0; i < 20 * 4; i++)
    {
      map[i] = FPC_TILE(((i % 20) + (i / 20)) % 3 == 0 ? 0 : 1,
                        i & 1, (i >> 1) & 1);
    }

  fpc_layer_init(2, g_tiles, 8, 8, 20, 4);
  fpc_layer_put(2, 0, 0, 20, 4, map, 20);

  /* Bad calls: nothing happens */

  fpc_image_put(99, 0, 0, 4, 4, tiles, 4);
  fpc_image_put(g_tiles, -4, 0, 4, 4, tiles, 4);
  fpc_image_put(g_tiles, 0, 0, 0, 4, tiles, 4);
  fpc_layer_set(7, 0, 0, 1);
  fpc_layer_init(5, g_tiles, 8, 8, 4, 4);
  fpc_sfx_play(40, 0, 65536, 255, 255);
  fpc_fm_on(-1, 0, 0, 0, 0);
  fpc_image_free(77);
}

FPC_EXPORT(fpc_update) void fpc_update(void)
{
  int32_t e;

  while ((e = fpc_key_next()) >= 0 && g_nkeys < (int)sizeof(g_keys) - 4)
    {
      int c = FPC_KEY_CODE(e);

      g_keys[g_nkeys++] = FPC_KEY_UP_EVENT(e) ? '-' : '+';
      g_keys[g_nkeys++] = c >= 0x80 ? '^' : (char)c;
      g_keys[g_nkeys] = '\0';
    }
}

FPC_EXPORT(fpc_draw) void fpc_draw(void)
{
  static const int32_t quad[8] =
  {
    150 * 16, 20 * 16, 230 * 16, 30 * 16, 220 * 16, 90 * 16, 160 * 16,
    80 * 16
  };

  int32_t disp[4];
  int32_t m[4];
  char line[64];
  int ok;

  fpc_clear(FPC_RGB(30, 30, 40));

  /* A layer (it repeats over the whole canvas), a rectangle */

  fpc_layer_draw(2, 3, 2);
  fpc_rect(4, 76, 60, 4, FPC_RGB(255, 255, 255));

  /* Images: the file's, the QOI, the 8-bit one (twice as big) */

  fpc_sprite(g_icon, 0, 0, 64, 64, 8, 8, 0);
  fpc_sprite(g_qoi, 0, 0, 16, 16, 80, 8, 0);
  fpc_sprite_rot(g_pal8, 0, 0, 32, 16, 100, 60, 0, 0, 512);

  /* 3D: a quadrilateral, a turned sprite, a floor; a matrix (a shear) */

  fpc_quad(g_icon, 0, 0, 64, 64, quad, 0);
  ok = fpc_quad3d(g_icon, 0, 0, 64, 64, 300, 50, 0, 600, 300, 100, 256);
  fpc_mode7(g_icon, 150, 200, 32 << 8, 32 << 8, 512, 24, 130, 120,
            FPC_WRAP);
  fpc_sprite_matrix(g_icon, 0, 0, 64, 64, 380, 50, 0, 256, 128, 0, 256);

  /* Bad drawing: off everything, unknown images */

  fpc_sprite(42, 0, 0, 16, 16, 10, 10, 0);
  fpc_sprite(g_icon, 60, 60, 16, 16, 10, 10, 0);
  fpc_rect(1000000, 5, 10, 10, 0);
  fpc_layer_draw(9, 0, 0);

  /* The text layer */

  fpc_game_display(disp, 4);
  snprintf(line, sizeof(line), "display %dx%d x%d mode %d  quad3d %d",
           (int)disp[0], (int)disp[1], (int)disp[2], (int)disp[3], ok);
  text(1, 26, line);
  snprintf(line, sizeof(line), "keys %s", g_keys);
  text(1, 27, line);
  if (fpc_mouse(m, 4))
    {
      snprintf(line, sizeof(line), "mouse %d,%d %d", (int)m[0], (int)m[1],
               (int)m[2]);
    }
  else
    {
      snprintf(line, sizeof(line), "no mouse");
    }

  text(1, 28, line);
  text(-3, 29, "abcdef: cut on the left");
  text(60, 29, "cut on the right");
  fpc_text_print(1, 40, "off the layer", 13);
  fpc_text_draw();
}

int main(void)
{
  return fpc_game_run(424, 240) < 0;
}
