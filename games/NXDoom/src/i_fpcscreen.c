/****************************************************************************
 * apps/games/NXDoom/src/i_fpcscreen.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doom full screen on the FPC's GPU (CONFIG_GAMES_NXDOOM_FULLSCREEN;
 * drivers/gpu, protocol v8): the game takes the display and the keyboard
 * from the desktop as the game SDK's games do, in the desktop's own
 * display mode, and shows its 320 x 200 frame at 4:3, as on the CRTs it
 * was drawn for: 240 rows (every fifth row twice), scaled the most whole
 * times that fit (3 x 3, 960 x 720, in 1280 x 800, 1280 x 720 and
 * 1024 x 768; 2 x 2 in 800 x 600), in the middle with black around.  The
 * rows go to the GPU as PUTs whose pixels are looked up in the palette's
 * RGB565 table on the way into the link's batch (fpc_gpu_put8()), two a
 * group of five rows, straight from the game's frame: two canvases, drawn
 * in turn and FLIPped to.  The desktop's window path (8-bit frames scaled by
 * the desktop and the NX server) took as much CPU as Doom itself.  The
 * display and the keyboard go back to the desktop when the game ends
 * (Quit Game, an error, the window closed).
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/nx/nxkeys.h>

#include "fpc_gpu.h"
#include "i_fpcscreen.h"
#include "d_loop.h"
#include "m_argv.h"

#define FS_W        320                  /* The game's frame */
#define FS_H        200
#define FS_H43      240                  /* Its rows at 4:3 */

static struct
{
  bool     on;
  int      back;                         /* The canvas drawn next */
  int      scale;                        /* The display's, both ways */
  int      cw;                           /* A canvas: the display / scale */
  int      ch;
  int      ox;                           /* The frame in it */
  int      oy;
  int      x[2];                         /* The canvases in the space */
  int      y[2];
  uint16_t lut[256];                     /* The palette, RGB565 */
  uint32_t frames;
  uint64_t send_us;                      /* In fpcscreen_frame(), in all */
  uint32_t send_max;
  bool     hash;                         /* -framehash */
  uint32_t fnv;                          /* Of a frame every tenth tic */
  int      hashed;                       /* Its tic, the last */
  uint32_t nhashed;
} g_fs;

/* Take the display and the keyboard: false if the GPU or the desktop will
 * not have it (the game then plays in its window)
 */

bool fpcscreen_open(void)
{
  int w;
  int h;
  int s;
  int k;

  if (fpc_gpu_version() < 8)
    {
      return false;
    }

  /* The desktop's mode, the frame at 4:3 as large as it fits whole */

  if (fpc_gpu_modesize(fpc_gpu_mode(), &w, &h) < 0)
    {
      return false;
    }

  s = w / FS_W < h / FS_H43 ? w / FS_W : h / FS_H43;
  s = s < 1 ? 1 : s > 4 ? 4 : s;
  g_fs.scale = s;
  g_fs.cw    = (w + s - 1) / s;
  g_fs.ch    = (h + s - 1) / s;
  g_fs.ox    = ((g_fs.cw - FS_W) / 2) & ~1;  /* (PUT8: x even) */
  g_fs.oy    = (g_fs.ch - FS_H43) / 2;
  if (g_fs.ox < 0 || g_fs.oy < 0)
    {
      return false;
    }

  if (fpc_gpu_game_display(true) < 0)
    {
      return false;
    }

  for (k = 0; k < 2; k++)
    {
      if (fpc_gpu_game_alloc(g_fs.cw, g_fs.ch, &g_fs.x[k], &g_fs.y[k]) < 0)
        {
          fpc_gpu_game_free_all();
          fpc_gpu_game_display(false);
          return false;
        }

      fpc_gpu_fill(g_fs.x[k], g_fs.y[k], g_fs.cw, g_fs.ch, 0);
    }

  fpc_gpu_game_keys(true);
  fpc_gpu_set_scale(s, s);
  fpc_gpu_game_flip(g_fs.x[0], g_fs.y[0]);
  fpc_gpu_flush();
  g_fs.back = 1;
  g_fs.hash = m_check_parm("-framehash") > 0;
  g_fs.fnv = 2166136261u;
  g_fs.hashed = 0;
  g_fs.nhashed = 0;
  g_fs.on = true;
  syslog(LOG_INFO, "nxdoom: full screen, 320 x 200 at 4:3 (320 x 240) at "
         "%d x %d in %d x %d, canvases at (%d, %d) and (%d, %d)\n", s, s, w,
         h, g_fs.x[0], g_fs.y[0], g_fs.x[1], g_fs.y[1]);
  return true;
}

