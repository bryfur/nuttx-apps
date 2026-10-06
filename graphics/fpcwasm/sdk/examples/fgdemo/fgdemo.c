/****************************************************************************
 * apps/graphics/fpcwasm/sdk/examples/fgdemo/fgdemo.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fgdemo, the game SDK's demonstration (apps/graphics/fpcgame/fg_demo.c),
 * as a WebAssembly app on the fpc API: the same tileset and two scrolling
 * layers (one with flipped tiles), the ship flown with the pad and turned
 * (rotated and scaled), orbs added on and halved, a sprite larger than the
 * GPU's cell flipped, rectangles, text in changing colours, a sample on A
 * and an FM tune.  Drawn call for call as the native one, so that a frame
 * of it is the native one's pixel for pixel (tests/desk/wasm.test against
 * games.test's reference).
 *
 *   wasm run -s '20 +right 50 freeze' fgdemo
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpc_wasm.h"

#define TW 8

static int g_tiles;
static int g_sheet;
static int32_t g_x;
static int32_t g_y;
static int g_angle;
static uint32_t g_shots;
static uint32_t g_f;

static const uint8_t g_lead[FPC_PATCH_SIZE] =
{
  0, 3, 2, 0, 0, 120, 250, 60, 150, 40, 1, 0, 0, 230, 250, 40, 180, 60
};

/* The tune: frame (16 bits, little-endian), voice, patch, note, velocity,
 * length (16 bits)
 */

#define NOTE(f, v, p, n, vel, len) \
  (f) & 0xff, (f) >> 8, v, p, n, vel, (len) & 0xff, (len) >> 8

static const uint8_t g_tune[] =
{
  NOTE(0, 12, 0, 69, 200, 14),  NOTE(16, 12, 0, 72, 200, 14),
  NOTE(32, 12, 0, 76, 200, 14), NOTE(48, 12, 0, 74, 200, 30),
  NOTE(80, 12, 0, 72, 200, 14), NOTE(96, 12, 0, 69, 200, 30),
};

static int8_t g_blip[2000];

static int iabs(int v)
{
  return v < 0 ? -v : v;
}

static void make_tiles(void)
{
  static uint16_t px[16 * 8 * 16 * TW];   /* 128 x 16 */
  int t;
  int x;
  int y;

  for (t = 0; t < 32; t++)
    {
      int tx = (t % 16) * TW;
      int ty = (t / 16) * TW;

      for (y = 0; y < TW; y++)
        {
          for (x = 0; x < TW; x++)
            {
              uint16_t c;

              switch (t % 8)
                {
                  case 1:
                    c = ((x ^ y) & 4) ? FPC_RGB(40, 80, 160) :
                                        FPC_RGB(30, 60, 120);
                    break;
                  case 2:
                    c = (y == 0 || (x == 0 && (y < 4))) ?
                        FPC_RGB(90, 50, 40) : FPC_RGB(170, 90, 60);
                    break;
                  case 3:
                    c = x + y < 8 ? FPC_RGB(60, 200, 80) : FPC_KEY;
                    break;
                  case 4:
                    c = (x == 3 || x == 4) ? FPC_RGB(220, 220, 90) : FPC_KEY;
                    break;
                  case 5:
                    c = FPC_RGB(20 + y * 20, 120, 200 - y * 10);
                    break;
                  case 6:
                    c = (x + y) & 1 ? FPC_RGB(80, 80, 80) :
                                      FPC_RGB(110, 110, 110);
                    break;
                  case 7:
                    c = x * x + y * y < 30 ? FPC_RGB(250, 180, 40) : FPC_KEY;
                    break;
                  default:
                    c = FPC_KEY;
                    break;
                }

              px[(ty + y) * 128 + tx + x] = c;
            }
        }
    }

  g_tiles = fpc_image_new(128, 16);
  fpc_image_put(g_tiles, 0, 0, 128, 16, px, 128);
}

static void make_sheet(void)
{
  static uint16_t px[96 * 72];            /* Ship, orb; banner below */
  int x;
  int y;

  for (y = 0; y < 72; y++)
    {
      for (x = 0; x < 96; x++)
        {
          uint16_t c = FPC_KEY;

          if (y < 32 && x < 32)
            {
              int dy = y - 16;

              if (x >= 4 && iabs(dy) <= (x - 4) / 3 + 1 && x < 30)
                {
                  c = iabs(dy) < 2 ? FPC_RGB(250, 250, 255) :
                                     FPC_RGB(80 + x * 5, 90, 200);
                }
            }
          else if (y < 16 && x >= 32 && x < 48)
            {
              int dx = x - 40;
              int dy = y - 8;
              int r = dx * dx + dy * dy;

              if (r < 50)
                {
                  c = FPC_RGB(255 - r * 4, 120 + r, 40);
                }
            }
          else if (y >= 32)
            {
              int yy = y - 32;

              c = (x < 20 && iabs(yy - 20) < x) ||
                  (x >= 20 && iabs(yy - 20) < 8) ?
                  FPC_RGB(40 + x * 2, 200 - yy * 3, 120) : FPC_KEY;
            }

          px[y * 96 + x] = c;
        }
    }

  g_sheet = fpc_image_new(96, 72);
  fpc_image_put(g_sheet, 0, 0, 96, 72, px, 96);
}

