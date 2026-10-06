/****************************************************************************
 * apps/graphics/fpcgame/fg_core.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The game SDK's frame loop: the display and the keyboard taken from the
 * desktop (drivers/gpu: fpc_gpu_game_display(), _keys()), the canvases,
 * sixty frames a second, the pad, time and random numbers.
 *
 * The display: of the modes, the one where the canvas comes out largest
 * at a whole scale (424 x 240: 1280 x 720 at 3 x 3; 320 x 240: 1024 x 768
 * at 3), the canvas in the middle of what the display then shows.  Two
 * canvases, each that whole view (its margins black), in the GPU's memory:
 * a frame is drawn into the one not shown, then FLIPped to.
 *
 * Pacing: after the FLIP, a READ of one pixel of the canvas waits for the
 * frame's drawing and for the FLIP's frame to start (the GPU runs nothing
 * after a FLIP before then), so a frame is built while the last one is
 * shown and the loop keeps the display's sixty frames a second.  Without
 * frames (the NuttX simulator's GPU model answers at once) the loop sleeps
 * out the 16.7 ms.
 ****************************************************************************/

#include "fg_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/input/kbd_codec.h>
#include <nuttx/nx/nxkeys.h>
#ifdef CONFIG_INPUT_AJOYSTICK
#  include <nuttx/input/ajoystick.h>
#endif

#include "fpc_gpu.h"

#define FG_PERIOD_US   16667
#define FG_DEF_W       424
#define FG_DEF_H       240
#define FG_AXIS        107                /* A pad's stick past its dead zone */

struct fg_s g_fg;

/****************************************************************************
 * Time and random numbers
 ****************************************************************************/

uint64_t fg_now_us(void)
{
  /* The driver's microsecond clock (the system's moves in 1 ms ticks),
   * made 64-bit here (it is read at least every frame)
   */

  static uint32_t last;
  static uint64_t hi;
  uint32_t now = fpc_gpu_game_us();

  if (now < last)
    {
      hi += 1ull << 32;
    }

  last = now;
  return hi | now;
}

uint32_t fg_time_us(void)
{
  return (uint32_t)(fg_now_us() - g_fg.t0);
}

uint32_t fg_time_ms(void)
{
  return (uint32_t)((fg_now_us() - g_fg.t0) / 1000);
}

uint32_t fg_frame(void)
{
  return g_fg.frame;
}

int fg_width(void)
{
  return g_fg.w;
}

int fg_height(void)
{
  return g_fg.h;
}

uint32_t fg_random(void)
{
  uint32_t x = g_fg.rnd;

  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_fg.rnd = x;
  return x;
}

int fg_random_range(int lo, int hi)
{
  if (hi < lo)
    {
      return lo;
    }

  return lo + (int)(fg_random() % (uint32_t)(hi - lo + 1));
}

void fg_random_seed(uint32_t seed)
{
  g_fg.rnd = seed != 0 ? seed : 0x2545f491;
}

void fg_exit(void)
{
  g_fg.quit = true;
}

void fg_script(FAR const char *script)
{
  g_fg.script = script;
}

FAR const struct fg_stats_s *fg_stats(void)
{
  return &g_fg.st;
}

/****************************************************************************
 * The pad: keys held down, by what they are on the pad
 ****************************************************************************/

static struct
{
  uint32_t held;                          /* Pad bits, from the keys held */
  uint32_t now;                           /* This frame's */
  uint32_t last;
  uint8_t  down[48];                      /* Keys held: their codes */
  int      ndown;
  int      joyfd;                         /* /dev/ajoy0, or -1 */
} g_pad;

#ifdef CONFIG_INPUT_AJOYSTICK
/* A real game pad, /dev/ajoy0 (the network coprocessor's BLE pads,
 * fpc/drivers/netcop): its stick or hat as the directions, its buttons
 * (A, B, X, Y, L, R, Select, Start) as A, B, C, D, the stick's click,
 * the stick's click, Select, Start
 */

