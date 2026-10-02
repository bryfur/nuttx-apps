/****************************************************************************
 * apps/graphics/fpcgame/fg_bench.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fgbench: how many sprites of a kind the GPU draws in a frame at 60
 * frames a second.  For each kind the count is raised by a quarter while
 * a stretch of frames has none late, then halved in between the last that
 * held and the first that did not; the result is the most that held, with
 * what a frame took the CPU (update and draw: building and sending the
 * commands) there.  On the board only (the simulator has no frames).
 *   fgbench            every kind
 *   fgbench KIND...    some: key16 half16 add16 rot16 rot32 key32
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpcgame.h"

#define MAXN      8000                    /* Sprites a frame at most */
#define SETTLE    10                      /* Frames before a stretch counts */
#define STRETCH   90                      /* Frames a count is tried for */

struct kind_s
{
  FAR const char *name;
  FAR const char *what;
  int size;                               /* 16 or 32 */
  uint32_t flags;
  bool rot;
};

static const struct kind_s g_kinds[] =
{
  { "key16",  "16 x 16 keyed (BLEND, opaque)",          16, 0,       false },
  { "half16", "16 x 16 keyed, 50 % (BLEND, reads D)",   16, FG_HALF, false },
  { "add16",  "16 x 16 keyed, added (BLEND, reads D)",  16, FG_ADD,  false },
  { "rot16",  "16 x 16 rotated and scaled (SPRITE)",    16, 0,       true  },
  { "key32",  "32 x 32 keyed (BLEND, opaque)",          32, 0,       false },
  { "rot32",  "32 x 32 rotated and scaled (SPRITE)",    32, 0,       true  },
};

#define NKINDS (int)(sizeof(g_kinds) / sizeof(g_kinds[0]))

static int g_sheet;
static int16_t g_x[MAXN] FG_PSRAM;
static int16_t g_y[MAXN] FG_PSRAM;
static bool g_want[NKINDS];
static int g_kind = -1;                   /* Being measured */
static int g_n;                           /* Sprites a frame now */
static int g_good;                        /* The most that held */
static int g_bad;                         /* The least that did not */
static uint32_t g_t;                      /* Frames into the stretch */
static uint32_t g_late0;
static uint64_t g_us;                     /* CPU time over the stretch */
static uint32_t g_good_us;                /* A frame's CPU time at g_good */
static int g_result[NKINDS];
static uint32_t g_result_us[NKINDS];

static void bench_load(void)
{
  static uint16_t px[48 * 32] FG_PSRAM;   /* An orb 16 x 16, a ship 32 x 32 */
  int x;
  int y;

  for (y = 0; y < 32; y++)
    {
      for (x = 0; x < 48; x++)
        {
          uint16_t c = FG_KEY;

          if (x < 16 && y < 16)
            {
              int r = (x - 8) * (x - 8) + (y - 8) * (y - 8);

              c = r < 50 ? FG_RGB(255 - r * 4, 120 + r, 40) : FG_KEY;
            }
          else if (x >= 16)
            {
              int dy = abs(y - 16);

              c = (x - 16 >= 2 && dy <= (x - 18) / 3 + 1) ?
                  FG_RGB(80 + (x - 16) * 5, 90, 200) : FG_KEY;
            }

          px[y * 48 + x] = c;
        }
    }

  g_sheet = fg_image_new(48, 32);
  fg_image_put(g_sheet, 0, 0, 48, 32, px, 96);
}

static void bench_init(void)
{
  int i;

  for (i = 0; i < MAXN; i++)              /* Inside the canvas, either size */
    {
      g_x[i] = (int16_t)fg_random_range(24, fg_width() - 56);
      g_y[i] = (int16_t)fg_random_range(24, fg_height() - 56);
    }
}

static bool next_kind(void)
{
  while (++g_kind < NKINDS && !g_want[g_kind]);
  if (g_kind == NKINDS)
    {
      return false;
    }

  g_n    = 50;
  g_good = 0;
  g_bad  = 0;
  g_good_us = 0;
  return true;
}

static void bench_update(void)
{
  uint32_t late;

  if (g_kind < 0 && !next_kind())
    {
      fg_exit();
      return;
    }

  g_t++;
  if (g_t <= SETTLE)
    {
      g_late0 = fg_stats()->late;
      g_us = 0;
      return;
    }

  g_us += fg_stats()->frame_us;
  late = fg_stats()->late - g_late0;
  if (late == 0 && g_t < SETTLE + STRETCH)
    {
      return;                             /* So far so good */
    }

  if (late == 0)
    {
      g_good = g_n;
      g_good_us = (uint32_t)(g_us / STRETCH);
    }
  else
    {
      g_bad = g_n;
    }

  if (g_bad == 0)                         /* Up by a quarter */
    {
      g_n = g_n + g_n / 4 > MAXN ? MAXN : g_n + g_n / 4;
      if (g_good == MAXN)
        {
          g_bad = MAXN + 1;
        }
    }
  else
    {
      g_n = (g_good + g_bad) / 2;         /* In between */
    }

  if (g_bad != 0 && g_bad - g_good <= (g_good / 50 > 2 ? g_good / 50 : 2))
    {
      g_result[g_kind]    = g_good;
      g_result_us[g_kind] = g_good_us;
      printf("fgbench: %-6s %5d a frame (%6d a second), a frame %5u us "
             "of CPU: %s\n", g_kinds[g_kind].name, g_good, g_good * 60,
             (unsigned)g_good_us, g_kinds[g_kind].what);
      if (!next_kind())
        {
          fg_exit();
          return;
        }
    }

  g_t = 0;
}

static void bench_draw(void)
{
  FAR const struct kind_s *k;
  int i;

  fg_clear(FG_RGB(10, 10, 30));
  if (g_kind < 0 || g_kind >= NKINDS)
    {
      return;
    }

  k = &g_kinds[g_kind];
  for (i = 0; i < g_n; i++)
    {
      int sx = k->size == 16 ? 0 : 16;

      if (k->rot)
        {
          fg_sprite_rot(g_sheet, sx, 0, k->size, k->size,
                        g_x[i] + k->size / 2, g_y[i] + k->size / 2, 0,
                        (int)((i * 97 + fg_frame() * 8) & 4095),
                        256 - (i & 63));
        }
      else
        {
          fg_sprite(g_sheet, sx, 0, k->size, k->size, g_x[i], g_y[i],
                    k->flags);
        }
    }

  fg_text_print(1, 1, "fgbench %s: %d", k->name, g_n);
  fg_text_draw();
}

static const struct fg_game_s g_bench =
{
  "fgbench", 424, 240, bench_load, bench_init, bench_update, bench_draw,
  NULL
};

int main(int argc, FAR char *argv[])
{
  int ret;
  int i;
  int j;

  for (i = 0; i < NKINDS; i++)
    {
      g_want[i] = argc < 2;
      for (j = 1; j < argc; j++)
        {
          g_want[i] |= strcmp(argv[j], g_kinds[i].name) == 0;
        }
    }

  g_kind = -1;
  g_t = 0;
  ret = fg_run(&g_bench);
  if (ret < 0)
    {
      fprintf(stderr, "fgbench: %d\n", ret);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
