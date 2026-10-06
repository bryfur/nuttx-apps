/****************************************************************************
 * apps/graphics/fpcwasm/sdk/fpc_native.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fpc API (fpc_wasm.h) for a native NuttX program: each function
 * straight onto the game SDK (apps/graphics/fpcgame), no sandbox.  An
 * app's C source builds both ways, as a WebAssembly app and as a native
 * one, for comparing the two (rocks: `rocksn`, docs/wasm.md) or for
 * debugging on the host's tools.  fpc_game_run() calls the app's fpc_load
 * ... fpc_quit (empty ones for those it lacks; fpc_draw it must have).
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/input/kbd_codec.h>
#include <nuttx/nx/nxkeys.h>

#include "fpcgame.h"
#include "fpc_wasm.h"

/* The callbacks an app leaves out: empty (weak definitions, not weak
 * references, which a loadable module cannot have unresolved)
 */

void weak_function fpc_load(void)
{
}

void weak_function fpc_init(void)
{
}

void weak_function fpc_update(void)
{
}

void weak_function fpc_quit(void)
{
}

void fpc_draw(void);

static bool g_in_game;
static const char *g_script;

/****************************************************************************
 * The system, the game
 ****************************************************************************/

int32_t fpc_api_version(void)
{
  return FPC_API_VERSION;
}

void fpc_log(const char *text, int32_t len)
{
  printf("%.*s\n", (int)len, text);
}

uint32_t fpc_time_us(void)
{
  struct timespec ts;

  if (g_in_game)
    {
      return fg_time_us();
    }

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)ts.tv_sec * 1000000u + (uint32_t)(ts.tv_nsec / 1000);
}

uint32_t fpc_time_ms(void)
{
  return fpc_time_us() / 1000;
}

void fpc_sleep_ms(int32_t ms)
{
  usleep(ms > 0 ? ms * 1000 : 0);
}

uint32_t fpc_random(void)
{
  return fg_random();
}

int32_t fpc_random_range(int32_t lo, int32_t hi)
{
  if (hi < lo)
    {
      return lo;
    }

  return lo + (int32_t)(fg_random() % ((uint32_t)hi - (uint32_t)lo + 1u));
}

void fpc_random_seed(uint32_t seed)
{
  fg_random_seed(seed);
}

int32_t fpc_sin(int32_t angle)
{
  return fg_sin(angle & 4095);
}

int32_t fpc_cos(int32_t angle)
{
  return fg_cos(angle & 4095);
}

/* The script for fg_script() (a native program's own -s option) */

void fpc_native_script(const char *script)
{
  g_script = script;
}

int32_t fpc_game_run(int32_t width, int32_t height)
{
  struct fg_game_s game;
  int ret;

  memset(&game, 0, sizeof(game));
  game.name   = "native";
  game.width  = width;
  game.height = height;
  game.load   = fpc_load;
  game.init   = fpc_init;
  game.update = fpc_update;
  game.draw   = fpc_draw;
  game.quit   = fpc_quit;
  if (g_script != NULL)
    {
      fg_script(g_script);
    }

  g_in_game = true;
  ret = fg_run(&game);
  g_in_game = false;
  return ret;
}

void fpc_game_exit(void)
{
  fg_exit();
}

int32_t fpc_game_width(void)
{
  return fg_width();
}

int32_t fpc_game_height(void)
{
  return fg_height();
}

uint32_t fpc_game_frame(void)
{
  return fg_frame();
}

int32_t fpc_game_display(int32_t *out, int32_t n)
{
  int v[4];
  int i;

  fg_display_info(&v[0], &v[1], &v[2], &v[3]);
  for (i = 0; i < n && i < 4; i++)
    {
      out[i] = v[i];
    }

  return 4;
}

int32_t fpc_game_stats(uint32_t *out, int32_t n)
{
  const struct fg_stats_s *st = fg_stats();
  uint32_t v[7];
  int i;

  v[0] = st->frames;
  v[1] = st->frame_us;
  v[2] = st->frame_us_max;
  v[3] = st->wait_us;
  v[4] = st->late;
  v[5] = st->sprites;
  v[6] = st->tiles;
  for (i = 0; i < n && i < 7; i++)
    {
      out[i] = v[i];
    }

  return 7;
}

/****************************************************************************
 * Images, drawing, layers, text
 ****************************************************************************/

int32_t fpc_image_new(int32_t w, int32_t h)
{
  return fg_image_new(w, h);
}

void fpc_image_free(int32_t img)
{
  fg_image_free(img);
}

int32_t fpc_image_width(int32_t img)
{
  return fg_image_width(img);
}

int32_t fpc_image_height(int32_t img)
{
  return fg_image_height(img);
}

void fpc_image_put(int32_t img, int32_t x, int32_t y, int32_t w, int32_t h,
                   const uint16_t *px, int32_t stride)
{
  fg_image_put(img, x, y, w, h, px, (size_t)stride * 2);
}