static uint32_t joy_bits(void)
{
  struct ajoy_sample_s s;
  uint32_t bits = 0;

  if (g_pad.joyfd < 0 ||
      read(g_pad.joyfd, &s, sizeof(s)) != (ssize_t)sizeof(s))
    {
      return 0;
    }

  bits |= s.as_x < -16384 ? FG_PAD_LEFT : s.as_x > 16384 ? FG_PAD_RIGHT : 0;
  bits |= s.as_y < -16384 ? FG_PAD_UP : s.as_y > 16384 ? FG_PAD_DOWN : 0;
  bits |= (s.as_buttons & AJOY_BUTTON_1_BIT) ? FG_PAD_A : 0;
  bits |= (s.as_buttons & AJOY_BUTTON_2_BIT) ? FG_PAD_B : 0;
  bits |= (s.as_buttons & AJOY_BUTTON_3_BIT) ? FG_PAD_C : 0;
  bits |= (s.as_buttons & AJOY_BUTTON_4_BIT) ? FG_PAD_D : 0;
  bits |= (s.as_buttons & (AJOY_BUTTON_5_BIT | AJOY_BUTTON_6_BIT)) ?
          FG_PAD_STICK : 0;
  bits |= (s.as_buttons & AJOY_BUTTON_7_BIT) ? FG_PAD_SELECT : 0;
  bits |= (s.as_buttons & AJOY_BUTTON_8_BIT) ? FG_PAD_START : 0;
  return bits;
}
#endif

static uint32_t key_bit(uint8_t code)
{
  if (NX_KEY_ISSPECIAL(code))
    {
      switch (NX_KEY_KEYCODE(code))
        {
          case KEYCODE_LEFT:  return FG_PAD_LEFT;
          case KEYCODE_RIGHT: return FG_PAD_RIGHT;
          case KEYCODE_UP:    return FG_PAD_UP;
          case KEYCODE_DOWN:  return FG_PAD_DOWN;
          default:            return 0;
        }
    }

  switch (code | 0x20)                    /* (letters either case) */
    {
      case 'a': return FG_PAD_LEFT;
      case 'd': return FG_PAD_RIGHT;
      case 'w': return FG_PAD_UP;
      case 's': return FG_PAD_DOWN;
      case 'z':
      case 'j': return FG_PAD_A;
      case 'x':
      case 'k': return FG_PAD_B;
      case 'c':
      case 'l': return FG_PAD_C;
      case 'v': return FG_PAD_D;
      case 'b': return FG_PAD_STICK;
      default:  break;
    }

  switch (code)
    {
      case ' ':          return FG_PAD_D;
      case NX_KEY_ENTER: return FG_PAD_START;
      case NX_KEY_TAB:   return FG_PAD_SELECT;
      default:           return 0;
    }
}

static void key_event(bool release, uint8_t code)
{
  int i;

  if (!release)
    {
      if (code == NX_KEY_ESCAPE)
        {
          g_fg.quit = true;               /* Escape: back to the desktop */
        }

      if (code == NX_KEY_SPECIAL(KEYCODE_PAUSE) ||
          code == NX_KEY_SPECIAL(KEYCODE_F12))
        {
          g_fg.frozen = !g_fg.frozen;     /* Pause, F12: the picture held */
          return;
        }

      for (i = 0; i < g_pad.ndown && g_pad.down[i] != code; i++);
      if (i == g_pad.ndown && g_pad.ndown < (int)sizeof(g_pad.down))
        {
          g_pad.down[g_pad.ndown++] = code;
        }
    }
  else
    {
      for (i = 0; i < g_pad.ndown; i++)
        {
          if (g_pad.down[i] == code)
            {
              g_pad.down[i] = g_pad.down[--g_pad.ndown];
              break;
            }
        }
    }
}

/* fg_script()'s key names */

