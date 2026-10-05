/****************************************************************************
 * apps/graphics/fpcgame/fg_internal.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The game SDK's own: shared between its source files, not for games.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCGAME_FG_INTERNAL_H
#define __APPS_GRAPHICS_FPCGAME_FG_INTERNAL_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "fpcgame.h"

#define FG_IMAGES      64
#define FG_CELL_W      64                 /* The GPU's SPRITE cell */
#define FG_CELL_H      32

/* A rectangle of the GPU's space */

struct fg_rect_s
{
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

struct fg_s
{
  FAR const struct fg_game_s *game;
  int w;                                  /* The canvas */
  int h;
  int mode;                               /* The display: mode, scale */
  int scale;
  struct fg_rect_s canvas[2];             /* Each the whole display's view */
  int mx;                                 /* Where the canvas is in them */
  int my;
  int back;                               /* The one being drawn */
  int ox;                                 /* Its pixel (0, 0) in the space */
  int oy;
  bool quit;
  bool frozen;                            /* Pause or F12: the loop held */
  bool scripted;                          /* fg_script(): not paced (simulator) */
  FAR const char *script;                 /* fg_script(): what is left */
  uint32_t script_at;
  uint32_t frame;
  uint64_t t0;                            /* fg_run()'s start, us */
  uint32_t rnd;
  struct fg_stats_s st;
  uint32_t sprites;                       /* This frame's */
  uint32_t tiles;
};

extern struct fg_s g_fg;

uint64_t fg_now_us(void);

/* Images (fg_gfx.c) */

FAR const struct fg_rect_s *fg_image_rect(int img);   /* NULL: none */
void fg_gfx_reset(void);                  /* Everything freed (the game ended) */

/* The affine core: the cell (cw x ch at cx, cy of the space) through the
 * map u = u0 + a i + b j, v = v0 + c i + d j (8.8) over the box (bx, by,
 * bw, bh: canvas coordinates), clipped to the canvas; cells larger than
 * the GPU's are drawn in pieces
 */

void fg_affine(int cx, int cy, int cw, int ch, int bx, int by, int bw,
               int bh, int a, int b, int c, int d, int32_t u0, int32_t v0,
               uint32_t flags);

void fg_blit_wrap(FAR const struct fg_rect_s *surf, int scrollx, int scrolly);

/* fg_mode7() from a rectangle of the space (a layer's surface) */

void fg_mode7_rect(FAR const struct fg_rect_s *r, int top, int bottom,
                   int32_t camx, int32_t camy, int angle, int height,
                   int horizon, int focal, uint32_t flags);
uint16_t fg_rgb555(uint16_t c);           /* TRI's colours: never FG_KEY */

/* Input (fg_core.c), sound (fg_audio.c), layers and text (fg_gfx.c) */

void fg_input_frame(void);
int  fg_audio_start(void);
void fg_audio_stop(void);
void fg_audio_frame(void);                /* The sequencer: once a frame */

#endif /* __APPS_GRAPHICS_FPCGAME_FG_INTERNAL_H */