void fpc_image_put8(int32_t img, int32_t x, int32_t y, int32_t w, int32_t h,
                    const uint8_t *px, int32_t stride, const uint16_t *pal,
                    int32_t npal)
{
  uint16_t p[256];

  memset(p, 0, sizeof(p));
  memcpy(p, pal, (size_t)(npal > 256 ? 256 : npal < 0 ? 0 : npal) * 2);
  fg_image_put8(img, x, y, w, h, px, (size_t)stride, p);
}

int32_t fpc_image_decode(const void *data, int32_t len)
{
  return -ENOSYS;                         /* (the runtime's wasm_image.c) */
}

int32_t fpc_image_load(const char *path, int32_t len)
{
  return -ENOSYS;
}

void fpc_clear(int32_t colour)
{
  fg_clear((uint16_t)colour);
}

void fpc_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t colour)
{
  fg_rect(x, y, w, h, (uint16_t)colour);
}

void fpc_sprite(int32_t img, int32_t sx, int32_t sy, int32_t w, int32_t h,
                int32_t x, int32_t y, int32_t flags)
{
  fg_sprite(img, sx, sy, w, h, x, y, (uint32_t)flags);
}

void fpc_sprite_rot(int32_t img, int32_t sx, int32_t sy, int32_t w,
                    int32_t h, int32_t cx, int32_t cy, int32_t flags,
                    int32_t angle, int32_t scale)
{
  fg_sprite_rot(img, sx, sy, w, h, cx, cy, (uint32_t)flags, angle & 4095,
                scale);
}

void fpc_sprite_matrix(int32_t img, int32_t sx, int32_t sy, int32_t w,
                       int32_t h, int32_t cx, int32_t cy, int32_t flags,
                       int32_t a, int32_t b, int32_t c, int32_t d)
{
  fg_sprite_matrix(img, sx, sy, w, h, cx, cy, (uint32_t)flags, a, b, c, d);
}

void fpc_quad(int32_t img, int32_t sx, int32_t sy, int32_t w, int32_t h,
              const int32_t *xy, int32_t flags)
{
  int32_t x[4] =
  {
    xy[0], xy[2], xy[4], xy[6]
  };

  int32_t y[4] =
  {
    xy[1], xy[3], xy[5], xy[7]
  };

  fg_quad(img, sx, sy, w, h, x, y, (uint32_t)flags);
}

int32_t fpc_quad3d(int32_t img, int32_t sx, int32_t sy, int32_t w,
                   int32_t h, int32_t cx, int32_t cy, int32_t flags,
                   int32_t yaw, int32_t pitch, int32_t roll, int32_t scale)
{
  return fg_quad3d(img, sx, sy, w, h, cx, cy, (uint32_t)flags, yaw & 4095,
                   pitch & 4095, roll & 4095, scale) ? 1 : 0;
}

void fpc_mode7(int32_t img, int32_t top, int32_t bottom, int32_t camx,
               int32_t camy, int32_t angle, int32_t height, int32_t horizon,
               int32_t focal, int32_t flags)
{
  fg_mode7(img, top, bottom, camx, camy, angle & 4095, height, horizon,
           focal, (uint32_t)flags);
}

int32_t fpc_layer_init(int32_t layer, int32_t tileset, int32_t tw,
                       int32_t th, int32_t mapw, int32_t maph)
{
  return fg_layer_init(layer, tileset, tw, th, mapw, maph);
}

void fpc_layer_free(int32_t layer)
{
  fg_layer_free(layer);
}

void fpc_layer_tileset(int32_t layer, int32_t tileset)
{
  fg_layer_tileset(layer, tileset);
}

void fpc_layer_set(int32_t layer, int32_t x, int32_t y, int32_t entry)
{
  fg_layer_set(layer, x, y, (uint16_t)entry);
}

int32_t fpc_layer_get(int32_t layer, int32_t x, int32_t y)
{
  return fg_layer_get(layer, x, y);
}

void fpc_layer_put(int32_t layer, int32_t x, int32_t y, int32_t w,
                   int32_t h, const uint16_t *entries, int32_t stride)
{
  int i;
  int j;

  for (j = 0; j < h; j++)
    {
      for (i = 0; i < w; i++)
        {
          fg_layer_set(layer, x + i, y + j, entries[(size_t)j * stride + i]);
        }
    }
}

void fpc_layer_redraw(int32_t layer)
{
  fg_layer_redraw(layer);
}

void fpc_layer_draw(int32_t layer, int32_t scrollx, int32_t scrolly)
{
  fg_layer_draw(layer, scrollx, scrolly);
}

void fpc_text_print(int32_t col, int32_t row, const char *text, int32_t len)
{
  char buf[FG_TEXT_COLS + 1];

  len = len > FG_TEXT_COLS ? FG_TEXT_COLS : len < 0 ? 0 : len;
  memcpy(buf, text, len);
  buf[len] = '\0';
  fg_text_print(col, row, "%s", buf);
}