static int script_key(FAR const char *w, int n)
{
  static const struct
  {
    FAR const char *name;
    uint8_t code;
  }
  keys[] =
  {
    { "left",  NX_KEY_SPECIAL(KEYCODE_LEFT) },
    { "right", NX_KEY_SPECIAL(KEYCODE_RIGHT) },
    { "up",    NX_KEY_SPECIAL(KEYCODE_UP) },
    { "down",  NX_KEY_SPECIAL(KEYCODE_DOWN) },
    { "space", ' ' },
    { "enter", NX_KEY_ENTER },
    { "tab",   NX_KEY_TAB },
  };

  int i;

  if (n == 1 && w[0] >= 'a' && w[0] <= 'z')
    {
      return w[0];
    }

  for (i = 0; i < (int)(sizeof(keys) / sizeof(keys[0])); i++)
    {
      if ((int)strlen(keys[i].name) == n && strncmp(keys[i].name, w, n) == 0)
        {
          return keys[i].code;
        }
    }

  syslog(LOG_WARNING, "fpcgame: script: no key %.*s\n", n, w);
  return -1;
}

/* The script's words up to this frame */

static void script_frame(void)
{
  static uint8_t tap[8];                  /* Pressed for one frame */
  static int ntap;
  FAR const char *s = g_fg.script;
  int i;

  for (i = 0; i < ntap; i++)
    {
      key_event(true, tap[i]);
    }

  ntap = 0;
  while (s != NULL && *s != '\0')
    {
      FAR const char *w;
      int n;
      int k;

      while (*s == ' ')
        {
          s++;
        }

      for (w = s; *s != '\0' && *s != ' '; s++);
      n = (int)(s - w);
      if (n == 0)
        {
          break;
        }

      if (w[0] >= '0' && w[0] <= '9')
        {
          g_fg.script_at = (uint32_t)atoi(w);
          if (g_fg.script_at > g_fg.frame)
            {
              s = w;                      /* A later frame's: wait */
              break;
            }
        }
      else if (n == 6 && strncmp(w, "freeze", 6) == 0)
        {
          g_fg.frozen = true;
        }
      else if (n == 4 && strncmp(w, "quit", 4) == 0)
        {
          g_fg.quit = true;
        }
      else if (w[0] == '+' || w[0] == '-')
        {
          if ((k = script_key(w + 1, n - 1)) >= 0)
            {
              key_event(w[0] == '-', (uint8_t)k);
            }
        }
      else if ((k = script_key(w, n)) >= 0 && ntap < (int)sizeof(tap))
        {
          key_event(false, (uint8_t)k);
          tap[ntap++] = (uint8_t)k;
        }
    }

  g_fg.script = s;
}

void fg_input_frame(void)
{
  uint8_t flags;
  uint8_t code;
  uint8_t ch;
  int i;

  if (!g_fg.frozen)
    {
      script_frame();
    }

  while (fpc_gpu_game_key(&flags, &code, &ch) > 0)
    {
      key_event((flags & NX_KEY_RELEASE) != 0, code);
    }

  g_pad.held = 0;
  for (i = 0; i < g_pad.ndown; i++)
    {
      g_pad.held |= key_bit(g_pad.down[i]);
    }

#ifdef CONFIG_INPUT_AJOYSTICK
  g_pad.held |= joy_bits();
#endif

  g_pad.last = g_pad.now;
  g_pad.now  = g_pad.held;
}

uint32_t fg_pad(void)
{
  return g_pad.now;
}

uint32_t fg_pad_pressed(void)
{
  return g_pad.now & ~g_pad.last;
}

uint32_t fg_pad_released(void)
{
  return g_pad.last & ~g_pad.now;
}

int fg_axis_x(void)
{
  return ((g_pad.now & FG_PAD_RIGHT) ? FG_AXIS : 0) -
         ((g_pad.now & FG_PAD_LEFT) ? FG_AXIS : 0);
}

int fg_axis_y(void)
{
  return ((g_pad.now & FG_PAD_UP) ? FG_AXIS : 0) -
         ((g_pad.now & FG_PAD_DOWN) ? FG_AXIS : 0);
}

/****************************************************************************
 * The display
 ****************************************************************************/

/* The mode and scale that show a w x h canvas largest */

