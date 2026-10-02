/****************************************************************************
 * apps/graphics/fpcgame/fpcgame.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FPC game SDK: full-screen 2D games on the GPU (protocol v8).
 *
 * A game is a few callbacks (struct fg_game_s) and fg_run(), which takes
 * the display and the keyboard from the desktop, calls them sixty times a
 * second and gives everything back when the game ends.  The game draws on
 * a canvas of its own size (424 x 240 by default), which the GPU shows
 * scaled up to fill the screen (SET_SCALE: 3 x 3 at 1280 x 720); each frame
 * is drawn into the canvas that is not shown, then flipped to.
 *
 * Pictures live in the GPU's memory as images (RGB565; FG_KEY marks the
 * pixels that are transparent): from 16-bit pixels, or from 8-bit indexed
 * ones through a palette (index 0 transparent).  Sprites are rectangles of
 * images, plain, flipped, scaled or rotated (the GPU's SPRITE), translucent
 * or added on; rectangles are filled; tile layers show a map of tiles from
 * a tileset image, scrolled, wrapping round; the text layer shows the 8 x
 * 8 font in any colour.  Everything is drawn in call order and clipped to
 * the canvas.
 *
 * Input is a game pad made of the keyboard (arrows or WASD, Z X C V for
 * A B C D, Enter, Escape to leave).  Sound is sixteen voices, each a
 * sample or a two-operator FM instrument, and a sequencer for music
 * (TRI's sound engine), mixed into the desktop's mixer.
 *
 * Everything is drawn through the FPC GPU driver (drivers/gpu), so a game
 * runs the same in the NuttX simulator (boards/fpc-sim), on the GPU's
 * reference model.  docs/games.md tells the whole story.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCGAME_FPCGAME_H
#define __APPS_GRAPHICS_FPCGAME_FPCGAME_H

#include <nuttx/compiler.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * The game
 ****************************************************************************/

struct fg_game_s
{
  const char *name;
  int width;                      /* The canvas, pixels (0: 424 x 240) */
  int height;
  void (*load)(void);             /* Images, layers, sounds (once) */
  void (*init)(void);             /* Game state (once, after load) */
  void (*update)(void);           /* Logic, once a frame */
  void (*draw)(void);             /* Drawing, once a frame, after update */
  void (*quit)(void);             /* Before the display is given back (may be NULL) */
};

/* Run a game: 0 when it has ended (fg_exit(), or Escape held), a negated
 * errno if it could not start (another game has the display, no GPU of
 * protocol v8, no room in the GPU's memory)
 */

int fg_run(FAR const struct fg_game_s *game);
void fg_exit(void);                       /* End after this frame */

/* Input by frame numbers instead of the keyboard (tests, demos): before
 * fg_run(), a string of words: a number is a frame (counted from init()),
 * the words after it happen at that frame's start: +KEY pressed, -KEY
 * released, KEY pressed for one frame, "freeze" (the picture held, as
 * the Pause key does), "quit".  Keys: left right up down z x c v a s d w
 * j k l b space enter tab.  The keyboard still works (Escape, Pause).
 * In the simulator the frames are not paced meanwhile, so that a script
 * plays out quickly and the same every time.
 *   fg_script("30 z  60 +right +z  200 -right  260 freeze")
 */

void fg_script(FAR const char *script);

int fg_width(void);                       /* The canvas */
int fg_height(void);
uint32_t fg_frame(void);                  /* Frames since init() */
uint32_t fg_time_us(void);                /* Since fg_run() */
uint32_t fg_time_ms(void);

/* Random numbers: xorshift32, never 0; seeded with a fixed value */

uint32_t fg_random(void);
int fg_random_range(int lo, int hi);      /* lo..hi inclusive */
void fg_random_seed(uint32_t seed);

/****************************************************************************
 * Colours, images
 ****************************************************************************/

#define FG_RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define FG_KEY          0xf81f            /* Transparent (in images) */

/* An image: a rectangle of the GPU's memory, RGB565.  -ENOMEM when there
 * is no room (images last until fg_image_free() or the game ends)
 */

int  fg_image_new(int w, int h);
void fg_image_free(int img);
int  fg_image_width(int img);
int  fg_image_height(int img);