void fpc_text_clear(void)
{
  fg_text_clear();
}

void fpc_text_color(int32_t colour)
{
  fg_text_color((uint16_t)colour);
}

void fpc_text_draw(void)
{
  fg_text_draw();
}

/****************************************************************************
 * Input, sound
 ****************************************************************************/

uint32_t fpc_pad(void)
{
  return fg_pad();
}

uint32_t fpc_pad_pressed(void)
{
  return fg_pad_pressed();
}

uint32_t fpc_pad_released(void)
{
  return fg_pad_released();
}

int32_t fpc_axis_x(void)
{
  return fg_axis_x();
}

int32_t fpc_axis_y(void)
{
  return fg_axis_y();
}

int32_t fpc_key_next(void)
{
  uint8_t flags;
  uint8_t code;
  uint8_t ch;

  while (fg_key_next(&flags, &code, &ch))
    {
      if (code < 0x80)
        {
          return (code >= 'A' && code <= 'Z' ? code + 0x20 : code) |
                 ((flags & NX_KEY_RELEASE) ? 0x100 : 0) |
                 (ch >= 0x20 && ch < 0x7f ? (int32_t)ch << 16 : 0);
        }
    }

  return -1;
}

int32_t fpc_mouse(int32_t *out, int32_t n)
{
  int v[4];
  int i;

  if (!fg_mouse(&v[0], &v[1], &v[2], &v[3]))
    {
      return 0;
    }

  for (i = 0; i < n && i < 4; i++)
    {
      out[i] = v[i];
    }

  return 1;
}

int32_t fpc_sample(int32_t id, const int8_t *data, int32_t samples,
                   int32_t loop_start, int32_t rate_hz)
{
  return fg_sample(id, data, (uint32_t)samples,
                   loop_start < 0 ? ~0u : (uint32_t)loop_start,
                   (uint32_t)rate_hz);
}

void fpc_sfx_play(int32_t voice, int32_t sample, uint32_t pitch16,
                  int32_t vol_l, int32_t vol_r)
{
  fg_sfx_play(voice, sample, pitch16, vol_l, vol_r);
}

void fpc_sfx_stop(int32_t voice)
{
  fg_sfx_stop(voice);
}

void fpc_patch(int32_t id, const uint8_t *p)
{
  struct fg_patch_s pt;
  int k;

  pt.alg      = p[0];
  pt.feedback = p[1];
  for (k = 0; k < 2; k++)
    {
      const uint8_t *o = p + 2 + 8 * k;

      pt.op[k].mult    = o[0];
      pt.op[k].detune  = (int8_t)o[1];
      pt.op[k].wave    = o[2];
      pt.op[k].level   = o[3];
      pt.op[k].attack  = o[4];
      pt.op[k].decay   = o[5];
      pt.op[k].sustain = o[6];
      pt.op[k].release = o[7];
    }

  fg_patch(id, &pt);
}

void fpc_fm_on(int32_t voice, int32_t patch, uint32_t freq16, int32_t vol_l,
               int32_t vol_r)
{
  fg_fm_on(voice, patch, freq16, vol_l, vol_r);
}

void fpc_fm_off(int32_t voice)
{
  fg_fm_off(voice);
}

void fpc_fm_freq(int32_t voice, uint32_t freq16)
{
  fg_fm_freq(voice, freq16);
}

uint32_t fpc_note_freq(int32_t note)
{
  return fg_note_freq(note);
}

void fpc_master_volume(int32_t vol_l, int32_t vol_r)
{
  fg_master_volume(vol_l, vol_r);
}

void fpc_voice_volume(int32_t voice, int32_t vol_l, int32_t vol_r)
{
  fg_voice_volume(voice, vol_l, vol_r);
}

void fpc_audio_stop_all(void)
{
  fg_audio_stop_all();
}

int32_t fpc_music_play(const void *notes, int32_t count,
                       int32_t length_frames, int32_t loop)
{
  static struct fg_song_s song;
  static struct fg_note_s *copy;
  const uint8_t *p = notes;
  int i;

  free(copy);
  copy = malloc(sizeof(*copy) * count);
  if (copy == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < count; i++, p += 8)
    {
      copy[i].frame = (uint16_t)(p[0] | p[1] << 8);
      copy[i].voice = p[2];
      copy[i].patch = p[3];
      copy[i].note  = p[4];
      copy[i].vel   = p[5];
      copy[i].len   = (uint16_t)(p[6] | p[7] << 8);
    }

  song.notes = copy;
  song.count = count;
  song.length_frames = (uint16_t)length_frames;
  song.loop = loop != 0;
  fg_music_play(&song);
  return 0;
}

void fpc_music_stop(void)
{
  fg_music_stop();
}

int32_t fpc_music_playing(void)
{
  return fg_music_playing() ? 1 : 0;
}