static void pick_display(int w, int h, FAR int *mode, FAR int *scale)
{
  static const int order[FPC_GPU_NMODES] = { 2, 0, 3, 1 };
  int best = -1;
  int bestcov = 0;
  int i;

  *mode  = 2;
  *scale = 1;
  for (i = 0; i < FPC_GPU_NMODES; i++)
    {
      int mw;
      int mh;
      int s;
      int cov;

      if (fpc_gpu_modesize(order[i], &mw, &mh) < 0)
        {
          continue;
        }

      s = mw / w < mh / h ? mw / w : mh / h;
      s = s > 4 ? 4 : s;
      if (s < 1)
        {
          continue;
        }

      cov = (int)((int64_t)w * s * h * s * 1000 / ((int64_t)mw * mh));
      if (s > best || (s == best && cov > bestcov))
        {
          best    = s;
          bestcov = cov;
          *mode   = order[i];
          *scale  = s;
        }
    }
}

static int display_open(int w, int h)
{
  int mw;
  int mh;
  int vw;
  int vh;
  int k;

  pick_display(w, h, &g_fg.mode, &g_fg.scale);
  fpc_gpu_modesize(g_fg.mode, &mw, &mh);
  vw = (mw + g_fg.scale - 1) / g_fg.scale;      /* What the display shows */
  vh = (mh + g_fg.scale - 1) / g_fg.scale;
  g_fg.mx = (vw - w) / 2;
  g_fg.my = (vh - h) / 2;

  for (k = 0; k < 2; k++)
    {
      int x;
      int y;

      if (fpc_gpu_game_alloc((vw + 3) & ~3, vh, &x, &y) < 0)
        {
          return -ENOMEM;
        }

      g_fg.canvas[k].x = x;
      g_fg.canvas[k].y = y;
      g_fg.canvas[k].w = vw;
      g_fg.canvas[k].h = vh;
      fpc_gpu_fill(x, y, vw, vh, 0);
    }

  fpc_gpu_game_mode(g_fg.mode);
  fpc_gpu_set_scale(g_fg.scale, g_fg.scale);
  fpc_gpu_game_flip(g_fg.canvas[0].x, g_fg.canvas[0].y);
  fpc_gpu_flush();

  g_fg.back = 1;
  g_fg.ox = g_fg.canvas[1].x + g_fg.mx;
  g_fg.oy = g_fg.canvas[1].y + g_fg.my;
  syslog(LOG_INFO, "fpcgame: %s: %d x %d in mode %d at %d x %d, "
         "canvases at (%d, %d) and (%d, %d)\n",
         g_fg.game->name ? g_fg.game->name : "game", w, h, g_fg.mode,
         g_fg.scale, g_fg.scale, g_fg.canvas[0].x + g_fg.mx,
         g_fg.canvas[0].y + g_fg.my, g_fg.canvas[1].x + g_fg.mx,
         g_fg.canvas[1].y + g_fg.my);
  return OK;
}

/* The frame drawn: show it, then wait until it is shown (or a frame's
 * time has passed), and draw the next into the other canvas
 */

static void present(void)
{
  FAR struct fg_rect_s *c = &g_fg.canvas[g_fg.back];
  static uint64_t last;
  uint64_t t0;
  uint64_t t1;
  uint16_t px;

  fpc_gpu_game_flip(c->x, c->y);
  fpc_gpu_flush();
  t0 = fg_now_us();
  fpc_gpu_read(c->x, c->y, 1, 1, &px, 2);       /* Drawn and shown */
  t1 = fg_now_us();

  if (t1 - t0 < 2000 && last != 0 && t1 - last < FG_PERIOD_US &&
      !g_fg.scripted)
    {
      usleep(FG_PERIOD_US - (uint32_t)(t1 - last));   /* No frames (the simulator) */
      t1 = fg_now_us();
    }

  if (last != 0 && t1 - last > FG_PERIOD_US + FG_PERIOD_US / 2)
    {
      g_fg.st.late++;
    }

  g_fg.st.wait_us = (uint32_t)(t1 - t0);
  last = t1;

  g_fg.back ^= 1;
  g_fg.ox = g_fg.canvas[g_fg.back].x + g_fg.mx;
  g_fg.oy = g_fg.canvas[g_fg.back].y + g_fg.my;
}

