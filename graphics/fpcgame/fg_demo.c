/****************************************************************************
 * apps/graphics/fpcgame/fg_demo.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fgdemo: the game SDK's demonstration and test.  Everything it shows is
 * made here: a tileset and two scrolling layers (one with flipped tiles),
 * a ship flown with the pad and turned towards where it goes (SPRITE,
 * rotated and scaled), orbs added on and halved, a sprite larger than the
 * GPU's cell flipped, rectangles, text in changing colours, a sample on A
 * and an FM tune.  "fgdemo N" ends after N frames (tests), else Escape;
 * "fgdemo N freeze" stops the picture at frame N and goes on showing it
 * (the board's picture compared with the simulator's: tools grab it);
 * "fgdemo -s SCRIPT" plays scripted input (fg_script()).
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpcgame.h"

#define TW 8

static int g_tiles;                       /* Tileset: 8 tiles across, 2 rows */
static int g_sheet;                       /* Ship 32 x 32, orb 16 x 16, banner 96 x 40 */
static int g_frames = -1;
static bool g_freeze;
static uint32_t g_f;                      /* The picture's frame */
static int32_t g_x;                       /* The ship (init(): statics keep */
static int32_t g_y;                       /* their values from run to run) */
static int g_angle;
static uint32_t g_shots;

static const struct fg_patch_s g_lead =
{
  0, 3, { { 2, 0, 0, 120, 250, 60, 150, 40 }, { 1, 0, 0, 230, 250, 40, 180, 60 } }
};

static const struct fg_note_s g_tune[] =
{
  { 0, 12, 0, 69, 200, 14 },   { 16, 12, 0, 72, 200, 14 },
  { 32, 12, 0, 76, 200, 14 },  { 48, 12, 0, 74, 200, 30 },
  { 80, 12, 0, 72, 200, 14 },  { 96, 12, 0, 69, 200, 30 },
};

static const struct fg_song_s g_song =
{
  g_tune, sizeof(g_tune) / sizeof(g_tune[0]), 128, true
};

static int8_t g_blip[2000];

static void make_tiles(void)
{
  static uint16_t px[16 * 8 * 16 * TW] FG_PSRAM;   /* 128 x 16 */
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
                  case 1: c = ((x ^ y) & 4) ? FG_RGB(40, 80, 160) : FG_RGB(30, 60, 120); break;
                  case 2: c = (y == 0 || (x == 0 && (y < 4))) ? FG_RGB(90, 50, 40) : FG_RGB(170, 90, 60); break;
                  case 3: c = x + y < 8 ? FG_RGB(60, 200, 80) : FG_KEY; break;   /* A slope */
                  case 4: c = (x == 3 || x == 4) ? FG_RGB(220, 220, 90) : FG_KEY; break;
                  case 5: c = FG_RGB(20 + y * 20, 120, 200 - y * 10); break;
                  case 6: c = (x + y) & 1 ? FG_RGB(80, 80, 80) : FG_RGB(110, 110, 110); break;
                  case 7: c = x * x + y * y < 30 ? FG_RGB(250, 180, 40) : FG_KEY; break;
                  default: c = FG_KEY; break;
                }

              px[(ty + y) * 128 + tx + x] = c;
            }
        }
    }

  g_tiles = fg_image_new(128, 16);
  fg_image_put(g_tiles, 0, 0, 128, 16, px, 256);
}

static void make_sheet(void)
{
  static uint16_t px[96 * 72] FG_PSRAM;   /* 96 wide: ship, orb; banner below */
  int x;
  int y;

  for (y = 0; y < 72; y++)
    {
      for (x = 0; x < 96; x++)
        {
          uint16_t c = FG_KEY;

          if (y < 32 && x < 32)           /* A ship pointing right */
            {
              int dy = y - 16;

              if (x >= 4 && abs(dy) <= (x - 4) / 3 + 1 && x < 30)
                {
                  c = abs(dy) < 2 ? FG_RGB(250, 250, 255) : FG_RGB(80 + x * 5, 90, 200);
                }
            }
          else if (y < 16 && x >= 32 && x < 48)   /* An orb */
            {
              int dx = x - 40;
              int dy = y - 8;
              int r = dx * dx + dy * dy;

              if (r < 50)
                {
                  c = FG_RGB(255 - r * 4, 120 + r, 40);
                }
            }
          else if (y >= 32)               /* A banner, 96 x 40: an arrow left */
            {
              int yy = y - 32;

              c = (x < 20 && abs(yy - 20) < x) || (x >= 20 && abs(yy - 20) < 8) ?
                  FG_RGB(40 + x * 2, 200 - yy * 3, 120) : FG_KEY;
            }

          px[y * 96 + x] = c;
        }
    }

  g_sheet = fg_image_new(96, 72);
  fg_image_put(g_sheet, 0, 0, 96, 72, px, 192);
}

