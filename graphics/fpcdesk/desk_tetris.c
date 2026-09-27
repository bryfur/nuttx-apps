/****************************************************************************
 * apps/graphics/fpcdesk/desk_tetris.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tetris: the classic rules.  The seven pieces come in shuffled bags of
 * seven; they rotate as in SRS, with simple wall kicks; a piece that
 * lands can still move for half a second.  Lines clear with a flash,
 * score 100, 300, 500 or 800 times the level (a soft drop 1 a row, a hard
 * drop 2), and every ten lines the level rises and the pieces fall faster.
 * The next piece is shown, one can be held, and a ghost shows where the
 * piece will land.
 *
 * Keys: Left / Right move, Up or x rotates (z the other way), Down drops
 * softly, Space drops hard, c holds, p or Escape pauses, Enter or F2
 * starts a new game.  The game pauses when its window is not the active
 * one.
 *
 * Drawing: the tiles (one for each colour, their ghosts, the empty cell)
 * are drawn once with GPU operations into a hidden surface; a cell is
 * then one COPY from it.  The window remembers what each cell shows and
 * copies only the cells that change: a piece falling a row is eight
 * COPYs.  The game runs on the desktop's frames (w->frames) while it is
 * being played, and asks for none when paused or over.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "desk_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TET_COLS        10
#define TET_ROWS        20
#define TET_HIDDEN      2               /* Rows above the top, for spawning */
#define TET_ALL         (TET_ROWS + TET_HIDDEN)
#define TET_CELL_MAX    28
#define TET_CELL_MIN    12
#define TET_MARGIN      16
#define TET_PANEL_W     150
#define TET_W           (2 * TET_MARGIN + TET_COLS * 26 + 20 + TET_PANEL_W)
#define TET_H           (2 * TET_MARGIN + TET_ROWS * 26)

#define TET_LOCK_MS     500             /* A landed piece can still move */
#define TET_LOCK_MOVES  15              /* ... this many times */
#define TET_CLEAR_MS    360             /* The flash of full lines */
#define TET_SOFT_MS     40              /* Soft drop: a row this often */

/* Tiles in the surface: the empty cell, 7 colours, 7 ghosts */

#define TILE_EMPTY      0
#define TILE_GHOST      8               /* + piece */
#define TILE_N          15

#define C_TET_BG        RGB(0x14, 0x14, 0x19)
#define C_TET_WELL      RGB(0x1b, 0x1b, 0x22)
#define C_TET_FRAME     RGB(0x30, 0x30, 0x38)

/* Game states */