/****************************************************************************
 * Running a game
 ****************************************************************************/

int fg_run(FAR const struct fg_game_s *game)
{
  uint64_t tsec;
  uint32_t maxus = 0;
  uint32_t worst = 0;
  uint64_t total = 0;                     /* The frames' draw times */
  FAR const char *script;
  int ret;

  if (fpc_gpu_version() < 8)
    {
      fprintf(stderr, "fpcgame: the GPU speaks protocol v%d: games need v8\n",
              fpc_gpu_version());
      return -ENOSYS;
    }

  script = g_fg.script;
  memset(&g_fg, 0, sizeof(g_fg));
  memset(&g_pad, 0, sizeof(g_pad));
#ifdef CONFIG_INPUT_AJOYSTICK
  g_pad.joyfd   = open("/dev/ajoy0", O_RDONLY | O_NONBLOCK);
#else
  g_pad.joyfd   = -1;
#endif
  g_fg.script   = script;
  g_fg.scripted = script != NULL;
  g_fg.game = game;
  g_fg.w = game->width > 0 ? game->width : FG_DEF_W;
  g_fg.h = game->height > 0 ? game->height : FG_DEF_H;
  g_fg.t0 = fg_now_us();
  fg_random_seed(0);

  ret = fpc_gpu_game_display(true);
  if (ret < 0)
    {
      fprintf(stderr, "fpcgame: the display is another game's\n");
      return ret;
    }

  fpc_gpu_game_keys(true);
  ret = display_open(g_fg.w, g_fg.h);
  if (ret < 0)
    {
      fprintf(stderr, "fpcgame: no room in the GPU's memory\n");
      goto out;
    }

  fg_audio_start();
  if (game->load != NULL)
    {
      game->load();
    }

  if (game->init != NULL)
    {
      game->init();
    }

  tsec = fg_now_us();
  while (!g_fg.quit)
    {
      uint64_t t = fg_now_us();

      g_fg.sprites = 0;
      g_fg.tiles   = 0;
      fg_input_frame();
      if (g_fg.frozen)
        {
          /* Nothing drawn or flipped: the canvas shown stays as it is
           * (and can be read from the GPU's memory: grab.py)
           */

          usleep(FG_PERIOD_US);
          continue;
        }

      if (game->update != NULL)
        {
          game->update();
        }

      fg_audio_frame();
      if (game->draw != NULL)
        {
          game->draw();
        }

      g_fg.st.frame_us = (uint32_t)(fg_now_us() - t);
      g_fg.st.sprites  = g_fg.sprites;
      g_fg.st.tiles    = g_fg.tiles;
      maxus = g_fg.st.frame_us > maxus ? g_fg.st.frame_us : maxus;
      worst = g_fg.st.frame_us > worst ? g_fg.st.frame_us : worst;
      total += g_fg.st.frame_us;
      if (fg_now_us() - tsec >= 1000000)
        {
          g_fg.st.frame_us_max = maxus;
          maxus = 0;
          tsec  = fg_now_us();
        }

      present();
      g_fg.frame++;
      g_fg.st.frames = g_fg.frame;
    }

  if (game->quit != NULL)
    {
      game->quit();
    }

  syslog(LOG_INFO, "fpcgame: %s: %u frames, %u late; a frame took %u us "
         "to draw on average, the worst %u us\n",
         game->name ? game->name : "game", (unsigned)g_fg.frame,
         (unsigned)g_fg.st.late,
         (unsigned)(g_fg.frame ? total / g_fg.frame : 0), (unsigned)worst);
  ret = OK;

out:
  if (g_pad.joyfd >= 0)
    {
      close(g_pad.joyfd);
      g_pad.joyfd = -1;
    }

  g_fg.script = NULL;                     /* (globals outlive a task here) */
  fg_audio_stop();
  fg_gfx_reset();
  fpc_gpu_game_free_all();
  fpc_gpu_game_keys(false);
  fpc_gpu_game_display(false);
  return ret;
}
