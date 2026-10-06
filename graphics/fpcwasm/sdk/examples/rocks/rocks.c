/****************************************************************************
 * apps/graphics/fpcwasm/sdk/examples/rocks/rocks.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rocks: TRI's Skyrocks (apps/games/trigames/skyrocks.c) as a WebAssembly
 * app on the fpc API (fpc_wasm.h, docs/wasm.md).  Fly the ship (arrows),
 * shoot the rocks (Z, held: a stream); a landscape scrolls below, clouds
 * above; the big rocks turn (rotated sprites), shots are added on, the
 * sounds are FM.  Everything is made here: 8-bit sheets through palettes.
 *
 *   wasm run rocks             the game
 *   wasm run rocks -b 400      a benchmark: 400 rocks (no deaths) for
 *                              600 frames, the frame times on the screen
 *                              and the average at the end
 *
 * The same source builds as a native NuttX program on fpc_native.c (the
 * fpc API straight onto the game SDK: `rocksn`), for comparisons.
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpc_wasm.h"

#define SHEET_W   64
#define SHEET_H   32
#define N_SHOTS   8
#define MAX_ROCKS 1024
#define SCREEN_W  424

struct rock_s
{
  int16_t x;
  int16_t y;
  int16_t dx;
  int16_t dy;
  uint8_t big;
  uint8_t alive;
};

struct shot_s
{
  int16_t x;
  int16_t y;
  uint8_t live;
};

static struct rock_s g_rock[MAX_ROCKS];
static struct shot_s g_shot[N_SHOTS];
static int g_nrocks = 16;
static int g_bench;                       /* -b N: rocks, no deaths */
static int g_sheet;
static int g_tiles;
static int g_ship_x;
static int g_ship_y;
static int g_score;
static int g_lives;
static int g_flash;
static uint32_t g_t;
static uint32_t g_frame_us_sum;           /* The bench's */
static uint32_t g_frame_us_n;

/* TRI's 15-bit colours, as RGB565 */

#define RGB555(r, g, b) FPC_RGB((r) << 3, (g) << 3, (b) << 3)

/* FM instruments (fpc_patch(): alg, feedback, op 0, op 1: mult, detune,
 * wave, level, attack, decay, sustain, release)
 */

static const uint8_t g_laser[FPC_PATCH_SIZE] =
{
  0, 2, 3, 0, 0, 200, 255, 120, 0, 200, 1, 0, 0, 255, 255, 60, 0, 120
};

static const uint8_t g_boom[FPC_PATCH_SIZE] =
{
  0, 7, 1, 0, 3, 255, 255, 40, 0, 60, 0, 0, 0, 255, 255, 30, 0, 40
};

static int iabs(int v)
{
  return v < 0 ? -v : v;
}

static void text(int col, int row, const char *s)
{
  fpc_text_print(col, row, s, (int32_t)strlen(s));
}

