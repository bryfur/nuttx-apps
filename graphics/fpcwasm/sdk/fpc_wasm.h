/****************************************************************************
 * apps/graphics/fpcwasm/sdk/fpc_wasm.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FPC's WebAssembly app API, version 1 (docs/wasm.md).
 *
 * A WebAssembly app (wasm32, WASI preview 1) runs on the FPC's `wasm`
 * runtime (WAMR: interpreted, or ahead-of-time compiled for the board's
 * Cortex-M33).  Besides WASI (stdio, files of its own directory, clocks,
 * random numbers, arguments), it has the functions below, imported from
 * the module "env", each named fpc_*:
 *
 *   - a full-screen game: fpc_game_run() takes the display and the
 *     keyboard from the desktop and calls the app's exported functions
 *     fpc_load(), fpc_init(), fpc_update() and fpc_draw() sixty times a
 *     second, until the game ends (Escape, or fpc_game_exit());
 *   - images in the GPU's memory, sprites (plain, flipped, rotated and
 *     scaled, translucent, added), rectangles, quadrilaterals in
 *     perspective, Mode 7 floors, tile layers and a text layer, drawn into
 *     a canvas of the game's size that the GPU shows scaled up;
 *   - a game pad made of the keyboard (and a real pad if there is one),
 *     key events, the mouse;
 *   - sixteen voices of sound (samples and two-operator FM) and a
 *     sequencer for music;
 *   - time, random numbers, a line in the system log.
 *
 * The ABI, which stays as it is: every parameter and every result is a
 * 32-bit integer (wasm i32).  A pointer is an offset into the app's linear
 * memory; the runtime checks every buffer it is given against the linear
 * memory and ends the app (a trap) if one is not inside it.  Handles
 * (images, layers, voices) are small integers; a bad handle or an
 * argument out of range is ignored (or answered with a negative errno),
 * never a trap.  Nothing the runtime keeps refers to the app's memory: a
 * sample or a song is copied.  New versions only add functions;
 * fpc_api_version() says which the runtime has, and a runtime refuses to
 * start an app that imports a function it does not have (it names it).
 *
 * This header is plain C99 (any C compiler that targets wasm32 can use
 * it): FPC_IMPORT and FPC_EXPORT are clang's import_module/export_name
 * attributes, and empty elsewhere (a compiler without them imports
 * undefined functions from "env" and exports by linker options).
 *
 * Building (wasi-sdk; docs/wasm.md, sdk/Makefile):
 *   clang --target=wasm32-wasip1 -O2 -o app.wasm app.c \
 *         -Wl,-z,stack-size=32768 -Wl,--strip-all
 *   wamrc --target=thumbv8m.main --target-abi=gnueabihf --cpu=cortex-m33 \
 *         --disable-simd --bounds-checks=1 --stack-bounds-checks=1 \
 *         --enable-multi-thread -o app.aot app.wasm
 ****************************************************************************/

#ifndef FPC_WASM_H
#define FPC_WASM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define FPC_API_VERSION 1