/* A frame: the 8-bit pixels and, when it changed, the palette (768 bytes,
 * 8 bits a channel)
 */

void fpcscreen_frame(const uint8_t *pixels, const uint8_t *rgbpal)
{
  int k = g_fs.back;
  int g;
  uint32_t t0;
  const uint16_t *lut;

  if (!g_fs.on)
    {
      return;
    }

  if (rgbpal != NULL)
    {
      int i;

      for (i = 0; i < 256; i++)
        {
          const uint8_t *c = rgbpal + 3 * i;

          g_fs.lut[i] = (uint16_t)((c[0] >> 3) << 11 | (c[1] >> 2) << 5 |
                                   (c[2] >> 3));
        }
    }

  /* -framehash: an FNV-1a of the first frame of every tenth tic and its
   * palette, logged at the end (a timedemo draws the same frames every
   * time, but for the wipe, which goes by the clock and is before the
   * first tic: a change to the renderer that should not change a pixel
   * can be checked)
   */

  if (g_fs.hash && gametic > 0 && gametic % 10 == 0 &&
      gametic != g_fs.hashed)
    {
      uint32_t h = g_fs.fnv;
      int i;

      for (i = 0; i < FS_W * FS_H; i++)
        {
          h = (h ^ pixels[i]) * 16777619u;
        }

      for (i = 0; i < 256; i++)
        {
          h = (h ^ g_fs.lut[i]) * 16777619u;
        }

      g_fs.fnv = h;
      g_fs.hashed = gametic;
      g_fs.nhashed++;
    }

  t0 = fpc_gpu_game_us();
  /* 200 rows as 240: each group of five rows as six, its middle one
   * twice (rows 0-2 and 2-4 of the group, one PUT each).  The palette
   * with the first PUT only: the driver compares the one a PUT brings
   * with what the GPU has (512 bytes), 80 times a frame; NULL is the one
   * last sent, where a PUT8 goes as one (a v10 GPU, x and w even:
   * fpc_gpu.h)
   */

  lut = g_fs.lut;
  for (g = 0; g < FS_H / 5; g++)
    {
      const uint8_t *src = pixels + g * 5 * FS_W;
      int y = g_fs.y[k] + g_fs.oy + g * 6;

      fpc_gpu_put8(g_fs.x[k] + g_fs.ox, y, FS_W, 3, src, FS_W, lut);
      if (fpc_gpu_version() >= 10 && ((g_fs.x[k] + g_fs.ox) & 1) == 0)
        {
          lut = NULL;
        }

      fpc_gpu_put8(g_fs.x[k] + g_fs.ox, y + 3, FS_W, 3, src + 2 * FS_W,
                   FS_W, lut);
    }

  fpc_gpu_game_flip(g_fs.x[k], g_fs.y[k]);
  fpc_gpu_flush();
  t0 = fpc_gpu_game_us() - t0;
  g_fs.send_us += t0;
  g_fs.send_max = t0 > g_fs.send_max ? t0 : g_fs.send_max;
  g_fs.frames++;
  g_fs.back = 1 - k;
}

/* The next key (the desktop hands them to the game while it has them) */

bool fpcscreen_key(struct nx_key_s *key)
{
  return g_fs.on && fpc_gpu_game_key(&key->flags, &key->code, &key->ch) > 0;
}

bool fpcscreen_active(void)
{
  return g_fs.on;
}

/* The display and the keyboard back to the desktop, as it left them */

void fpcscreen_close(void)
{
  if (!g_fs.on)
    {
      return;
    }

  g_fs.on = false;
  syslog(LOG_INFO, "nxdoom: %u frames sent, %u us each on average, the "
         "longest %u us\n", (unsigned)g_fs.frames,
         (unsigned)(g_fs.frames ? g_fs.send_us / g_fs.frames : 0),
         (unsigned)g_fs.send_max);
  if (g_fs.hash)
    {
      syslog(LOG_INFO, "nxdoom: frame hash %08x over %u frames, the last "
             "at tic %d\n", (unsigned)g_fs.fnv, (unsigned)g_fs.nhashed,
             g_fs.hashed);
    }

  g_fs.frames = 0;
  g_fs.send_us = 0;
  g_fs.send_max = 0;
  fpc_gpu_game_free_all();
  fpc_gpu_game_keys(false);
  fpc_gpu_game_display(false);
  fpc_gpu_flush();
}