#define ST_PLAY         0
#define ST_CLEAR        1               /* Lines flashing */
#define ST_PAUSED       2
#define ST_OVER         3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct tetris_s
{
  /* The game */

  uint8_t board[TET_ALL][TET_COLS];     /* 0 empty, 1-7 a piece's colour */
  uint8_t bag[7];
  int nbag;
  int piece;                            /* 0-6 */
  int rot;
  int px;                               /* The piece's box, in cells */
  int py;
  int next;
  int hold;                             /* -1: none */
  bool held;                            /* Held since this piece came */
  int state;                            /* ST_* */
  int prevstate;                        /* Before a pause */
  bool soft;                            /* Down held: falling fast */
  uint32_t softms;                      /* The last Down */
  int fall;                             /* ms towards the next row */
  int lockms;                           /* ms on the ground */
  int lockmoves;
  uint32_t last;                        /* The last frame's time */
  uint32_t clearstart;
  uint8_t clearrows[4];
  int nclear;
  unsigned int score;
  unsigned int lines;
  unsigned int level;
  uint32_t seed;

  /* The layout */

  int cw;                               /* Client size laid out for */
  int ch;
  int cell;
  int bx;                               /* The board, frame coordinates */
  int by;
  int panx;                             /* The panel */

  /* What is drawn: each cell's tile and flash, the panel's figures */

  uint8_t drawn[TET_ROWS][TET_COLS];
  uint8_t drawnflash[TET_ROWS];
  bool overlay;                         /* The pause / game over panel */
  int drawnnext;
  int drawnhold;
  unsigned int drawnscore;
  unsigned int drawnlines;
  unsigned int drawnlevel;
  bool valid;                           /* Anything drawn at all */

  struct ui_keys_s keys;
  struct desk_ops_s o;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The pieces I O T S Z J L in their spawn orientation: the size of their
 * box, and their four cells in it (x, y)
 */

static const uint8_t g_tet_box[7] =
{
  4, 2, 3, 3, 3, 3, 3
};

static const int8_t g_tet_shape[7][8] =
{
  {
    0, 1, 1, 1, 2, 1, 3, 1
  },
  {
    0, 0, 1, 0, 0, 1, 1, 1
  },
  {
    1, 0, 0, 1, 1, 1, 2, 1
  },
  {
    1, 0, 2, 0, 0, 1, 1, 1
  },
  {
    0, 0, 1, 0, 1, 1, 2, 1
  },
  {
    0, 0, 0, 1, 1, 1, 2, 1
  },
  {
    2, 0, 0, 1, 1, 1, 2, 1
  }
};

static const uint16_t g_tet_colour[7] =
{
  RGB(0x1e, 0xc8, 0xf0),                /* I cyan */
  RGB(0xf5, 0xc8, 0x14),                /* O yellow */
  RGB(0xa8, 0x48, 0xe8),                /* T purple */
  RGB(0x48, 0xd0, 0x48),                /* S green */
  RGB(0xf0, 0x44, 0x48),                /* Z red */
  RGB(0x30, 0x74, 0xf4),                /* J blue */
  RGB(0xf8, 0x94, 0x20),                /* L orange */
};

/* Wall kicks: tried in turn (x right, y down) */

static const int8_t g_tet_kickx[8] =
{
  0, -1, 1, 0, -1, 1, -2, 2
};

static const int8_t g_tet_kicky[8] =
{
  0, 0, 0, -1, -1, -1, 0, 0
};

/* ms a row, by level (the guideline's gravity) */

static const uint16_t g_tet_speed[] =
{
  1000, 793, 618, 473, 355, 262, 190, 135, 94, 64, 43, 28, 18, 11, 7
};

static NXWINDOW g_tet_tiles;            /* The surface of tiles */
static int g_tet_tilecell;              /* The cell size they are drawn at */
static volatile bool g_tet_lost;        /* Its pixels are gone */

/****************************************************************************
 * Private Functions: the game
 ****************************************************************************/

static uint32_t tet_rand(struct tetris_s *t)
{
  t->seed = t->seed * 1103515245 + 12345;
  return t->seed >> 16;
}

static int tet_bag_next(struct tetris_s *t)
{
  int i;

  if (t->nbag == 0)
    {
      for (i = 0; i < 7; i++)
        {
          t->bag[i] = i;
        }

      for (i = 6; i > 0; i--)
        {
          int j = tet_rand(t) % (i + 1);
          uint8_t s = t->bag[i];

          t->bag[i] = t->bag[j];
          t->bag[j] = s;
        }

      t->nbag = 7;
    }

  return t->bag[--t->nbag];
}

/* The cells of piece p in rotation r (clockwise quarter turns) */

static void tet_cells(int p, int r, int cells[4][2])
{
  int n = g_tet_box[p];
  int i;

  for (i = 0; i < 4; i++)
    {
      int x = g_tet_shape[p][2 * i];
      int y = g_tet_shape[p][2 * i + 1];
      int k;

      for (k = 0; k < (r & 3); k++)
        {
          int nx = n - 1 - y;

          y = x;
          x = nx;
        }

      cells[i][0] = x;
      cells[i][1] = y;
    }
}

static bool tet_fits(struct tetris_s *t, int p, int r, int x, int y)
{
  int cells[4][2];
  int i;

  tet_cells(p, r, cells);
  for (i = 0; i < 4; i++)
    {
      int cx = x + cells[i][0];
      int cy = y + cells[i][1];

      if (cx < 0 || cx >= TET_COLS || cy < 0 || cy >= TET_ALL ||
          t->board[cy][cx] != 0)
        {
          return false;
        }
    }

  return true;
}

static void tet_spawn(struct tetris_s *t, int p)
{
  t->piece     = p;
  t->rot       = 0;
  t->px        = (TET_COLS - g_tet_box[p]) / 2;
  t->py        = p == 0 ? 0 : 1;
  t->fall      = 0;
  t->lockms    = 0;
  t->lockmoves = 0;
  t->held      = false;
  t->soft      = false;

  if (!tet_fits(t, p, 0, t->px, t->py))
    {
      t->state = ST_OVER;
    }
}

static void tet_new(struct tetris_s *t)
{
  memset(t->board, 0, sizeof(t->board));
  t->nbag  = 0;
  t->seed ^= desk_ms();
  t->score = 0;
  t->lines = 0;
  t->level = 1;
  t->hold  = -1;
  t->state = ST_PLAY;
  t->next  = tet_bag_next(t);
  tet_spawn(t, tet_bag_next(t));
  t->last  = desk_ms();
}

/* Where the piece would land */

static int tet_drop_y(struct tetris_s *t)
{
  int y = t->py;

  while (tet_fits(t, t->piece, t->rot, t->px, y + 1))
    {
      y++;
    }

  return y;
}

static bool tet_move(struct tetris_s *t, int dx, int dy)
{
  if (!tet_fits(t, t->piece, t->rot, t->px + dx, t->py + dy))
    {
      return false;
    }

  t->px += dx;
  t->py += dy;
  if (dx != 0 && t->lockms > 0 && t->lockmoves < TET_LOCK_MOVES)
    {
      t->lockms = 0;
      t->lockmoves++;
    }

  return true;
}

static void tet_rotate(struct tetris_s *t, int dir)
{
  int r = (t->rot + dir) & 3;
  int n = t->piece == 0 ? 8 : 6;
  int i;

  if (t->piece == 1)
    {
      return;                           /* O */
    }

  for (i = 0; i < n; i++)
    {
      int x = t->px + g_tet_kickx[i];
      int y = t->py + g_tet_kicky[i];

      if (tet_fits(t, t->piece, r, x, y))
        {
          t->px  = x;
          t->py  = y;
          t->rot = r;
          if (t->lockms > 0 && t->lockmoves < TET_LOCK_MOVES)
            {
              t->lockms = 0;
              t->lockmoves++;
            }

          return;
        }
    }
}

/* The piece becomes part of the board; full lines start to flash */

static void tet_lock(struct tetris_s *t)
{
  int cells[4][2];
  int y;
  int x;
  int i;

  tet_cells(t->piece, t->rot, cells);
  for (i = 0; i < 4; i++)
    {
      t->board[t->py + cells[i][1]][t->px + cells[i][0]] = t->piece + 1;
    }

  t->nclear = 0;
  for (y = 0; y < TET_ALL; y++)
    {
      for (x = 0; x < TET_COLS && t->board[y][x] != 0; x++)
        {
        }

      if (x == TET_COLS && t->nclear < 4)
        {
          t->clearrows[t->nclear++] = y;
        }
    }

  if (t->nclear > 0)
    {
      static const uint16_t points[5] =
      {
        0, 100, 300, 500, 800
      };

      t->score     += points[t->nclear] * t->level;
      t->state      = ST_CLEAR;
      t->clearstart = desk_ms();
      return;
    }

  tet_spawn(t, t->next);
  t->next = tet_bag_next(t);
}

/* The flash is over: the lines go, the rows above come down */

static void tet_collapse(struct tetris_s *t)
{
  int i;

  for (i = 0; i < t->nclear; i++)
    {
      int y = t->clearrows[i];

      memmove(t->board[1], t->board[0], y * TET_COLS);
      memset(t->board[0], 0, TET_COLS);
    }

  t->lines += t->nclear;
  t->level  = t->lines / 10 + 1;
  t->nclear = 0;
  t->state  = ST_PLAY;
  tet_spawn(t, t->next);
  t->next = tet_bag_next(t);
}

static void tet_hold(struct tetris_s *t)
{
  int p = t->piece;

  if (t->held)
    {
      return;
    }

  if (t->hold < 0)
    {
      t->hold = p;
      tet_spawn(t, t->next);
      t->next = tet_bag_next(t);
    }
  else
    {
      int h = t->hold;

      t->hold = p;
      tet_spawn(t, h);
    }

  t->held = true;
}

/* Time passes: gravity, the lock delay, the flash */

static void tet_step(struct tetris_s *t, int dt)
{
  int interval;

  if (t->state == ST_CLEAR)
    {
      if (desk_ms() - t->clearstart >= TET_CLEAR_MS)
        {
          tet_collapse(t);
        }

      return;
    }

  if (t->state != ST_PLAY)
    {
      return;
    }

  if (t->soft && desk_ms() - t->softms > 150)
    {
      t->soft = false;                  /* Down let go (no repeat since) */
    }

  interval = g_tet_speed[t->level - 1 < 14 ? t->level - 1 : 14];
  if (t->soft && interval > TET_SOFT_MS)
    {
      interval = TET_SOFT_MS;
    }

  t->fall += dt;
  while (t->fall >= interval)
    {
      t->fall -= interval;
      if (tet_move(t, 0, 1))
        {
          t->score += t->soft;
          t->lockms = 0;
        }
      else
        {
          t->fall = 0;
          break;
        }
    }

  if (!tet_fits(t, t->piece, t->rot, t->px, t->py + 1))
    {
      t->lockms += dt;
      if (t->lockms >= TET_LOCK_MS)
        {
          tet_lock(t);
        }
    }
}

/****************************************************************************
 * Private Functions: drawing helpers (tiles, the side panel)
 ****************************************************************************/

/* A tile: a round square with a gradient from lighter to its colour, a
 * gloss over its top half and a hairline of light
 */

static void tile_draw(struct desk_ops_s *o, int x, int y, int s, uint16_t c,
                      bool ghost)
{
  struct nx_gpuop_s *op;
  int r = s >= 20 ? 6 : 3;
  int slot = s >= 20 ? LUT_R6 : LUT_R3;
  int in = s >= 20 ? 2 : 1;
  int ts = s - 2 * in;

  ops_fill(o, x, y, s, s, C_TET_BG);
  ops_rrect(o, x + 1, y + 1, s - 2, s - 2, LUT_R3, 3, C_TET_WELL, 255);
  if (ghost)
    {
      /* A translucent tile, darker inside: an outline of the colour */

      ops_rrect(o, x + in, y + in, ts, ts, slot, r, c, 110);
      ops_rrect(o, x + in + 2, y + in + 2, ts - 4, ts - 4, LUT_R3, 3,
                C_TET_WELL, 210);
      ops_rrect(o, x + in + 2, y + in + 2, ts - 4, ts - 4, LUT_R3, 3, c,
                36);
      return;
    }

  ops_rrect_vgrad(o, x + in, y + in, ts, ts, slot, r,
                  colour_mix(c, C_WHITE, 70), colour_mix(c, C_BLACK, 40),
                  255);

  /* A gloss on the top half, a line of light along the top, a shade
   * along the bottom
   */

  op = ops_add(o);
  op->op     = NX_GPUOP_BLEND;
  op->x      = x + in;
  op->y      = y + in;
  op->w      = ts;
  op->h      = ts / 2;
  op->colour = C_WHITE;
  op->alpha  = 36;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_LUT | NX_BLEND_DITHER;
  op->slot   = slot;
  op->p[0]   = x + in + r;
  op->p[1]   = y + in + r;
  op->p[2]   = x + in + ts - 1 - r;
  op->p[3]   = y + in + ts - 1 - r;

  ops_blend(o, x + in + r, y + in, ts - 2 * r, 1, C_WHITE, 110);
  ops_blend(o, x + in + r, y + in + ts - 1, ts - 2 * r, 1, C_BLACK, 60);
}

/* The surface of tiles, at the current cell size */

static void tiles_make(struct tetris_s *t)
{
  int i;

  if (g_tet_tiles == NULL)
    {
      g_tet_tiles = desk_surface_open(TILE_N * TET_CELL_MAX, TET_CELL_MAX,
                                      &g_tet_lost);
      if (g_tet_tiles == NULL)
        {
          return;
        }
    }

  ops_reset(&t->o);
  for (i = 0; i < TILE_N; i++)
    {
      int x = i * TET_CELL_MAX;

      ui_flush(g_tet_tiles, &t->o, 24);
      if (i == TILE_EMPTY)
        {
          ops_fill(&t->o, x, 0, t->cell, t->cell, C_TET_BG);
          ops_rrect(&t->o, x + 1, 0 + 1, t->cell - 2, t->cell - 2, LUT_R3,
                    3, C_TET_WELL, 255);
        }
      else if (i < TILE_GHOST)
        {
          tile_draw(&t->o, x, 0, t->cell, g_tet_colour[i - 1], false);
        }
      else
        {
          tile_draw(&t->o, x, 0, t->cell, g_tet_colour[i - TILE_GHOST],
                    true);
        }
    }

  ops_draw(g_tet_tiles, &t->o);
  g_tet_tilecell = t->cell;
  g_tet_lost     = false;
}

/* One tile, copied from the surface */

static void tile_copy(struct desk_ops_s *o, int tile, int x, int y, int s)
{
  struct nx_gpuop_s *op = ops_add(o);

  op->op  = NX_GPUOP_COPY;
  op->x   = x;
  op->y   = y;
  op->w   = s;
  op->h   = s;
  op->src = g_tet_tiles;
  op->sx  = tile * TET_CELL_MAX;
  op->sy  = 0;
}

/* A piece in a box of the panel (the next, the held one) */

static void panel_piece(struct desk_ops_s *o, int x, int y, int w, int h,
                        int p, int cell)
{
  int cells[4][2];
  int minx = 4;
  int maxx = 0;
  int miny = 4;
  int maxy = 0;
  int i;

  ops_fill(o, x, y, w, h, C_WINDOW_BG);
  ops_rrect(o, x, y, w, h, LUT_R6, 6, C_TET_BG, 255);
  if (p < 0)
    {
      return;
    }

  tet_cells(p, 0, cells);
  for (i = 0; i < 4; i++)
    {
      minx = cells[i][0] < minx ? cells[i][0] : minx;
      maxx = cells[i][0] > maxx ? cells[i][0] : maxx;
      miny = cells[i][1] < miny ? cells[i][1] : miny;
      maxy = cells[i][1] > maxy ? cells[i][1] : maxy;
    }

  for (i = 0; i < 4; i++)
    {
      tile_copy(o, p + 1,
                x + (w - (maxx - minx + 1) * cell) / 2 +
                (cells[i][0] - minx) * cell,
                y + (h - (maxy - miny + 1) * cell) / 2 +
                (cells[i][1] - miny) * cell, cell);
    }
}

/* A figure of the panel: its label, then its value */

static void panel_figure(struct desk_ops_s *o, int x, int y, int w,
                         const char *label, unsigned int v)
{
  char buf[16];

  snprintf(buf, sizeof(buf), "%u", v);
  ops_fill(o, x, y, w, 44, C_WINDOW_BG);
  ops_text(o, &g_desk_font_small, x, y, label, C_TEXT_DIM, 255);
  ops_text(o, &g_desk_font_title, x, y + 16, buf, C_TEXT, 255);
}

/* The panel over the board: paused, game over */

static void board_overlay(struct desk_ops_s *o, int bx, int by, int bw,
                          int bh, const char *title, const char *hint)
{
  int w = bw - 24;
  int h = 92;
  int x = bx + 12;
  int y = by + (bh - h) / 2;

  ops_blend(o, bx, by, bw, bh, C_BLACK, 110);
  ops_rrect(o, x, y, w, h, LUT_R12, 12, RGB(0x24, 0x24, 0x2a), 235);
  ops_text_centre(o, &g_desk_font_title, x + w / 2, y + 20, title, C_TEXT,
                  255);
  ops_text_centre(o, &g_desk_font_small, x + w / 2, y + 56, hint,
                  C_TEXT_DIM, 255);
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static void tet_layout(struct desk_win_s *w)
{
  struct tetris_s *t = w->priv;
  int cell = (w->csize.h - 2 * TET_MARGIN) / TET_ROWS;
  int cellw = (w->csize.w - 2 * TET_MARGIN - 20 - TET_PANEL_W) / TET_COLS;

  cell = cellw < cell ? cellw : cell;
  cell = cell > TET_CELL_MAX ? TET_CELL_MAX : cell;
  cell = cell < TET_CELL_MIN ? TET_CELL_MIN : cell;

  t->cw    = w->csize.w;
  t->ch    = w->csize.h;
  t->cell  = cell;
  t->bx    = TET_MARGIN;
  t->by    = TITLE_H + (w->csize.h - TET_ROWS * cell) / 2;
  t->panx  = t->bx + TET_COLS * cell + 20;
  t->valid = false;

  if (g_tet_tilecell != cell || g_tet_tiles == NULL)
    {
      tiles_make(t);
    }
}

/* What each cell should show now: the board, the ghost, the piece */

static void tet_view(struct tetris_s *t, uint8_t view[TET_ROWS][TET_COLS])
{
  int cells[4][2];
  int x;
  int y;
  int i;

  for (y = 0; y < TET_ROWS; y++)
    {
      for (x = 0; x < TET_COLS; x++)
        {
          view[y][x] = t->board[y + TET_HIDDEN][x];
        }
    }

  if (t->state != ST_PLAY && t->state != ST_PAUSED)
    {
      return;
    }

  tet_cells(t->piece, t->rot, cells);
  y = tet_drop_y(t);
  for (i = 0; i < 4; i++)
    {
      int gy = y + cells[i][1] - TET_HIDDEN;

      if (gy >= 0)
        {
          view[gy][t->px + cells[i][0]] = TILE_GHOST + t->piece;
        }
    }

  for (i = 0; i < 4; i++)
    {
      int py = t->py + cells[i][1] - TET_HIDDEN;

      if (py >= 0)
        {
          view[py][t->px + cells[i][0]] = t->piece + 1;
        }
    }
}

/* Draw what has changed */

static void tet_update(struct desk_win_s *w)
{
  struct tetris_s *t = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  struct desk_ops_s *o = &t->o;
  uint8_t view[TET_ROWS][TET_COLS];
  uint8_t flash[TET_ROWS];
  bool overlay = t->state == ST_PAUSED || t->state == ST_OVER;
  int cell = t->cell;
  int bottom = t->by + TET_ROWS * cell;
  int x;
  int y;
  int i;

  if (g_tet_lost || g_tet_tilecell != cell)
    {
      tiles_make(t);
      t->valid = false;
    }

  ops_reset(o);
  if (!t->valid)
    {
      ops_fill(o, 0, TITLE_H, t->cw, t->ch, C_WINDOW_BG);
      ops_rrect(o, t->bx - 4, t->by - 4, TET_COLS * cell + 8,
                TET_ROWS * cell + 8, LUT_R6, 6, C_TET_FRAME, 255);
      ops_fill(o, t->bx, t->by, TET_COLS * cell, TET_ROWS * cell,
               C_TET_BG);
      ops_text(o, &g_desk_font_small, t->panx, t->by, "NEXT", C_TEXT_DIM,
               255);
      ops_text(o, &g_desk_font_small, t->panx, t->by + 104, "HOLD",
               C_TEXT_DIM, 255);
      ops_draw(hwnd, o);
      if (bottom - t->by >= 420)
        {
          ops_text(o, &g_desk_font_small, t->panx, bottom - 44,
                   "Arrows move, rotate", C_TEXT_DIM, 200);
          ops_text(o, &g_desk_font_small, t->panx, bottom - 30,
                   "Space drops, C holds", C_TEXT_DIM, 200);
          ops_text(o, &g_desk_font_small, t->panx, bottom - 16,
                   "P pauses", C_TEXT_DIM, 200);
          ops_draw(hwnd, o);
        }

      memset(t->drawn, 0xff, sizeof(t->drawn));
      memset(t->drawnflash, 0, sizeof(t->drawnflash));
      t->drawnnext  = -2;
      t->drawnhold  = -2;
      t->drawnscore = ~0u;
      t->drawnlines = ~0u;
      t->drawnlevel = ~0u;
      t->overlay    = false;
    }

  /* Leaving the pause: the board under its panel again */

  if (t->overlay && !overlay)
    {
      memset(t->drawn, 0xff, sizeof(t->drawn));
      t->overlay = false;
    }

  /* The cells: those that changed, and the rows of a flash */

  tet_view(t, view);
  memset(flash, 0, sizeof(flash));
  if (t->state == ST_CLEAR)
    {
      int age = desk_ms() - t->clearstart;
      int a = 255 - age * 255 / TET_CLEAR_MS;

      a = a < 1 ? 1 : a > 255 ? 255 : a;
      for (i = 0; i < t->nclear; i++)
        {
          if (t->clearrows[i] >= TET_HIDDEN)
            {
              flash[t->clearrows[i] - TET_HIDDEN] = (uint8_t)a;
            }
        }
    }

  for (y = 0; y < TET_ROWS; y++)
    {
      bool rowflash = flash[y] != t->drawnflash[y];

      for (x = 0; x < TET_COLS; x++)
        {
          if (view[y][x] == t->drawn[y][x] && !rowflash)
            {
              continue;
            }

          ui_flush(hwnd, o, 2);
          tile_copy(o, view[y][x], t->bx + x * cell, t->by + y * cell,
                    cell);
          t->drawn[y][x] = view[y][x];
        }

      if (flash[y] != 0)
        {
          ops_blend(o, t->bx, t->by + y * cell, TET_COLS * cell, cell,
                    C_WHITE, flash[y] * 3 / 4);
        }

      t->drawnflash[y] = flash[y];
    }

  /* The panel */

  ui_flush(hwnd, o, 40);
  if (t->drawnnext != t->next)
    {
      panel_piece(o, t->panx, t->by + 18, TET_PANEL_W - 20, 76, t->next,
                  cell < 22 ? cell : 22);
      t->drawnnext = t->next;
    }

  ui_flush(hwnd, o, 40);
  if (t->drawnhold != t->hold)
    {
      panel_piece(o, t->panx, t->by + 122, TET_PANEL_W - 20, 76, t->hold,
                  cell < 22 ? cell : 22);
      t->drawnhold = t->hold;
    }

  ui_flush(hwnd, o, 40);
  if (t->drawnscore != t->score)
    {
      panel_figure(o, t->panx, t->by + 218, TET_PANEL_W, "SCORE",
                   t->score);
      t->drawnscore = t->score;
    }

  ui_flush(hwnd, o, 40);
  if (t->drawnlines != t->lines)
    {
      panel_figure(o, t->panx, t->by + 270, TET_PANEL_W, "LINES",
                   t->lines);
      t->drawnlines = t->lines;
    }

  ui_flush(hwnd, o, 40);
  if (t->drawnlevel != t->level)
    {
      panel_figure(o, t->panx, t->by + 322, TET_PANEL_W, "LEVEL",
                   t->level);
      t->drawnlevel = t->level;
    }

  /* Paused, over: a panel over the board */

  if (overlay && !t->overlay)
    {
      ui_flush(hwnd, o, 64);
      board_overlay(o, t->bx, t->by, TET_COLS * cell, TET_ROWS * cell,
                    t->state == ST_OVER ? "Game Over" : "Paused",
                    t->state == ST_OVER ? "Enter: a new game" :
                                          "P: go on");
      t->overlay = true;
    }

  t->valid = true;
  ops_draw(hwnd, o);
}

static void tet_frames(struct desk_win_s *w)
{
  struct tetris_s *t = w->priv;

  w->frames = t->state == ST_PLAY || t->state == ST_CLEAR;
}

static void tet_frame(struct desk_win_s *w)
{
  struct tetris_s *t = w->priv;
  uint32_t now = desk_ms();
  int dt = now - t->last;

  t->last = now;
  tet_step(t, dt > 100 ? 100 : dt);
  tet_update(w);
  tet_frames(w);
}

static void tet_pause(struct desk_win_s *w, bool pause)
{
  struct tetris_s *t = w->priv;

  if (pause && (t->state == ST_PLAY || t->state == ST_CLEAR))
    {
      t->prevstate = t->state;
      t->state     = ST_PAUSED;
    }
  else if (!pause && t->state == ST_PAUSED)
    {
      t->state = t->prevstate;
      t->last  = desk_ms();
      if (t->state == ST_CLEAR)
        {
          t->clearstart = desk_ms();
        }
    }

  tet_update(w);
  tet_frames(w);
}

static void tet_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct tetris_s *t = w->priv;
  int keys[16];
  int nk = ui_keys(&t->keys, ch, n, keys, 16);
  int i;

  for (i = 0; i < nk; i++)
    {
      int k = keys[i] & ~KEY_SHIFT;

      if (k == '\r' || k == '\n' || k == KEY_F2)
        {
          if (t->state == ST_OVER || k == KEY_F2)
            {
              tet_new(t);
            }
          else if (t->state == ST_PAUSED)
            {
              tet_pause(w, false);
            }

          continue;
        }

      if (k == 'p' || k == 'P' || k == KEY_ESC)
        {
          tet_pause(w, t->state != ST_PAUSED);
          continue;
        }

      if (t->state == ST_PAUSED)
        {
          tet_pause(w, false);
        }

      if (t->state != ST_PLAY)
        {
          continue;
        }

      switch (k)
        {
          case KEY_LEFT:
            tet_move(t, -1, 0);
            break;

          case KEY_RIGHT:
            tet_move(t, 1, 0);
            break;

          case KEY_UP:
          case 'x':
          case 'X':
            tet_rotate(t, 1);
            break;

          case 'z':
          case 'Z':
            tet_rotate(t, 3);
            break;

          case KEY_DOWN:

            /* A row now, then fast while the key repeats */

            if (tet_move(t, 0, 1))
              {
                t->score++;
                t->fall = 0;
              }

            t->soft   = true;
            t->softms = desk_ms();
            break;

          case ' ':
            {
              int y = tet_drop_y(t);

              t->score += 2 * (y - t->py);
              t->py = y;
              tet_lock(t);
            }
            break;

          case 'c':
          case 'C':
            tet_hold(t);
            break;

          default:
            break;
        }
    }

  tet_update(w);
  tet_frames(w);
}

static void tet_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct tetris_s *t = w->priv;
  static uint8_t last;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(last & NX_MOUSE_LEFTBUTTON);

  last = buttons;
  if (!press)
    {
      return;
    }

  /* A click: a new game when it is over, on when paused */

  if (t->state == ST_OVER)
    {
      tet_new(t);
      tet_update(w);
      tet_frames(w);
    }
  else if (t->state == ST_PAUSED)
    {
      tet_pause(w, false);
    }
}