FPC_EXPORT(fpc_load) void fpc_load(void)
{
  static uint8_t sheet[SHEET_W * SHEET_H];
  static uint8_t tiles[8 * 8 * 8];         /* 8 tiles of 8 x 8, in a row */
  static uint16_t map0[64 * 32];
  static uint16_t map1[64 * 32];
  static const uint16_t pal1[6] =
  {
    0, RGB555(31, 31, 31), RGB555(10, 18, 31), RGB555(14, 12, 10),
    RGB555(20, 18, 16), RGB555(31, 28, 8)
  };

  static const uint16_t pal2[8] =
  {
    0, RGB555(10, 18, 6), RGB555(6, 26, 8), RGB555(14, 12, 10),
    RGB555(20, 20, 24), RGB555(24, 24, 28), RGB555(30, 30, 31),
    RGB555(31, 31, 20)
  };

  int x;
  int y;
  int c;

  /* The sheet: ship 16 x 16 at (0, 0), rock 16 x 16 at (16, 0), shot 8 x
   * 8 at (32, 0), big rock 32 x 16 at (0, 16)
   */

  memset(sheet, 0, sizeof(sheet));
  for (y = 0; y < 16; y++)
    {
      for (x = 0; x < 16; x++)
        {
          int dy = y - 8;
          int r2 = (x - 8) * (x - 8) + dy * dy;

          sheet[y * SHEET_W + x] = (x < 14 - iabs(dy) * 2 && x > 1) ?
                                   (x > 10 ? 1 : 2) : 0;
          sheet[y * SHEET_W + 16 + x] = r2 < 56 ?
                                        ((x * 7 + y * 3) % 5 ? 3 : 4) : 0;
        }
    }

  for (y = 0; y < 8; y++)
    {
      for (x = 0; x < 8; x++)
        {
          sheet[y * SHEET_W + 32 + x] = (x > 1 && x < 6 && y > 2 && y < 5) ?
                                        5 : 0;
        }
    }

  for (y = 0; y < 16; y++)
    {
      for (x = 0; x < 32; x++)
        {
          int dx = (x - 16) / 2;
          int dy = y - 8;

          sheet[(16 + y) * SHEET_W + x] = dx * dx + dy * dy < 56 ?
                                          ((x * 5 + y * 7) % 6 ? 3 : 4) : 0;
        }
    }

  g_sheet = fpc_image_new(SHEET_W, SHEET_H);
  fpc_image_put8(g_sheet, 0, 0, SHEET_W, SHEET_H, sheet, SHEET_W, pal1, 6);

  /* The tiles: the landscape's and the clouds' */

  memset(tiles, 0, sizeof(tiles));
  for (y = 0; y < 8; y++)
    {
      for (x = 0; x < 8; x++)
        {
          int r = y * 64;

          tiles[r + 1 * 8 + x] = ((x + y) & 3) == 0 ? 2 : 1;
          tiles[r + 2 * 8 + x] = y < 2 ? 3 : ((x + y) & 3) == 0 ? 2 : 1;
          tiles[r + 3 * 8 + x] = ((x * 3 + y * 5) % 7) < 3 ? 4 : 5;
          tiles[r + 4 * 8 + x] = 6;
          tiles[r + 5 * 8 + x] = x >= 7 - y ? 6 : 0;
          tiles[r + 6 * 8 + x] = x <= y ? 6 : 0;
          tiles[r + 7 * 8 + x] = (x == 3 && y == 3) ? 7 : 0;
        }
    }

  g_tiles = fpc_image_new(64, 8);
  fpc_image_put8(g_tiles, 0, 0, 64, 8, tiles, 64, pal2, 8);

  for (x = 0; x < 64; x++)
    {
      int h = 20 + ((x * 7) % 5) - (((x / 4) * 3) % 4);

      for (y = 0; y < 32; y++)
        {
          uint16_t e = 0;

          if (y == h)
            {
              e = FPC_TILE(2, x & 1, 0);
            }
          else if (y > h)
            {
              e = FPC_TILE(((x * 13 + y * 7) % 11) == 0 ? 3 : 1, 0, 0);
            }
          else if (((x * 31 + y * 17) % 23) == 0)
            {
              e = FPC_TILE(7, 0, 0);
            }

          map0[y * 64 + x] = e;
        }
    }

  memset(map1, 0, sizeof(map1));
  for (c = 0; c < 12; c++)
    {
      int cx = (c * 17) % 60;
      int cy = 2 + (c * 5) % 12;
      int w = 2 + c % 3;
      int i;

      map1[cy * 64 + cx] = FPC_TILE(5, 0, 0);
      for (i = 1; i <= w; i++)
        {
          map1[cy * 64 + cx + i] = FPC_TILE(4, 0, 0);
        }

      map1[cy * 64 + cx + w + 1] = FPC_TILE(6, 0, 0);
    }

  fpc_layer_init(0, g_tiles, 8, 8, 64, 32);
  fpc_layer_put(0, 0, 0, 64, 32, map0, 64);
  fpc_layer_init(1, g_tiles, 8, 8, 64, 32);
  fpc_layer_put(1, 0, 0, 64, 32, map1, 64);

  fpc_patch(0, g_laser);
  fpc_patch(1, g_boom);
  fpc_text_color(FPC_RGB(255, 255, 255));
}

static void spawn_rock(int i)
{
  g_rock[i].x = (int16_t)(SCREEN_W + fpc_random_range(0, 64));
  g_rock[i].y = (int16_t)fpc_random_range(8, 200);
  g_rock[i].dx = (int16_t)(-1 - fpc_random_range(0, 2));
  g_rock[i].dy = (int16_t)fpc_random_range(-1, 1);
  g_rock[i].big = i % 4 == 0;
  g_rock[i].alive = 1;
}

FPC_EXPORT(fpc_init) void fpc_init(void)
{
  int i;

  g_ship_x = 40;
  g_ship_y = 120;
  g_score  = 0;
  g_lives  = 3;
  g_flash  = 0;
  g_t      = 0;
  memset(g_shot, 0, sizeof(g_shot));
  for (i = 0; i < g_nrocks; i++)
    {
      spawn_rock(i);
      g_rock[i].x = (int16_t)fpc_random_range(160, SCREEN_W);
    }

  fpc_text_clear();
}