/* Pixels into an image: 16-bit (FG_KEY: transparent), or 8-bit through a
 * palette of RGB565 colours (index 0 transparent; a colour that happens to
 * be FG_KEY is moved off it)
 */

void fg_image_put(int img, int x, int y, int w, int h,
                  FAR const uint16_t *px, size_t stride);
void fg_image_put8(int img, int x, int y, int w, int h,
                   FAR const uint8_t *px, size_t stride,
                   FAR const uint16_t *pal565);
int  fg_image_from8(int w, int h, FAR const uint8_t *px, size_t stride,
                    FAR const uint16_t *pal565);   /* A new image, or < 0 */

/****************************************************************************
 * Drawing into this frame's canvas (in call order, clipped to it)
 ****************************************************************************/

#define FG_HFLIP     0x01                 /* Sprites: mirrored across */
#define FG_VFLIP     0x02                 /*   upside down */
#define FG_HALF      0x04                 /*   50 % over what is there */
#define FG_ADD       0x08                 /*   added on (saturating) */
#define FG_OPAQUE    0x10                 /*   FG_KEY pixels drawn too */
#define FG_ALPHA(a)  ((uint32_t)((a) & 0xff) << 8)   /* Translucent: 1..254 (0: opaque) */

void fg_clear(uint16_t colour);
void fg_rect(int x, int y, int w, int h, uint16_t colour);

/* The w x h rectangle at (sx, sy) of an image, its top-left at (x, y) */

void fg_sprite(int img, int sx, int sy, int w, int h, int x, int y,
               uint32_t flags);

/* ... rotated by angle (0..4095 a turn, clockwise) and scaled by scale
 * (8.8: 256 = 1) about (cx, cy), where its centre goes
 */

void fg_sprite_rot(int img, int sx, int sy, int w, int h, int cx, int cy,
                   uint32_t flags, int angle, int scale);

/* ... through any matrix: the inverse map, destination to source offsets,
 * [a b; c d] in 8.8, about (cx, cy) (TRI's TRI_OP_SPRITE_AFFINE exactly)
 */

void fg_sprite_matrix(int img, int sx, int sy, int w, int h, int cx, int cy,
                      uint32_t flags, int a, int b, int c, int d);

int  fg_sin(int angle);                   /* 8.8; angle 0..4095 a turn */
int  fg_cos(int angle);

/****************************************************************************
 * Tile layers
 *
 * A layer shows a map of mapw x maph tiles (each tw x th pixels, from a
 * tileset: an image holding them in rows, tile n at the n-th place,
 * row-major; tile 0 is empty and its place unused), scrolled by any amount
 * and wrapping round.  It keeps the map
 * drawn in the GPU's memory and redraws only the tiles that change, so
 * showing it costs a blit or four.  A map entry is FG_TILE(index, hflip,
 * vflip); FG_KEY pixels and empty tiles are see-through.
 ****************************************************************************/

#define FG_LAYERS       4
#define FG_TILE(i, h, v) ((uint16_t)(((i) & 0x0fff) | ((h) ? 0x1000 : 0) | ((v) ? 0x2000 : 0)))
#define FG_TILE_INDEX(e) ((e) & 0x0fff)

int  fg_layer_init(int layer, int tileset, int tw, int th, int mapw, int maph);
void fg_layer_free(int layer);
void fg_layer_tileset(int layer, int tileset);   /* Another (the same tiles' layout) */
void fg_layer_set(int layer, int x, int y, uint16_t entry);
uint16_t fg_layer_get(int layer, int x, int y);
void fg_layer_redraw(int layer);                 /* Every tile again (a tileset changed) */
void fg_layer_draw(int layer, int scrollx, int scrolly);   /* Into the canvas */

/****************************************************************************
 * Text: the 8 x 8 font, on a layer of 64 x 32 characters
 ****************************************************************************/

#define FG_TEXT_COLS    64
#define FG_TEXT_ROWS    32

void fg_text_print(int col, int row, FAR const char *fmt, ...)
     printf_like(3, 4);
void fg_text_center(int row, FAR const char *fmt, ...) printf_like(2, 3);
void fg_text_clear(void);
void fg_text_color(uint16_t colour);             /* All the text */
void fg_text_draw(void);                         /* Into the canvas */

/****************************************************************************
 * Input: a pad from the keyboard
 ****************************************************************************/