FPC_EXPORT(fpc_load) void fpc_load(void)
{
  int x;
  int y;
  int i;

  make_tiles();
  make_sheet();

  fpc_layer_init(0, g_tiles, TW, TW, 64, 32);
  fpc_layer_init(1, g_tiles, TW, TW, 64, 32);
  for (y = 0; y < 32; y++)
    {
      for (x = 0; x < 64; x++)
        {
          fpc_layer_set(0, x, y,
                        FPC_TILE((x * 7 + y * 3) % 5 == 0 ? 5 : 1, 0, 0));
          if (y >= 26)
            {
              fpc_layer_set(1, x, y, FPC_TILE(y == 26 ? 6 : 2, 0, 0));
            }
          else if (y == 25 && (x & 7) < 2)
            {
              fpc_layer_set(1, x, y, FPC_TILE(3, (x & 7) == 1, 0));
            }
          else if (y == 24 && (x % 11) == 0)
            {
              fpc_layer_set(1, x, y, FPC_TILE(7, 0, 0));
            }
        }
    }

  for (i = 0; i < (int)sizeof(g_blip); i++)
    {
      g_blip[i] = (int8_t)((((i * 7) ^ (i >> 3)) & 64) - 32) * (2000 - i) /
                  2000;
    }

  fpc_sample(0, g_blip, sizeof(g_blip), -1, 11025);
  fpc_patch(0, g_lead);
  fpc_master_volume(200, 200);
  fpc_music_play(g_tune, sizeof(g_tune) / FPC_NOTE_SIZE, 128, 1);
}

FPC_EXPORT(fpc_init) void fpc_init(void)
{
  g_x = 200 << 8;
  g_y = 120 << 8;
  g_angle = 0;
  g_shots = 0;
}

FPC_EXPORT(fpc_update) void fpc_update(void)
{
  int ax = fpc_axis_x();
  int ay = fpc_axis_y();
  int w = fpc_game_width();

  g_x += ax * 3;
  g_y -= ay * 3;
  g_x = g_x < (16 << 8) ? 16 << 8 : g_x > ((w - 16) << 8) ? (w - 16) << 8 :
        g_x;
  g_y = g_y < (16 << 8) ? 16 << 8 : g_y > (190 << 8) ? 190 << 8 : g_y;
  if (ax != 0 || ay != 0)
    {
      g_angle += 24;
    }

  if (fpc_pad_pressed() & (FPC_PAD_A | FPC_PAD_START))
    {
      fpc_sfx_play(g_shots++ & 3, 0, 65536, 200, 200);
    }

  g_f = fpc_game_frame();
}

FPC_EXPORT(fpc_draw) void fpc_draw(void)
{
  static const char help[] = "arrows: fly   Z: blip   Esc: quit";
  uint32_t f = g_f;
  char line[64];
  int k;

  fpc_clear(FPC_RGB(10, 10, 30));
  fpc_layer_draw(0, (int)(f / 2), 0);
  fpc_layer_draw(1, (int)f * 2, 0);

  fpc_rect(10, 10, 100, 6, FPC_RGB(200, 40, 40));
  fpc_rect(-5, 200, 30, 50, FPC_RGB(40, 200, 40));

  for (k = 0; k < 8; k++)
    {
      int ox = 20 + k * 48 +
               (int)((fpc_sin((int)f * 16 + k * 512) * 20) >> 8);
      int fl = k < 3 ? 0 : k < 6 ? FPC_ADD : FPC_HALF;

      fpc_sprite(g_sheet, 32, 0, 16, 16, ox, 40, fl);
      fpc_sprite(g_sheet, 32, 0, 16, 16, ox + 6, 46, fl);
    }

  fpc_sprite(g_sheet, 0, 32, 96, 40, 300, 150, FPC_HFLIP);
  fpc_sprite(g_sheet, 0, 32, 96, 40, 300, 100, 0);

  fpc_sprite_rot(g_sheet, 0, 0, 32, 32, g_x >> 8, g_y >> 8, 0, g_angle,
                 256 + (int)((fpc_sin((int)f * 20) * 64) >> 8));
  fpc_sprite(g_sheet, 0, 0, 32, 32, 40, 120, FPC_VFLIP);
  fpc_sprite_rot(g_sheet, 0, 0, 32, 32, -6, 160, 0, (int)f * 30, 256);

  fpc_text_color(FPC_RGB(128 + (int)((fpc_sin((int)f * 8) * 127) >> 8),
                         255, 128));
  snprintf(line, sizeof(line), "FPC GAME SDK  frame %5u", (unsigned)f);
  fpc_text_print(1, 1, line, (int32_t)strlen(line));
  fpc_text_print((fpc_game_width() / 8 - (int)strlen(help)) / 2, 28, help,
                 (int32_t)strlen(help));
  fpc_text_draw();
}

int main(void)
{
  int ret = fpc_game_run(424, 240);

  if (ret < 0)
    {
      fprintf(stderr, "fgdemo: %d\n", (int)ret);
      return 1;
    }

  return 0;
}