FPC_EXPORT(fpc_update) void fpc_update(void)
{
  uint32_t held = fpc_pad();
  int i;
  int s;

  g_ship_x += fpc_axis_x() / 32;
  g_ship_y -= fpc_axis_y() / 32;
  g_ship_x = g_ship_x < 0 ? 0 : g_ship_x > SCREEN_W - 16 ? SCREEN_W - 16 :
             g_ship_x;
  g_ship_y = g_ship_y < 0 ? 0 : g_ship_y > 224 ? 224 : g_ship_y;

  if ((fpc_pad_pressed() & FPC_PAD_A) ||
      ((held & FPC_PAD_A) && g_t % 8 == 0))
    {
      for (i = 0; i < N_SHOTS; i++)
        {
          if (!g_shot[i].live)
            {
              g_shot[i].live = 1;
              g_shot[i].x = (int16_t)(g_ship_x + 14);
              g_shot[i].y = (int16_t)(g_ship_y + 4);
              fpc_fm_on(i & 3, 0, fpc_note_freq(84 + (int)(g_t % 3)), 120,
                        120);
              break;
            }
        }
    }

  for (i = 0; i < N_SHOTS; i++)
    {
      if (g_shot[i].live)
        {
          g_shot[i].x += 6;
          if (g_shot[i].x > SCREEN_W)
            {
              g_shot[i].live = 0;
            }
        }
    }

  for (i = 0; i < g_nrocks; i++)
    {
      struct rock_s *r = &g_rock[i];
      int w;

      if (!r->alive)
        {
          if (g_t % 30 == 0)
            {
              spawn_rock(i);
            }

          continue;
        }

      r->x += r->dx;
      r->y += r->dy;
      w = r->big ? 32 : 16;
      if (r->x < -w)
        {
          spawn_rock(i);
        }

      if (r->y < -16)
        {
          r->y = 200;
        }
      else if (r->y > 200)
        {
          r->y = -16;
        }

      for (s = 0; s < N_SHOTS; s++)
        {
          if (g_shot[s].live && g_shot[s].x + 6 > r->x &&
              g_shot[s].x + 2 < r->x + w && g_shot[s].y + 5 > r->y &&
              g_shot[s].y + 3 < r->y + 16)
            {
              g_shot[s].live = 0;
              r->alive = 0;
              g_score += r->big ? 50 : 20;
              fpc_fm_on(4 + (i & 3), 1, fpc_note_freq(r->big ? 36 : 48),
                        200, 200);
            }
        }

      if (!g_bench && g_ship_x + 12 > r->x + 2 && g_ship_x + 2 < r->x + w - 2 &&
          g_ship_y + 12 > r->y + 2 && g_ship_y + 2 < r->y + 14 && !g_flash)
        {
          g_flash = 30;
          g_lives--;
          r->alive = 0;
          fpc_fm_on(8, 1, fpc_note_freq(30), 255, 255);
          if (g_lives <= 0)
            {
              g_score = 0;
              g_lives = 3;
            }
        }
    }

  if (g_flash)
    {
      g_flash--;
    }

  if (g_bench && g_t >= 600)
    {
      fpc_game_exit();                    /* The benchmark: 10 s */
    }

  g_t++;
}

FPC_EXPORT(fpc_draw) void fpc_draw(void)
{
  char line[64];
  int i;

  fpc_clear(RGB555(3, 5, 14));
  fpc_layer_draw(0, (int)(g_t / 2), 4);
  for (i = 0; i < N_SHOTS; i++)
    {
      if (g_shot[i].live)
        {
          fpc_sprite(g_sheet, 32, 0, 8, 8, g_shot[i].x, g_shot[i].y,
                     FPC_ADD);
        }
    }

  for (i = 0; i < g_nrocks; i++)
    {
      const struct rock_s *r = &g_rock[i];

      if (!r->alive)
        {
          continue;
        }

      if (r->big)
        {
          fpc_sprite_rot(g_sheet, 0, 16, 32, 16, r->x + 16, r->y + 8, 0,
                         (int)(g_t * 8 + i * 300) & 4095, 256);
        }
      else
        {
          fpc_sprite(g_sheet, 16, 0, 16, 16, r->x, r->y,
                     ((g_t >> 3) & i & 1) ? FPC_HFLIP : 0);
        }
    }

  if (!(g_flash & 4))
    {
      fpc_sprite(g_sheet, 0, 0, 16, 16, g_ship_x, g_ship_y, 0);
    }

  fpc_layer_draw(1, (int)(g_t / 4), (int)((g_t / 32) % 3));

  if (g_t % 6 == 0)
    {
      snprintf(line, sizeof(line), "SCORE %6d   SHIPS %d", g_score, g_lives);
      text(1, 1, line);
    }

  if (g_bench && g_t % 10 == 0)
    {
      uint32_t st[FPC_STAT_COUNT];

      fpc_game_stats(st, FPC_STAT_COUNT);
      if (g_t > 60)
        {
          g_frame_us_sum += st[FPC_STAT_FRAME_US];
          g_frame_us_n++;
        }

      snprintf(line, sizeof(line), "%d rocks  frame %5u us  max %5u us",
               g_nrocks, (unsigned)st[FPC_STAT_FRAME_US],
               (unsigned)st[FPC_STAT_FRAME_US_MAX]);
      text(1, 2, line);
    }

  fpc_text_draw();
}

int main(int argc, char **argv)
{
  int ret;

  if (argc >= 3 && strcmp(argv[1], "-b") == 0)
    {
      g_bench = atoi(argv[2]);
      g_nrocks = g_bench < 1 ? 1 : g_bench > MAX_ROCKS ? MAX_ROCKS : g_bench;
    }

  ret = fpc_game_run(424, 240);
  if (ret < 0)
    {
      fprintf(stderr, "rocks: %d\n", (int)ret);
      return 1;
    }

  if (g_bench && g_frame_us_n > 0)
    {
      printf("rocks: %d rocks, %u us a frame on average (update + draw "
             "+ sending)\n", g_nrocks,
             (unsigned)(g_frame_us_sum / g_frame_us_n));
    }

  return 0;
}