#define FG_PAD_UP       0x0001
#define FG_PAD_DOWN     0x0002
#define FG_PAD_LEFT     0x0004
#define FG_PAD_RIGHT    0x0008
#define FG_PAD_A        0x0010            /* Z, J */
#define FG_PAD_B        0x0020            /* X, K */
#define FG_PAD_C        0x0040            /* C, L */
#define FG_PAD_D        0x0080            /* V, Space */
#define FG_PAD_STICK    0x0100            /* B (the stick's click) */
#define FG_PAD_START    0x0200            /* Enter */
#define FG_PAD_SELECT   0x0400            /* Tab */

uint32_t fg_pad(void);                    /* Held this frame */
uint32_t fg_pad_pressed(void);            /* Went down this frame */
uint32_t fg_pad_released(void);           /* Went up this frame */
int fg_axis_x(void);                      /* -107 (left) .. 107 (right) */
int fg_axis_y(void);                      /* -107 (down) .. 107 (up) */

/****************************************************************************
 * Sound: sixteen voices, each a sample (8-bit signed, any rate) or a
 * two-operator FM instrument, at 48 kHz into the mixer
 ****************************************************************************/

#define FG_VOICES       16
#define FG_SAMPLES      64
#define FG_PATCHES      32

struct fg_patch_s                         /* As TRI's tri_fm_patch_t */
{
  uint8_t alg;                            /* 0: op 0 modulates op 1; 1: additive */
  uint8_t feedback;                       /* 0..7 */
  struct
  {
    uint8_t mult;                         /* 0..15 (0: x 0.5) */
    int8_t  detune;                       /* -8..7 */
    uint8_t wave;                         /* 0 sine, 1 half, 2 abs, 3 pulse */
    uint8_t level;                        /* 0..255 */
    uint8_t attack, decay, sustain, release;
  } op[2];
};

struct fg_note_s                          /* As TRI's tri_note_t */
{
  uint16_t frame;
  uint8_t  voice;
  uint8_t  patch;
  uint8_t  note;                          /* MIDI */
  uint8_t  vel;
  uint16_t len;                           /* Frames until note off, 0: never */
};

struct fg_song_s
{
  FAR const struct fg_note_s *notes;      /* In frame order */
  int count;
  uint16_t length_frames;
  bool loop;
};

int  fg_sample(int id, FAR const int8_t *data, uint32_t samples,
               uint32_t loop_start, uint32_t rate_hz);   /* loop_start ~0: once */
void fg_sfx_play(int voice, int sample, uint32_t pitch16, int vol_l, int vol_r);
void fg_sfx_stop(int voice);
void fg_patch(int id, FAR const struct fg_patch_s *patch);
void fg_fm_on(int voice, int patch, uint32_t freq16, int vol_l, int vol_r);
void fg_fm_off(int voice);
void fg_fm_freq(int voice, uint32_t freq16);
uint32_t fg_note_freq(int midi_note);     /* Hz, 16.16 */
void fg_master_volume(int vol_l, int vol_r);
void fg_voice_volume(int voice, int vol_l, int vol_r);
void fg_audio_stop_all(void);
void fg_music_play(FAR const struct fg_song_s *song);
void fg_music_stop(void);
bool fg_music_playing(void);

/* The raw sound commands (TRI's TRI_OP_AUDIO_* words: tri_isa.h), for a
 * layer that speaks them (the TRI games' runtime)
 */

void fg_audio_cmd(uint32_t op, uint32_t imm, FAR const uint32_t *p, uint32_t nw);
void fg_audio_vram(FAR const uint8_t *(*base)(uint32_t off),
                   bool (*range)(uint32_t off, uint32_t len));

/****************************************************************************
 * Statistics (the game's own: frame times, GPU work)
 ****************************************************************************/

struct fg_stats_s
{
  uint32_t frames;
  uint32_t frame_us;                      /* update + draw + sending, last frame */
  uint32_t frame_us_max;                  /* ... worst in the last second */
  uint32_t wait_us;                       /* Waiting for the GPU's frame, last frame */
  uint32_t late;                          /* Frames that missed their vsync */
  uint32_t sprites;                       /* Last frame */
  uint32_t tiles;                         /* Tiles redrawn, last frame */
};

FAR const struct fg_stats_s *fg_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_GRAPHICS_FPCGAME_FPCGAME_H */