static void demo_load(void)
{
  int x;
  int y;
  int i;

  make_tiles();
  make_sheet();

  fg_layer_init(0, g_tiles, TW, TW, 64, 32);   /* The far one: sky tiles */
  fg_layer_init(1, g_tiles, TW, TW, 64, 32);   /* Ground with slopes */
  for (y = 0; y < 32; y++)
    {
      for (x = 0; x < 64; x++)
        {
          fg_layer_set(0, x, y, FG_TILE((x * 7 + y * 3) % 5 == 0 ? 5 : 1, 0, 0));
          if (y >= 26)
            {
              fg_layer_set(1, x, y, FG_TILE(y == 26 ? 6 : 2, 0, 0));
            }
          else if (y == 25 && (x & 7) < 2)
            {
              fg_layer_set(1, x, y, FG_TILE(3, (x & 7) == 1, 0));   /* Slopes, one flipped */
            }
          else if (y == 24 && (x % 11) == 0)
            {
              fg_layer_set(1, x, y, FG_TILE(7, 0, 0));
            }
        }
    }

  for (i = 0; i < (int)sizeof(g_blip); i++)
    {
      g_blip[i] = (int8_t)((((i * 7) ^ (i >> 3)) & 64) - 32) * (2000 - i) / 2000;
    }

  fg_sample(0, g_blip, sizeof(g_blip), 0xffffffff, 11025);
  fg_patch(0, &g_lead);
  fg_master_volume(200, 200);
  fg_music_play(&g_song);
}

static void demo_init(void)
{
  g_x = 200 << 8;
  g_y = 120 << 8;
  g_angle = 0;
  g_shots = 0;
}

static void demo_update(void)
{
  int ax = fg_axis_x();
  int ay = fg_axis_y();

  g_x += ax * 3;
  g_y -= ay * 3;
  g_x = g_x < (16 << 8) ? 16 << 8 : g_x > ((fg_width() - 16) << 8) ? (fg_width() - 16) << 8 : g_x;
  g_y = g_y < (16 << 8) ? 16 << 8 : g_y > (190 << 8) ? 190 << 8 : g_y;
  if (ax != 0 || ay != 0)
    {
      g_angle += 24;                      /* Spins while flown */
    }

  if (fg_pad_pressed() & (FG_PAD_A | FG_PAD_START))
    {
      fg_sfx_play(g_shots++ & 3, 0, 65536, 200, 200);
    }

  if (g_frames > 0 && (int)fg_frame() >= g_frames && !g_freeze)
    {
      fg_exit();
    }

  g_f = g_freeze && g_frames > 0 && (int)fg_frame() >= g_frames ?
        (uint32_t)g_frames : fg_frame();
}

static void demo_draw(void)
{
  uint32_t f = g_f;
  int k;

  fg_clear(FG_RGB(10, 10, 30));
  fg_layer_draw(0, (int)(f / 2), 0);
  fg_layer_draw(1, (int)f * 2, 0);

  fg_rect(10, 10, 100, 6, FG_RGB(200, 40, 40));          /* Rects, one off the canvas */
  fg_rect(-5, 200, 30, 50, FG_RGB(40, 200, 40));

  for (k = 0; k < 8; k++)                 /* Orbs: plain, added, halved */
    {
      int ox = 20 + k * 48 + (int)((fg_sin((int)f * 16 + k * 512) * 20) >> 8);
      uint32_t fl = k < 3 ? 0 : k < 6 ? FG_ADD : FG_HALF;

      fg_sprite(g_sheet, 32, 0, 16, 16, ox, 40, fl);
      fg_sprite(g_sheet, 32, 0, 16, 16, ox + 6, 46, fl);
    }

  fg_sprite(g_sheet, 0, 32, 96, 40, 300, 150, FG_HFLIP);  /* Larger than a cell, flipped */
  fg_sprite(g_sheet, 0, 32, 96, 40, 300, 100, 0);

  fg_sprite_rot(g_sheet, 0, 0, 32, 32, g_x >> 8, g_y >> 8, 0, g_angle, 256 + (int)((fg_sin((int)f * 20) * 64) >> 8));
  fg_sprite(g_sheet, 0, 0, 32, 32, 40, 120, FG_VFLIP);
  fg_sprite_rot(g_sheet, 0, 0, 32, 32, -6, 160, 0, (int)f * 30, 256);   /* Cut by the edge */

  fg_text_color(FG_RGB(128 + (int)((fg_sin((int)f * 8) * 127) >> 8), 255, 128));
  fg_text_print(1, 1, "FPC GAME SDK  frame %5u", (unsigned)f);
  fg_text_center(28, "arrows: fly   Z: blip   Esc: quit");
  fg_text_draw();
}

static const struct fg_game_s g_demo =
{
  "fgdemo", 424, 240, demo_load, demo_init, demo_update, demo_draw, NULL
};

int main(int argc, FAR char *argv[])
{
  int ret;

  if (argc == 3 && strcmp(argv[1], "-s") == 0)
    {
      fg_script(argv[2]);
    }
  else
    {
      g_frames = argc > 1 ? atoi(argv[1]) : -1;
      g_freeze = argc > 2 && strcmp(argv[2], "freeze") == 0;
    }

  ret = fg_run(&g_demo);
  if (ret < 0)
    {
      fprintf(stderr, "fgdemo: %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("fgdemo: %u frames, %u late\n", (unsigned)fg_stats()->frames,
         (unsigned)fg_stats()->late);
  return EXIT_SUCCESS;
}
