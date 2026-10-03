/****************************************************************************
 * apps/graphics/fpcgame/fg_3ddemo.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * fg3d: the game SDK's 3D effects (fg_3d.c): a Mode 7 track the camera
 * circles (the pad steers it off its course), cards turned in 3D about
 * their centres (one shown only while it faces the viewer), and an image
 * drawn as a quadrilateral that breathes.  Everything it shows is made
 * here.  "fg3d N" ends after N frames, "fg3d N freeze" stops the picture
 * at frame N, "fg3d -s SCRIPT" plays scripted input (as fgdemo).
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpcgame.h"

#define MAP 256                           /* The track: MAP x MAP */
#define CARD 64

static int g_map;
static int g_card;
static int g_frames = -1;
static bool g_freeze;
static uint32_t g_f;
static int g_steer;                       /* The pad's turn, added to the course's */
static int g_lift;

static void make_map(void)
{
  static uint16_t row[MAP];
  int x;
  int y;

  g_map = fg_image_new(MAP, MAP);
  for (y = 0; y < MAP; y++)
    {
      for (x = 0; x < MAP; x++)
        {
          int dx = x - MAP / 2;
          int dy = y - MAP / 2;
          int d2 = dx * dx + dy * dy;
          uint16_t c;

          if (d2 > 80 * 80 && d2 < 104 * 104)          /* The road: a ring */
            {
              c = (d2 > 90 * 90 && d2 < 94 * 94 && ((x + y) & 8)) ?
                  FG_RGB(240, 240, 240) : FG_RGB(90, 90, 100);
            }
          else if ((d2 > 77 * 77 && d2 <= 80 * 80) || (d2 >= 104 * 104 && d2 < 107 * 107))
            {
              c = ((x ^ y) & 4) ? FG_RGB(220, 40, 40) : FG_RGB(240, 240, 240);   /* Kerbs */
            }
          else if (d2 < 30 * 30)
            {
              c = FG_RGB(40, 90, 200);                  /* A pond */
            }
          else
            {
              c = ((x >> 4) ^ (y >> 4)) & 1 ? FG_RGB(40, 150, 50) : FG_RGB(50, 170, 60);
            }

          row[x] = c;
        }

      fg_image_put(g_map, 0, y, MAP, 1, row, sizeof(row));
    }
}

static void make_card(void)
{
  static uint16_t px[CARD * CARD];
  int x;
  int y;

  g_card = fg_image_new(CARD, CARD);
  for (y = 0; y < CARD; y++)
    {
      for (x = 0; x < CARD; x++)
        {
          int dx = x * 2 - CARD + 1;
          int dy = y * 2 - CARD + 1;
          uint16_t c;

          if (x < 3 || y < 3 || x >= CARD - 3 || y >= CARD - 3)
            {
              c = FG_RGB(250, 210, 60);                 /* The border */
            }
          else if (dx * dx + dy * dy < 34 * 34 && dx * dx + dy * dy > 22 * 22)
            {
              c = FG_RGB(220, 40, 60);
            }
          else if ((dx > -6 && dx < 6 && dy > -12 && dy < 26) ||
                   (dy > -30 && dy <= -12 && (dx < 0 ? -dx : dx) < dy + 32))
            {
              c = FG_RGB(30, 30, 30);                   /* An arrow: up */
            }
          else
            {
              c = (x + y) & 8 ? FG_RGB(240, 240, 250) : FG_RGB(200, 210, 240);
            }

          px[y * CARD + x] = c;
        }
    }

  fg_image_put(g_card, 0, 0, CARD, CARD, px, CARD * 2);
}

static void demo_load(void)
{
  make_map();
  make_card();
}

static void demo_init(void)
{
  g_steer = 0;
  g_lift = 0;
}

static void demo_update(void)
{
  g_steer += fg_axis_x() >> 5;
  g_lift += fg_axis_y() >> 6;
  g_lift = g_lift < -16 ? -16 : g_lift > 40 ? 40 : g_lift;
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
  int a = (int)f * 8;                     /* Around the ring, 512 frames a lap */
  int32_t cx = (MAP / 2 << 8) + 92 * fg_cos(a);   /* (8.8) */
  int32_t cy = (MAP / 2 << 8) + 92 * fg_sin(a);
  int32_t qx[4];
  int32_t qy[4];
  int k;

  fg_clear(FG_RGB(120, 170, 240));        /* The sky, a band of haze */
  fg_rect(0, 70, fg_width(), 10, FG_RGB(170, 200, 240));

  /* Facing along the ring (its tangent: angle a + a half turn, as angle
   * 0 faces up the image), the pad's turn on top
   */

  fg_mode7(g_map, 0, fg_height(), cx, cy, a + 2048 + g_steer, 20 + g_lift, 80, 200, 0);

  /* Cards: turned about y, about x, and all three ways; the middle one
   * only while it faces the viewer
   */

  fg_quad3d(g_card, 0, 0, CARD, CARD, 70, 40, 0, (int)f * 12, 0, 0, 256);
  if (!fg_quad3d(g_card, 0, 0, CARD, CARD, 212, 40, 0, 0, (int)f * 10, 0, 256))
    {
      fg_rect(208, 38, 8, 4, FG_RGB(250, 250, 250));   /* (its back: a dash) */
    }

  fg_quad3d(g_card, 0, 0, CARD, CARD, 354, 40, 0, (int)f * 8, (int)f * 6, (int)f * 4, 200);

  /* A quadrilateral that breathes: its corners each on a circle */

  for (k = 0; k < 4; k++)
    {
      int ang = (int)f * 24 + k * 1024;
      int bx = (k == 1 || k == 2) ? 56 : 0;
      int by = k >= 2 ? 56 : 0;

      qx[k] = (12 + bx) * 16 + ((fg_cos(ang) * 6) >> 4);
      qy[k] = (170 + by) * 16 + ((fg_sin(ang) * 6) >> 4);
    }

  fg_quad(g_card, 0, 0, CARD, CARD, qx, qy, 0);

  fg_text_color(FG_RGB(255, 255, 255));
  fg_text_print(1, 1, "FPC 3D EFFECTS  frame %5u", (unsigned)f);
  fg_text_center(28, "arrows: steer, height   Esc: quit");
  fg_text_draw();
}

static const struct fg_game_s g_demo =
{
  "fg3d", 424, 240, demo_load, demo_init, demo_update, demo_draw, NULL
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
      fprintf(stderr, "fg3d: %d\n", ret);
      return EXIT_FAILURE;
    }

  printf("fg3d: %u frames, %u late\n", (unsigned)fg_stats()->frames,
         (unsigned)fg_stats()->late);
  return EXIT_SUCCESS;
}