static void tet_focus(struct desk_win_s *w)
{
  if (!w->active)
    {
      tet_pause(w, true);
    }
}

static void tet_draw(struct desk_win_s *w)
{
  struct tetris_s *t = w->priv;

  if (t->cw != w->csize.w || t->ch != w->csize.h)
    {
      tet_layout(w);
    }

  t->valid = false;
  tet_update(w);
}

static void tet_resize(struct desk_win_s *w)
{
  tet_layout(w);
  tet_update(w);
}

static void tet_close(struct desk_win_s *w)
{
  w->frames = false;
  desk_surface_close(g_tet_tiles);
  g_tet_tiles    = NULL;
  g_tet_tilecell = 0;
}

/* The icon: a T, an L and part of a J, stacked */

static void tet_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  static const int8_t cells[30] =
  {
    1, 0, 2, 0, 1, 2, 1, 1, 2, 2, 1, 2,     /* T */
    3, 0, 6, 3, 1, 6, 3, 2, 6, 2, 2, 6,     /* L */
    0, 2, 5, 1, 2, 5                        /* J */
  };

  int c = s / 6;
  int x0 = x + (s - 4 * c) / 2;
  int y0 = y + (s - 3 * c) / 2;
  int i;

  icon_tile(o, x, y, s, RGB(0x3a, 0x3a, 0x52), RGB(0x16, 0x16, 0x24), a);
  for (i = 0; i < 10; i++)
    {
      int cx = x0 + cells[3 * i] * c;
      int cy = y0 + cells[3 * i + 1] * c;
      uint16_t col = g_tet_colour[cells[3 * i + 2]];

      ops_rrect(o, cx, cy, c - 1, c - 1, LUT_R3, 3, col, a);
    }
}

static struct desk_win_s *tet_open(void)
{
  struct desk_win_s *w;
  struct tetris_s *t;

  t = calloc(1, sizeof(*t));
  if (t == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_TETRIS, "Tetris", TET_W, TET_H);
  if (w == NULL)
    {
      free(t);
      return NULL;
    }

  w->priv = t;
  t->seed = desk_ms() * 2654435761u;
  tet_new(t);
  tet_layout(w);
  tet_update(w);
  tet_frames(w);
  return w;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_tetris =
{
  .name   = "Tetris",
  .icon   = tet_icon,
  .open   = tet_open,
  .draw   = tet_draw,
  .resize = tet_resize,
  .mouse  = tet_mouse,
  .key    = tet_key,
  .close  = tet_close,
  .frame  = tet_frame,
  .focus  = tet_focus,
  .single = true,
};