#if defined(__clang__) && defined(__wasm__)
#  define FPC_IMPORT(name) \
     __attribute__((import_module("env"), import_name(#name)))
#  define FPC_EXPORT(name) __attribute__((export_name(#name)))
#else
#  define FPC_IMPORT(name)
#  define FPC_EXPORT(name)
#endif

/****************************************************************************
 * The system
 ****************************************************************************/

/* The API version the runtime has (FPC_API_VERSION or later) */

FPC_IMPORT(fpc_api_version) int32_t fpc_api_version(void);

/* A line in the system log (syslog), tagged with the app's name; len
 * bytes of text (no terminator needed; at most 200 are kept)
 */

FPC_IMPORT(fpc_log) void fpc_log(const char *text, int32_t len);

/* Microseconds and milliseconds since the app started (they wrap) */

FPC_IMPORT(fpc_time_us) uint32_t fpc_time_us(void);
FPC_IMPORT(fpc_time_ms) uint32_t fpc_time_ms(void);

/* Sleep (a CLI app; a game is paced by its frames) */

FPC_IMPORT(fpc_sleep_ms) void fpc_sleep_ms(int32_t ms);

/* Random numbers: xorshift32, never 0, the same sequence every run from a
 * fixed seed (fpc_random_seed() for another); lo..hi inclusive
 */

FPC_IMPORT(fpc_random) uint32_t fpc_random(void);
FPC_IMPORT(fpc_random_range) int32_t fpc_random_range(int32_t lo,
                                                      int32_t hi);
FPC_IMPORT(fpc_random_seed) void fpc_random_seed(uint32_t seed);

/* Sine and cosine, 8.8 (256 = 1); the angle 0..4095 a turn */

FPC_IMPORT(fpc_sin) int32_t fpc_sin(int32_t angle);
FPC_IMPORT(fpc_cos) int32_t fpc_cos(int32_t angle);

/****************************************************************************
 * The game
 *
 * fpc_game_run(width, height) takes the display and the keyboard (0, 0:
 * a canvas of 424 x 240, which fills 1280 x 720 at 3 x 3) and calls the
 * functions the app exports, each without parameters or result:
 *
 *   fpc_load()     once: images, layers, sounds          (optional)
 *   fpc_init()     once, after fpc_load: the game's state (optional)
 *   fpc_update()   every frame: the logic                 (optional)
 *   fpc_draw()     every frame, after fpc_update          (required)
 *   fpc_quit()     once, before the display is given back (optional)
 *
 * It returns when the game has ended: 0, or a negative errno if it could
 * not start (-EBUSY: another game has the display; -ENOMEM: no room in
 * the GPU's memory; -ENOENT: no fpc_draw export).  If the app traps in
 * one of them, the display and the keyboard are given back and the app
 * ends.  Images, layers and sounds exist only while the game runs, and
 * the functions that draw, read input or make sounds do nothing outside
 * it.  Each frame is drawn into the canvas not shown, then flipped to.
 ****************************************************************************/

FPC_IMPORT(fpc_game_run) int32_t fpc_game_run(int32_t width, int32_t height);
FPC_IMPORT(fpc_game_exit) void fpc_game_exit(void);   /* After this frame */

FPC_IMPORT(fpc_game_width) int32_t fpc_game_width(void);    /* The canvas */
FPC_IMPORT(fpc_game_height) int32_t fpc_game_height(void);
FPC_IMPORT(fpc_game_frame) uint32_t fpc_game_frame(void);   /* Since init */

/* The display the game has: out[0], out[1] its size, out[2] the scale
 * (each canvas pixel is shown out[2] x out[2]), out[3] the display mode
 * (0 1024 x 768, 1 800 x 600, 2 1280 x 720, 3 1280 x 800).  Fills up to n;
 * returns how many it has (4), 0 outside a game
 */

FPC_IMPORT(fpc_game_display) int32_t fpc_game_display(int32_t *out,
                                                      int32_t n);

/* The game's statistics, out[0..n-1] of: FPC_STAT_* (returns the count) */

#define FPC_STAT_FRAMES       0           /* Frames drawn */
#define FPC_STAT_FRAME_US     1           /* update + draw + sending, last frame */
#define FPC_STAT_FRAME_US_MAX 2           /* ... worst in the last second */
#define FPC_STAT_WAIT_US      3           /* Waiting for the GPU's frame */
#define FPC_STAT_LATE         4           /* Frames that missed their vsync */
#define FPC_STAT_SPRITES      5           /* Sprites, last frame */
#define FPC_STAT_TILES        6           /* Tiles redrawn, last frame */
#define FPC_STAT_COUNT        7

FPC_IMPORT(fpc_game_stats) int32_t fpc_game_stats(uint32_t *out, int32_t n);

/****************************************************************************
 * Colours, images
 ****************************************************************************/

#define FPC_RGB(r, g, b) \
  ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define FPC_KEY          0xf81f           /* Transparent (in images) */

/* A new image, w x h RGB565 pixels in the GPU's memory: its handle, or a
 * negative errno (-ENOMEM: no room).  At most 64 at a time
 */

FPC_IMPORT(fpc_image_new) int32_t fpc_image_new(int32_t w, int32_t h);
FPC_IMPORT(fpc_image_free) void fpc_image_free(int32_t img);
FPC_IMPORT(fpc_image_width) int32_t fpc_image_width(int32_t img);
FPC_IMPORT(fpc_image_height) int32_t fpc_image_height(int32_t img);

/* Pixels into a rectangle of an image: RGB565 (FPC_KEY transparent),
 * stride in pixels; or 8-bit indices through a palette of npal RGB565
 * colours (index 0 transparent, indices past npal black), stride in bytes
 */

FPC_IMPORT(fpc_image_put) void fpc_image_put(int32_t img, int32_t x,
                                             int32_t y, int32_t w, int32_t h,
                                             const uint16_t *px,
                                             int32_t stride);
FPC_IMPORT(fpc_image_put8) void fpc_image_put8(int32_t img, int32_t x,
                                               int32_t y, int32_t w,
                                               int32_t h, const uint8_t *px,
                                               int32_t stride,
                                               const uint16_t *pal,
                                               int32_t npal);

/* A new image from an image file's bytes (BMP: 8, 24 or 32 bits,
 * uncompressed; QOI): pixels whose alpha is below 128 become FPC_KEY.
 * fpc_image_load() reads the file from the app's own directory (a
 * relative path, no ".."). The image's handle, or a negative errno
 */

FPC_IMPORT(fpc_image_decode) int32_t fpc_image_decode(const void *data,
                                                      int32_t len);
FPC_IMPORT(fpc_image_load) int32_t fpc_image_load(const char *path,
                                                  int32_t len);

/****************************************************************************
 * Drawing into this frame's canvas (in call order, clipped to it)
 ****************************************************************************/

#define FPC_HFLIP     0x01                /* Mirrored across */
#define FPC_VFLIP     0x02                /* Upside down */
#define FPC_HALF      0x04                /* 50 % over what is there */
#define FPC_ADD       0x08                /* Added on (saturating) */
#define FPC_OPAQUE    0x10                /* FPC_KEY pixels drawn too */
#define FPC_WRAP      0x20                /* fpc_mode7(): the image repeats */
#define FPC_ALPHA(a)  ((int32_t)(((a) & 0xff) << 8))   /* Translucent: 1..254 */

FPC_IMPORT(fpc_clear) void fpc_clear(int32_t colour);
FPC_IMPORT(fpc_rect) void fpc_rect(int32_t x, int32_t y, int32_t w,
                                   int32_t h, int32_t colour);

/* The w x h rectangle at (sx, sy) of an image, its top-left at (x, y) */

FPC_IMPORT(fpc_sprite) void fpc_sprite(int32_t img, int32_t sx, int32_t sy,
                                       int32_t w, int32_t h, int32_t x,
                                       int32_t y, int32_t flags);

/* ... rotated by angle (0..4095 a turn, clockwise) and scaled by scale
 * (8.8: 256 = 1) about its centre, which goes to (cx, cy)
 */

FPC_IMPORT(fpc_sprite_rot) void fpc_sprite_rot(int32_t img, int32_t sx,
                                               int32_t sy, int32_t w,
                                               int32_t h, int32_t cx,
                                               int32_t cy, int32_t flags,
                                               int32_t angle, int32_t scale);

/* ... through any matrix: the inverse map, destination to source, [a b;
 * c d] in 8.8, about (cx, cy)
 */

FPC_IMPORT(fpc_sprite_matrix) void fpc_sprite_matrix(int32_t img, int32_t sx,
                                                     int32_t sy, int32_t w,
                                                     int32_t h, int32_t cx,
                                                     int32_t cy, int32_t flags,
                                                     int32_t a, int32_t b,
                                                     int32_t c, int32_t d);

/* ... as any convex quadrilateral in perspective: xy[0..7] are its
 * corners' x0, y0, x1, y1, x2, y2, x3, y3 (top-left, top-right,
 * bottom-right, bottom-left) in the canvas, 12.4 (1/16 pixel).  Flags:
 * FPC_OPAQUE, FPC_HFLIP, FPC_VFLIP
 */

FPC_IMPORT(fpc_quad) void fpc_quad(int32_t img, int32_t sx, int32_t sy,
                                   int32_t w, int32_t h, const int32_t *xy,
                                   int32_t flags);

/* ... turned in 3D about its centre at (cx, cy): roll, pitch, yaw (each
 * 0..4095 a turn), scaled (8.8), seen from 256 pixels.  1 if drawn, 0 if
 * its back faces the viewer
 */

FPC_IMPORT(fpc_quad3d) int32_t fpc_quad3d(int32_t img, int32_t sx, int32_t sy,
                                          int32_t w, int32_t h, int32_t cx,
                                          int32_t cy, int32_t flags,
                                          int32_t yaw, int32_t pitch,
                                          int32_t roll, int32_t scale);

/* A floor in perspective (Mode 7) on canvas rows top..bottom - 1 below the
 * horizon row: the image's plane seen from (camx, camy) of it (8.8
 * pixels), facing angle, from height pixels above it, a lens of focal
 * pixels.  Flags: FPC_OPAQUE, FPC_WRAP
 */

FPC_IMPORT(fpc_mode7) void fpc_mode7(int32_t img, int32_t top, int32_t bottom,
                                     int32_t camx, int32_t camy,
                                     int32_t angle, int32_t height,
                                     int32_t horizon, int32_t focal,
                                     int32_t flags);

/****************************************************************************
 * Tile layers (4): a map of mapw x maph tiles, each tw x th pixels (at
 * most 64 x 32) from a tileset image (tile n at the n-th place, row-major;
 * tile 0 empty), scrolled and wrapping round; only changed tiles are
 * redrawn.  An entry is FPC_TILE(index, hflip, vflip)
 ****************************************************************************/

#define FPC_LAYERS          4
#define FPC_TILE(i, h, v) \
  ((uint16_t)(((i) & 0x0fff) | ((h) ? 0x1000 : 0) | ((v) ? 0x2000 : 0)))

FPC_IMPORT(fpc_layer_init) int32_t fpc_layer_init(int32_t layer,
                                                  int32_t tileset,
                                                  int32_t tw, int32_t th,
                                                  int32_t mapw, int32_t maph);
FPC_IMPORT(fpc_layer_free) void fpc_layer_free(int32_t layer);
FPC_IMPORT(fpc_layer_tileset) void fpc_layer_tileset(int32_t layer,
                                                     int32_t tileset);
FPC_IMPORT(fpc_layer_set) void fpc_layer_set(int32_t layer, int32_t x,
                                             int32_t y, int32_t entry);
FPC_IMPORT(fpc_layer_get) int32_t fpc_layer_get(int32_t layer, int32_t x,
                                                int32_t y);

/* A w x h rectangle of the map at (x, y) at once: entries row by row,
 * stride entries apart
 */

FPC_IMPORT(fpc_layer_put) void fpc_layer_put(int32_t layer, int32_t x,
                                             int32_t y, int32_t w, int32_t h,
                                             const uint16_t *entries,
                                             int32_t stride);
FPC_IMPORT(fpc_layer_redraw) void fpc_layer_redraw(int32_t layer);
FPC_IMPORT(fpc_layer_draw) void fpc_layer_draw(int32_t layer,
                                               int32_t scrollx,
                                               int32_t scrolly);

/****************************************************************************
 * Text: the 8 x 8 font on a layer of 64 x 32 characters
 ****************************************************************************/

#define FPC_TEXT_COLS 64
#define FPC_TEXT_ROWS 32

FPC_IMPORT(fpc_text_print) void fpc_text_print(int32_t col, int32_t row,
                                               const char *text,
                                               int32_t len);
FPC_IMPORT(fpc_text_clear) void fpc_text_clear(void);
FPC_IMPORT(fpc_text_color) void fpc_text_color(int32_t colour);
FPC_IMPORT(fpc_text_draw) void fpc_text_draw(void);

/****************************************************************************
 * Input
 ****************************************************************************/

/* The pad: arrows or WASD, Z/J = A, X/K = B, C/L = C, V/Space = D, B = the
 * stick's click, Enter = START, Tab = SELECT (Escape ends the game)
 */

#define FPC_PAD_UP      0x0001
#define FPC_PAD_DOWN    0x0002
#define FPC_PAD_LEFT    0x0004
#define FPC_PAD_RIGHT   0x0008
#define FPC_PAD_A       0x0010
#define FPC_PAD_B       0x0020
#define FPC_PAD_C       0x0040
#define FPC_PAD_D       0x0080
#define FPC_PAD_STICK   0x0100
#define FPC_PAD_START   0x0200
#define FPC_PAD_SELECT  0x0400

FPC_IMPORT(fpc_pad) uint32_t fpc_pad(void);            /* Held */
FPC_IMPORT(fpc_pad_pressed) uint32_t fpc_pad_pressed(void);    /* Went down */
FPC_IMPORT(fpc_pad_released) uint32_t fpc_pad_released(void);  /* Went up */
FPC_IMPORT(fpc_axis_x) int32_t fpc_axis_x(void);       /* -107 .. 107 */
FPC_IMPORT(fpc_axis_y) int32_t fpc_axis_y(void);       /* -107 (down) .. 107 */

/* Key events, as they came (presses and releases, no repeats): -1 when
 * there are none left, else FPC_KEY_CODE(e) the key (ASCII, lower case
 * for letters, or FPC_KEY_LEFT ...), FPC_KEY_UP_EVENT(e) set for a
 * release, FPC_KEY_CHAR(e) the character it typed (0: none)
 */

#define FPC_KEY_CODE(e)      ((e) & 0xff)
#define FPC_KEY_UP_EVENT(e)  (((e) >> 8) & 1)
#define FPC_KEY_CHAR(e)      (((e) >> 16) & 0xff)

#define FPC_KEY_BACKSPACE    0x08
#define FPC_KEY_TAB          0x09
#define FPC_KEY_ENTER        0x0d
#define FPC_KEY_ESCAPE       0x1b
#define FPC_KEY_LEFT         0x80
#define FPC_KEY_RIGHT        0x81
#define FPC_KEY_UP           0x82
#define FPC_KEY_DOWN         0x83
#define FPC_KEY_HOME         0x84
#define FPC_KEY_END          0x85
#define FPC_KEY_PAGEUP       0x86
#define FPC_KEY_PAGEDOWN     0x87
#define FPC_KEY_INSERT       0x88
#define FPC_KEY_DELETE       0x89
#define FPC_KEY_F1           0x90         /* ... FPC_KEY_F1 + 11: F12 */

FPC_IMPORT(fpc_key_next) int32_t fpc_key_next(void);

/* The mouse, in canvas pixels: out[0], out[1] the pointer (kept inside
 * the canvas), out[2] the buttons (FPC_MOUSE_*), out[3] the wheel's
 * movement since the last call.  Fills up to n; returns 1 if there is a
 * mouse, 0 if not (or outside a game)
 */

#define FPC_MOUSE_LEFT   0x01
#define FPC_MOUSE_RIGHT  0x02
#define FPC_MOUSE_MIDDLE 0x04

FPC_IMPORT(fpc_mouse) int32_t fpc_mouse(int32_t *out, int32_t n);

/****************************************************************************
 * Sound: sixteen voices, each a sample (8-bit signed, any rate) or a
 * two-operator FM instrument, 48 kHz into the desktop's mixer; volumes
 * 0..255; pitch and frequency 16.16 (pitch 0x10000: the sample's rate)
 ****************************************************************************/

#define FPC_VOICES   16
#define FPC_SAMPLES  64
#define FPC_PATCHES  32

/* Sample id (copied): 0, or a negative errno.  loop_start -1: played once */

FPC_IMPORT(fpc_sample) int32_t fpc_sample(int32_t id, const int8_t *data,
                                          int32_t samples,
                                          int32_t loop_start,
                                          int32_t rate_hz);
FPC_IMPORT(fpc_sfx_play) void fpc_sfx_play(int32_t voice, int32_t sample,
                                           uint32_t pitch16, int32_t vol_l,
                                           int32_t vol_r);
FPC_IMPORT(fpc_sfx_stop) void fpc_sfx_stop(int32_t voice);

/* An FM instrument: 18 bytes, alg (0: op 0 modulates op 1; 1: both
 * heard), feedback (0..7), then for op 0 and op 1: mult (0..15), detune
 * (signed, -8..7), wave (0 sine, 1 half, 2 abs, 3 pulse), level, attack,
 * decay, sustain, release
 */

#define FPC_PATCH_SIZE 18

FPC_IMPORT(fpc_patch) void fpc_patch(int32_t id, const uint8_t *patch);
FPC_IMPORT(fpc_fm_on) void fpc_fm_on(int32_t voice, int32_t patch,
                                     uint32_t freq16, int32_t vol_l,
                                     int32_t vol_r);
FPC_IMPORT(fpc_fm_off) void fpc_fm_off(int32_t voice);
FPC_IMPORT(fpc_fm_freq) void fpc_fm_freq(int32_t voice, uint32_t freq16);
FPC_IMPORT(fpc_note_freq) uint32_t fpc_note_freq(int32_t midi_note);
FPC_IMPORT(fpc_master_volume) void fpc_master_volume(int32_t vol_l,
                                                     int32_t vol_r);
FPC_IMPORT(fpc_voice_volume) void fpc_voice_volume(int32_t voice,
                                                   int32_t vol_l,
                                                   int32_t vol_r);
FPC_IMPORT(fpc_audio_stop_all) void fpc_audio_stop_all(void);

/* Music (copied): count notes of 8 bytes each, in frame order: frame
 * (uint16, little-endian), voice, patch, MIDI note, velocity, length in
 * frames (uint16; 0: held); the song is length_frames long, looped if
 * loop.  0, or a negative errno
 */

#define FPC_NOTE_SIZE 8

FPC_IMPORT(fpc_music_play) int32_t fpc_music_play(const void *notes,
                                                  int32_t count,
                                                  int32_t length_frames,
                                                  int32_t loop);
FPC_IMPORT(fpc_music_stop) void fpc_music_stop(void);
FPC_IMPORT(fpc_music_playing) int32_t fpc_music_playing(void);

#ifdef __cplusplus
}
#endif

#endif /* FPC_WASM_H */
