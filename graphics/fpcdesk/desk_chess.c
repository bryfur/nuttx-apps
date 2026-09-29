/****************************************************************************
 * apps/graphics/fpcdesk/desk_chess.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chess: a board, and an engine to play against (desk_chess_engine.c).
 *
 * Play against the computer as White or Black at six levels (the first
 * three search to a fixed depth with some noise in their judgement, the
 * others think for 1, 3 or 10 seconds a move), or two players at one
 * board.  Move by dragging a piece or by clicking it and then where it
 * goes; its legal moves show as dots (rings where it takes).  The last
 * move is marked in the accent, a king in check glows red, a pawn that
 * reaches the last rank asks what it becomes.  The moves are listed in
 * SAN; each player's captures and the material ahead show beside the
 * board.  Undo and Redo (Ctrl-Z, Ctrl-Y) take back or replay moves (a
 * move and the reply against the computer), New Game (F2), Flip (F);
 * Stop makes the computer move now.  Checkmate, stalemate, the fifty-move
 * rule, threefold repetition and insufficient material end the game.  The
 * menu copies the position (FEN) or the moves, or sets up a position from
 * a FEN (Ctrl-L).
 *
 * The engine runs in a thread of its own (below the desktop's priority),
 * with its search state on that thread's stack (the SRAM on the board)
 * and a transposition table of CONFIG_FPCDESK_CHESS_TT_KB from the heap
 * (the PSRAM), allocated while the window is open.  It reports each depth
 * and its move with desk_wake().
 *
 * Drawing: the pieces are drawn once for the board's size on the CPU into
 * coverage masks (desk_chess_pieces.c), kept in a surface of the GPU's
 * memory, and each piece on the board is four BLENDs from there in its
 * colours (as are the move dots, the rings and the glow of a check).  The
 * board is the kit's custom widget; a move draws again only the squares
 * it changed, and a dragged piece only the squares it leaves and the
 * piece where it is.
 ****************************************************************************/

#include <nuttx/config.h>

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "desk_kit.h"
#include "desk_chess.h"

#ifdef CONFIG_FPCDESK_CHESS

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CHESS_W         800
#define CHESS_H         600
#define SIDE_W          232
#define NWG             48
#define GAME_MAX        600              /* Plies */
#define SQ_MIN          24
#define SQ_MAX          112
#define SMALL           26               /* Captured pieces */
#define DRAG_START      4                /* Pixels before a press drags */

#define MODE_TWO        0
#define MODE_WHITE      1                /* The player is White */
#define MODE_BLACK      2

#define RES_NONE        0
#define RES_MATE        1
#define RES_STALEMATE   2
#define RES_FIFTY       3
#define RES_REPEAT      4
#define RES_MATERIAL    5
#define RES_LONG        6

/* The widgets */

enum
{
  W_NONE = 0,
  W_NEW,
  W_UNDO,
  W_REDO,
  W_FLIP,
  W_MODE,
  W_LEVEL,
  W_STOP,
  W_MENU,
  W_BOARD,
  W_TOPNAME,
  W_TOPCAPS,
  W_MOVES,
  W_BOTNAME,
  W_BOTCAPS,
  W_STATUS,
  W_INFO
};

/* Dialogs and menus */

#define DLG_FEN         1
#define DLG_OVER        2
#define MENU_MAIN       1

/* The surface: the pieces' masks in rows (one a mask), the marks after
 * them, then the small pieces' masks
 */

#define SURF_W(sq)      (6 * (sq) > 12 * SMALL ? 6 * (sq) : 12 * SMALL)
#define SURF_H(sq)      (5 * (sq) + 2 * SMALL)

/* The pieces' colours by mask: the body, its shade, the outline, the
 * lines inside
 */

#define C_WFILL         RGB(0xfa, 0xfa, 0xf6)
#define C_WSHADE        RGB(0xcd, 0xc8, 0xbd)
#define C_WINK          RGB(0x26, 0x23, 0x1f)
#define C_BFILL         RGB(0x46, 0x42, 0x3d)
#define C_BSHADE        RGB(0x1e, 0x1c, 0x19)
#define C_BINK          RGB(0x14, 0x12, 0x0f)
#define C_BLINE         RGB(0xd9, 0xd4, 0xc8)
#define C_CHECK         RGB(0xe8, 0x2a, 0x2a)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct level_s
{
  const char *name;
  uint8_t  depth;                        /* 0: by time */
  uint16_t ms;
  uint8_t  noise;                        /* +- centipawns */
};

/* The game: the moves from a position, and where it is now */

struct game_s
{
  struct ch_pos_s start;
  struct ch_pos_s pos;                   /* After n moves */
  int      n;                            /* Played */
  int      total;                        /* Recorded (Redo goes to it) */
  int      result;                       /* RES_* of pos */
  ch_move_t move[GAME_MAX];
  char     san[GAME_MAX][8];
  uint64_t hash[GAME_MAX + 1];           /* Of the position before each */
};

/* The engine's thread: what it is given and what it answers */

struct think_s
{
  pthread_t thread;
  bool      running;                     /* The thread is there */
  volatile bool stop;                    /* Stop: move now */
  bool      discard;                     /* Its answer is not wanted */
  pthread_mutex_t lock;
  struct ch_pos_s pos;
  uint64_t  hist[128];
  int       nhist;
  int       depth;
  uint32_t  ms;
  int       noise;
  uint32_t  seed;
  uint8_t   age;
  uint32_t  polls;
  bool      done;                        /* Under lock: the answer is in */
  bool      news;                        /* ... a depth is done */
  ch_move_t best;
  struct ch_info_s info;
};

struct chess_s
{
  struct kit_s kit;
  struct kit_widget_s wg[NWG];
  struct kit_list_s list;
  struct kit_surface_s surf;
  struct desk_win_s *w;

  /* The game and the players */

  struct game_s g;
  int      mode;                         /* MODE_* */
  int      level;
  bool     flip;                         /* Black at the bottom */
  ch_move_t legal[CH_MAXMOVES];          /* Of g.pos */
  int      nlegal;

  /* The board: where it is, the surface's size */

  int      sq;                           /* A square, pixels */
  int      bx;                           /* The board's top left */
  int      by;
  int      made;                         /* The surface's square size, 0 */

  /* Choosing and moving: squares are 0-63 (a1 = 0), -1: none */

  int      sel;                          /* Selected */
  bool      wassel;                       /* ... already before the press */
  int      press;                        /* Where the button went down */
  int      pressx;
  int      pressy;
  bool     dragging;
  int      dragx;                        /* The dragged piece's top left */
  int      dragy;
  int      hover;                        /* The square dragged over */
  uint8_t  buttons;
  ch_move_t promo[4];                    /* Asking: Q, N, R, B; 0: not */
  int      promohot;

  /* The engine */

  struct think_s think;
  struct ch_tt_s *tt;
  uint32_t ttn;
  uint32_t seed;
  uint8_t  age;
  bool     thinking;

  /* Texts the widgets show */

  char     topname[48];
  char     botname[48];
  char     status[64];
  char     info[64];
  char     answer[96];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_modes[] =
{
  "Two players", "Play as White", "Play as Black"
};

static const struct level_s g_levels[] =
{
  { "Beginner",       1,     0, 150 },
  { "Casual",         2,     0,  70 },
  { "Intermediate",   4,     0,  25 },
  { "Club (1 s)",     0,  1000,   0 },
  { "Strong (3 s)",   0,  3000,   0 },
  { "Master (10 s)",  0, 10000,   0 },
};

#define NLEVELS         (int)(sizeof(g_levels) / sizeof(g_levels[0]))

static const char *g_levelnames[NLEVELS];

static const char * const g_menu[] =
{
  "Load Position (FEN)" DESK_CH_ELLIPSIS,
  "Copy Position (FEN)",
  "Copy Moves",
};

static const struct kit_col_s g_cols[] =
{
  { "",      44, 0, KIT_COL_DIM | KIT_COL_RIGHT },
  { "White", 82, 0, 0 },
  { "Black",  0, 0, 0 },
};

static const char g_start[] =
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

static const int g_worth[7] =
{
  0, 1, 3, 3, 5, 9, 0
};

/****************************************************************************
 * Private Functions: the game
 ****************************************************************************/

static bool human_turn(struct chess_s *c)
{
  return c->mode == MODE_TWO ||
         (c->mode == MODE_WHITE) == (c->g.pos.side == CH_WHITE);
}

/* The position after n moves */

static void game_goto(struct game_s *g, int n)
{
  struct ch_undo_s u;
  int i;

  g->pos = g->start;
  for (i = 0; i < n; i++)
    {
      ch_make(&g->pos, g->move[i], &u);
    }

  g->n = n;
}

static int game_result(struct game_s *g, int nlegal)
{
  struct ch_pos_s *p = &g->pos;
  int reps = 1;
  int i;

  if (nlegal == 0)
    {
      return ch_incheck(p, p->side) ? RES_MATE : RES_STALEMATE;
    }

  if (p->halfmove >= 100)
    {
      return RES_FIFTY;
    }

  for (i = g->n - 2; i >= 0 && i >= g->n - p->halfmove; i -= 2)
    {
      reps += g->hash[i] == p->hash;
    }

  if (reps >= 3)
    {
      return RES_REPEAT;
    }

  if (ch_insufficient(p))
    {
      return RES_MATERIAL;
    }

  return g->n >= GAME_MAX ? RES_LONG : RES_NONE;
}

static void game_new(struct game_s *g, const struct ch_pos_s *start)
{
  g->start = *start;
  g->pos   = *start;
  g->n     = 0;
  g->total = 0;
  g->hash[0] = start->hash;
}

static void game_play(struct game_s *g, ch_move_t m)
{
  struct ch_undo_s u;
  int i = g->n;

  ch_san(&g->pos, m, g->san[i]);
  g->move[i] = m;
  g->hash[i] = g->pos.hash;
  ch_make(&g->pos, m, &u);
  g->n = i + 1;
  g->total = g->n;
}

/****************************************************************************
 * Private Functions: the board's geometry
 ****************************************************************************/

/* Square s (0-63) on the screen */

static void sq_xy(struct chess_s *c, int s, int *x, int *y)
{
  int f = s & 7;
  int r = s >> 3;

  if (c->flip)
    {
      f = 7 - f;
      r = 7 - r;
    }

  *x = c->bx + f * c->sq;
  *y = c->by + (7 - r) * c->sq;
}

static int sq_at(struct chess_s *c, int x, int y)
{
  int f;
  int r;

  if (x < c->bx || y < c->by || x >= c->bx + 8 * c->sq ||
      y >= c->by + 8 * c->sq)
    {
      return -1;
    }

  f = (x - c->bx) / c->sq;
  r = 7 - (y - c->by) / c->sq;
  return c->flip ? (7 - r) * 8 + 7 - f : r * 8 + f;
}

static inline int sq88(int s)
{
  return (s >> 3) * 16 + (s & 7);
}

static inline int sq64(int s)
{
  return CH_SQ64(s);
}

static void board_geometry(struct chess_s *c)
{
  struct kit_widget_s *wg = kit_get(&c->kit, W_BOARD);
  int sq;

  if (wg == NULL)
    {
      return;
    }

  sq = (wg->w < wg->h ? wg->w : wg->h) / 8;
  sq = sq < SQ_MIN ? SQ_MIN : sq > SQ_MAX ? SQ_MAX : sq & ~1;
  c->sq = sq;
  c->bx = wg->x + (wg->w - 8 * sq) / 2;
  c->by = wg->y + (wg->h - 8 * sq) / 2;
}

/****************************************************************************
 * Private Functions: the surface
 ****************************************************************************/

/* One mask of size x size into the surface at (x, y): the coverage in
 * each pixel's low byte (BLEND's A8 source)
 */

static void mask_put(struct chess_s *c, const uint8_t *m, int size, int x,
                     int y, uint16_t *row)
{
  struct nxgl_rect_s dest;
  struct nxgl_point_s origin;
  const void *src[1];
  int i;
  int j;

  for (j = 0; j < size; j++)
    {
      for (i = 0; i < size; i++)
        {
          row[i] = m[j * size + i];
        }

      dest.pt1.x = x;
      dest.pt1.y = y + j;
      dest.pt2.x = x + size - 1;
      dest.pt2.y = y + j;
      origin     = dest.pt1;
      src[0]     = row;
      nx_bitmap(c->surf.hwnd, &dest, src, &origin, size * 2);
    }
}

/* The pieces at the board's size (and small), the marks: drawn on the CPU
 * (now and then only: a new size) and put in the surface
 */

static void surface_make(struct chess_s *c)
{
  int sq = c->sq;
  uint8_t *m;
  uint16_t *row;
  int t;
  int k;

  if (c->made != sq || c->surf.hwnd == NULL)
    {
      kit_surface_close(&c->surf);
      c->made = 0;
      if (kit_surface_open(&c->surf, SURF_W(sq), SURF_H(sq), c->w) < 0)
        {
          return;
        }
    }

  m   = malloc(CH_MASKS * sq * sq);
  row = malloc(sq * 2);
  if (m == NULL || row == NULL)
    {
      free(m);
      free(row);
      return;
    }

  for (t = CH_PAWN; t <= CH_KING; t++)
    {
      ch_piece_masks(t, sq, 0, m);
      for (k = 0; k < CH_MASKS; k++)
        {
          mask_put(c, m + k * sq * sq, sq, (t - 1) * sq, k * sq, row);
        }

      ch_piece_masks(t, SMALL, 1, m);
      for (k = 0; k < CH_MASKS; k++)
        {
          mask_put(c, m + k * SMALL * SMALL, SMALL,
                   ((t - 1) * 2 + (k & 1)) * SMALL,
                   5 * sq + (k >> 1) * SMALL, row);
        }
    }

  for (k = 0; k < 3; k++)
    {
      ch_mark_mask(k, sq, m);
      mask_put(c, m, sq, k * sq, 4 * sq, row);
    }

  free(m);
  free(row);
  c->surf.lost = false;
  c->made = sq;
}

/* A piece: its four masks blended in its colours */

static void piece_draw(struct chess_s *c, struct desk_ops_s *o, int pc,
                       int x, int y, bool small)
{
  static const uint16_t white[CH_MASKS] =
  {
    C_WFILL, C_WSHADE, C_WINK, C_WINK
  };

  static const uint16_t black[CH_MASKS] =
  {
    C_BFILL, C_BSHADE, C_BINK, C_BLINE
  };

  /* Small black pieces on a dark card: an outline that shows */

  static const uint16_t lit[CH_MASKS] =
  {
    C_BFILL, C_BSHADE, RGB(0x9a, 0x96, 0x8e), C_BLINE
  };

  const uint16_t *col = CH_SIDE(pc) == CH_WHITE ? white :
                        small && g_theme.dark ? lit : black;
  int t = CH_TYPE(pc);
  int k;

  if (c->made == 0 || t < CH_PAWN || t > CH_KING)
    {
      return;
    }

  for (k = 0; k < CH_MASKS; k++)
    {
      if (small)
        {
          kit_surface_blend(o, &c->surf, ((t - 1) * 2 + (k & 1)) * SMALL,
                            5 * c->made + (k >> 1) * SMALL, SMALL, SMALL,
                            x, y, NX_BLEND_SRC_A8, col[k], 255);
        }
      else
        {
          kit_surface_blend(o, &c->surf, (t - 1) * c->sq, k * c->sq, c->sq,
                            c->sq, x, y, NX_BLEND_SRC_A8, col[k], 255);
        }
    }
}

static void mark_draw(struct chess_s *c, struct desk_ops_s *o, int kind,
                      int x, int y, uint16_t colour, uint8_t alpha)
{
  if (c->made != 0)
    {
      kit_surface_blend(o, &c->surf, kind * c->sq, 4 * c->sq, c->sq, c->sq,
                        x, y, NX_BLEND_SRC_A8, colour, alpha);
    }
}

/****************************************************************************
 * Private Functions: drawing the board
 ****************************************************************************/

static uint16_t sq_colour(bool light)
{
  if (light)
    {
      return THEMED(RGB(0xc9, 0xd1, 0xd8), RGB(0xf0, 0xd9, 0xb5));
    }

  return THEMED(RGB(0x6e, 0x84, 0x96), RGB(0xb5, 0x88, 0x63));
}

static bool is_light(int s)
{
  return ((s & 7) + (s >> 3)) & 1;
}

static bool is_target(struct chess_s *c, int s)
{
  int i;

  if (c->sel < 0)
    {
      return false;
    }

  for (i = 0; i < c->nlegal; i++)
    {
      if (CH_FROM(c->legal[i]) == sq88(c->sel) &&
          CH_TO(c->legal[i]) == sq88(s))
        {
          return true;
        }
    }

  return false;
}

/* The promotion chooser: which of its squares s is (0-3), or -1 */

static int promo_item(struct chess_s *c, int s)
{
  int to;
  int i;

  if (c->promo[0] == CH_NOMOVE)
    {
      return -1;
    }

  to = sq64(CH_TO(c->promo[0]));
  for (i = 0; i < 4; i++)
    {
      if (s == (c->g.pos.side == CH_WHITE ? to - 8 * i : to + 8 * i))
        {
          return i;
        }
    }

  return -1;
}

static void square_draw(struct chess_s *c, struct desk_ops_s *o, int s)
{
  struct ch_pos_s *p = &c->g.pos;
  bool light = is_light(s);
  int pc = p->board[sq88(s)];
  int item = promo_item(c, s);
  int sq = c->sq;
  int x;
  int y;

  sq_xy(c, s, &x, &y);
  ops_fill(o, x, y, sq, sq, sq_colour(light));

  if (item >= 0)
    {
      static const uint8_t kinds[4] =
      {
        CH_QUEEN, CH_KNIGHT, CH_ROOK, CH_BISHOP
      };

      ops_rrect(o, x + 3, y + 3, sq - 6, sq - 6, LUT_R12, 12,
                item == c->promohot ? g_theme.accent : g_theme.card, 245);
      piece_draw(c, o, CH_PC(p->side, kinds[item]), x, y, false);
      return;
    }

  /* The last move, the selection, where a dragged piece would go */

  if (c->g.n > 0)
    {
      ch_move_t m = c->g.move[c->g.n - 1];

      if (s == sq64(CH_FROM(m)) || s == sq64(CH_TO(m)))
        {
          ops_blend(o, x, y, sq, sq, g_theme.accent, 105);
        }
    }

  if (s == c->sel)
    {
      ops_blend(o, x, y, sq, sq, g_theme.accent, 165);
    }

  if (c->dragging && s == c->hover && is_target(c, s))
    {
      ops_blend(o, x, y, sq, sq, C_WHITE, 70);
    }

  /* A king in check */

  if (CH_TYPE(pc) == CH_KING && ch_incheck(p, CH_SIDE(pc)))
    {
      mark_draw(c, o, CH_MARK_CHECK, x, y, C_CHECK, 235);
    }

  /* The coordinates: ranks at the left, files at the bottom, in the other
   * square's colour
   */

  if ((c->flip ? 7 - (s & 7) : (s & 7)) == 0)
    {
      char r[2] = { (char)('1' + (s >> 3)), '\0' };

      ops_text(o, sq >= 56 ? KIT_FONT_BOLD : KIT_FONT_SMALL, x + 3, y + 2,
               r, sq_colour(!light), 255);
    }

  if ((c->flip ? 7 - (s >> 3) : (s >> 3)) == 0)
    {
      char f[2] = { (char)('a' + (s & 7)), '\0' };
      const struct desk_font_s *fn = sq >= 56 ? KIT_FONT_BOLD :
                                                KIT_FONT_SMALL;

      ops_text(o, fn, x + sq - 3 - text_width(fn, f),
               y + sq - 2 - text_height(fn), f, sq_colour(!light), 255);
    }

  if (pc != CH_EMPTY && !(c->dragging && s == c->press))
    {
      piece_draw(c, o, pc, x, y, false);
    }

  if (is_target(c, s))
    {
      mark_draw(c, o, pc != CH_EMPTY ? CH_MARK_RING : CH_MARK_DOT, x, y,
                C_BLACK, pc != CH_EMPTY ? 70 : 60);
    }

  if (c->promo[0] != CH_NOMOVE)
    {
      ops_blend(o, x, y, sq, sq, C_BLACK, 120);
    }
}

/* Everything of the board's widget in [x0, x1) x [y0, y1): the ground
 * around the board, the squares, a dragged piece; each piece of it cut
 * to the rectangle
 */

static void board_paint(struct chess_s *c, int x0, int y0, int x1, int y1)
{
  struct kit_s *k = &c->kit;
  struct kit_widget_s *wg = kit_get(k, W_BOARD);
  struct desk_ops_s *o;
  int bw = 8 * c->sq;
  int from;
  int s;

  if (wg == NULL)
    {
      return;
    }

  if (c->made != c->sq || c->surf.lost)
    {
      surface_make(c);
    }

  /* The ground: the four strips around the board, a shadow under it */

  o = kit_ops(k, 12);
  from = o->n;
  ops_fill(o, wg->x, wg->y, wg->w, c->by - wg->y, g_theme.window_bg);
  ops_fill(o, wg->x, c->by + bw, wg->w, wg->y + wg->h - c->by - bw,
           g_theme.window_bg);
  ops_fill(o, wg->x, c->by, c->bx - wg->x, bw, g_theme.window_bg);
  ops_fill(o, c->bx + bw, c->by, wg->x + wg->w - c->bx - bw, bw,
           g_theme.window_bg);
  ops_rrect(o, c->bx - 3, c->by - 2, bw + 6, bw + 7, LUT_R6, 6, C_BLACK,
            THEMED(90, 40));
  ui_clip(o, from, x0, y0, x1, y1);

  for (s = 0; s < 64; s++)
    {
      int x;
      int y;

      sq_xy(c, s, &x, &y);
      if (x >= x1 || y >= y1 || x + c->sq <= x0 || y + c->sq <= y0)
        {
          continue;
        }

      o = kit_ops(k, 24);
      from = o->n;
      square_draw(c, o, s);
      ui_clip(o, from, x0 > x ? x0 : x, y0 > y ? y0 : y,
              x1 < x + c->sq ? x1 : x + c->sq,
              y1 < y + c->sq ? y1 : y + c->sq);
    }

  if (c->dragging)
    {
      int pc = c->g.pos.board[sq88(c->press)];

      o = kit_ops(k, 12);
      from = o->n;
      piece_draw(c, o, pc, c->dragx, c->dragy, false);
      ui_clip(o, from, x0, y0, x1, y1);
    }
}

/* Draw these squares again now (a bit each, a1 first), and the rectangle
 * r if it is not empty
 */

static void board_update(struct chess_s *c, uint64_t squares, int rx,
                         int ry, int rw, int rh)
{
  int ux0 = 0x7fff;
  int uy0 = 0x7fff;
  int ux1 = -1;
  int uy1 = -1;
  int s;

  if (c->sq == 0)
    {
      return;
    }

  for (s = 0; s < 64; s++)
    {
      int x;
      int y;

      if ((squares >> s & 1) == 0)
        {
          continue;
        }

      sq_xy(c, s, &x, &y);
      board_paint(c, x, y, x + c->sq, y + c->sq);
      ux0 = x < ux0 ? x : ux0;
      uy0 = y < uy0 ? y : uy0;
      ux1 = x + c->sq > ux1 ? x + c->sq : ux1;
      uy1 = y + c->sq > uy1 ? y + c->sq : uy1;
    }

  if (rw > 0 && rh > 0)
    {
      board_paint(c, rx, ry, rx + rw, ry + rh);
      ux0 = rx < ux0 ? rx : ux0;
      uy0 = ry < uy0 ? ry : uy0;
      ux1 = rx + rw > ux1 ? rx + rw : ux1;
      uy1 = ry + rh > uy1 ? ry + rh : uy1;
    }

  if (ux1 > ux0)
    {
      kit_touched(&c->kit, ux0, uy0, ux1 - ux0, uy1 - uy0);
    }
}

/* The selection's squares: it and its targets */

static uint64_t sel_squares(struct chess_s *c)
{
  uint64_t m = 0;
  int i;

  if (c->sel < 0)
    {
      return 0;
    }

  m = 1ull << c->sel;
  for (i = 0; i < c->nlegal; i++)
    {
      if (CH_FROM(c->legal[i]) == sq88(c->sel))
        {
          m |= 1ull << sq64(CH_TO(c->legal[i]));
        }
    }

  return m;
}

/* The squares a move changes, and where each side's king is */

static uint64_t move_squares(struct chess_s *c, ch_move_t m)
{
  uint64_t r = 1ull << sq64(CH_FROM(m)) | 1ull << sq64(CH_TO(m));
  int to = CH_TO(m);

  if (CH_FLAGS(m) & CH_F_CASTLE)
    {
      r |= 0xa1ull << (CH_RANK(to) * 8) | 0x09ull << (CH_RANK(to) * 8);
    }

  if (CH_FLAGS(m) & CH_F_EP)
    {
      r |= 1ull << (sq64(to) + (CH_RANK(to) == 5 ? -8 : 8));
    }

  return r;
}

static uint64_t king_squares(struct chess_s *c)
{
  return 1ull << sq64(c->g.pos.king[0]) | 1ull << sq64(c->g.pos.king[1]);
}

/****************************************************************************
 * Private Functions: the panel beside the board
 ****************************************************************************/

/* The pieces a side has taken (the other side's), by value, and how much
 * it is ahead in material
 */

static void caps_draw(struct chess_s *c, struct kit_widget_s *wg, int side)
{
  struct desk_ops_s *o;
  struct game_s *g = &c->g;
  int count[7];
  int ahead = 0;
  int x = wg->x;
  int y = wg->y + (wg->h - SMALL) / 2;
  int i;
  int t;

  /* Taken: what the other side is missing of its full set (so a
   * position set up from a FEN shows them too; a promoted pawn is
   * missing, its new piece is not counted)
   */

  for (t = CH_PAWN; t <= CH_QUEEN; t++)
    {
      static const int8_t full[6] =
      {
        0, 8, 2, 2, 2, 1
      };

      count[t] = full[t] - g->pos.npieces[CH_PC(side ^ 1, t)];
      count[t] = count[t] < 0 ? 0 : count[t];
    }

  for (i = 0; i < 128; i++)
    {
      int pc = CH_OFF(i) ? 0 : g->pos.board[i];

      if (pc != CH_EMPTY)
        {
          ahead += CH_SIDE(pc) == side ? g_worth[CH_TYPE(pc)] :
                                         -g_worth[CH_TYPE(pc)];
        }
    }

  o = kit_ops(&c->kit, 8);
  ops_fill(o, wg->x, wg->y, wg->w, wg->h, g_theme.card);
  for (t = CH_PAWN; t <= CH_QUEEN; t++)
    {
      for (i = 0; i < count[t] && x + SMALL <= wg->x + wg->w - 30; i++)
        {
          o = kit_ops(&c->kit, CH_MASKS);
          piece_draw(c, o, CH_PC(side ^ 1, t), x, y, true);
          x += SMALL / 2;
        }

      if (count[t] > 0)
        {
          x += SMALL / 2 + 2;
        }
    }

  if (ahead > 0)
    {
      char buf[16];

      snprintf(buf, sizeof(buf), "+%d", ahead);
      o = kit_ops(&c->kit, 8);
      ops_text(o, KIT_FONT_BOLD, x + SMALL / 2 + 2, wg->y + (wg->h -
               text_height(KIT_FONT_BOLD)) / 2, buf, g_theme.text_dim, 255);
    }
}

static const char *moves_text(struct kit_s *k, struct kit_widget_s *wg,
                              int row, int col, char *buf, int size)
{
  struct chess_s *c = k->priv;
  struct game_s *g = &c->g;
  int off = g->start.side == CH_BLACK;
  int i = row * 2 + col - 1 - off;

  if (col == 0)
    {
      snprintf(buf, size, "%d.", g->start.fullmove + row);
      return buf;
    }

  if (i < 0)
    {
      return DESK_CH_ELLIPSIS;
    }

  return i < g->n ? g->san[i] : NULL;
}

static void names_set(struct chess_s *c)
{
  static const char * const side[2] =
  {
    "White", "Black"
  };

  int bottom = c->flip ? CH_BLACK : CH_WHITE;
  int s;

  for (s = 0; s < 2; s++)
    {
      int who = s == 0 ? bottom : bottom ^ 1;
      char *buf = s == 0 ? c->botname : c->topname;
      bool computer = (c->mode == MODE_WHITE && who == CH_BLACK) ||
                      (c->mode == MODE_BLACK && who == CH_WHITE);

      if (computer)
        {
          snprintf(buf, 48, "%s\nComputer, %s", side[who],
                   g_levels[c->level].name);
        }
      else
        {
          snprintf(buf, 48, "%s\n%s", side[who],
                   c->mode == MODE_TWO ? "Player" : "You");
        }
    }

  kit_set_text(&c->kit, W_TOPNAME, c->topname);
  kit_set_text(&c->kit, W_BOTNAME, c->botname);
  kit_invalidate(&c->kit, W_TOPCAPS);
  kit_invalidate(&c->kit, W_BOTCAPS);
}

static void score_text(char *buf, int size, int score, int side)
{
  int white = side == CH_WHITE ? score : -score;

  if (CH_ISMATE(score))
    {
      int plies = CH_MATE - abs(score);

      snprintf(buf, size, "%s mates in %d", white > 0 ? "White" : "Black",
               (plies + 1) / 2);
    }
  else
    {
      snprintf(buf, size, "%c%d.%02d", white < 0 ? '-' : '+',
               abs(white) / 100, abs(white) % 100);
    }
}

static void status_set(struct chess_s *c)
{
  static const char * const side[2] =
  {
    "White", "Black"
  };

  struct ch_pos_s *p = &c->g.pos;

  switch (c->g.result)
    {
      case RES_MATE:
        snprintf(c->status, sizeof(c->status), "Checkmate " DESK_CH_MIDDOT
                 " %s wins", side[p->side ^ 1]);
        break;

      case RES_STALEMATE:
        strlcpy(c->status, "Stalemate " DESK_CH_MIDDOT " Draw",
                sizeof(c->status));
        break;

      case RES_FIFTY:
        strlcpy(c->status, "Draw " DESK_CH_MIDDOT " Fifty-move rule",
                sizeof(c->status));
        break;

      case RES_REPEAT:
        strlcpy(c->status, "Draw " DESK_CH_MIDDOT " Threefold repetition",
                sizeof(c->status));
        break;

      case RES_MATERIAL:
        strlcpy(c->status, "Draw " DESK_CH_MIDDOT " Insufficient material",
                sizeof(c->status));
        break;

      case RES_LONG:
        strlcpy(c->status, "Draw " DESK_CH_MIDDOT " The game is too long",
                sizeof(c->status));
        break;

      default:
        snprintf(c->status, sizeof(c->status), "%s%s%s", c->thinking ?
                 "Thinking" DESK_CH_ELLIPSIS : side[p->side],
                 c->thinking ? "" : " to move",
                 ch_incheck(p, p->side) ? " " DESK_CH_MIDDOT " Check" : "");
        break;
    }

  kit_set_text(&c->kit, W_STATUS, c->status);
}

static void info_set(struct chess_s *c, const struct ch_info_s *in,
                     int side)
{
  char score[32];

  if (in == NULL || in->depth == 0)
    {
      c->info[0] = '\0';
    }
  else
    {
      score_text(score, sizeof(score), in->score, side);
      snprintf(c->info, sizeof(c->info), "Depth %d " DESK_CH_MIDDOT " %s "
               DESK_CH_MIDDOT " %lu kN/s", in->depth, score,
               (unsigned long)(in->ms > 0 ? in->nodes / in->ms :
                               in->nodes / 1000));
    }

  kit_set_text(&c->kit, W_INFO, c->info);
}

/* The widgets after the game changed */

static void panel_update(struct chess_s *c)
{
  struct kit_s *k = &c->kit;
  int off = c->g.start.side == CH_BLACK;
  int rows = (c->g.n + off + 1) / 2;

  c->list.n = rows;
  kit_list_changed(k, W_MOVES);
  kit_list_select(k, W_MOVES, rows - 1);
  if (rows > 0)
    {
      kit_list_show(k, W_MOVES, rows - 1);
    }

  kit_enable(k, W_UNDO, c->g.n > 0);
  kit_enable(k, W_REDO, c->g.total > c->g.n && !c->thinking);
  kit_enable(k, W_STOP, c->thinking);
  kit_invalidate(k, W_TOPCAPS);
  kit_invalidate(k, W_BOTCAPS);
  status_set(c);
}

/****************************************************************************
 * Private Functions: the engine's thread
 ****************************************************************************/

static void think_report(void *arg, const struct ch_info_s *info)
{
  struct chess_s *c = arg;

  pthread_mutex_lock(&c->think.lock);
  c->think.info = *info;
  c->think.news = true;
  pthread_mutex_unlock(&c->think.lock);
  desk_wake(c->w);
}

/* The simulator runs one thread at a time and gets to its interrupts
 * (the desktop's input, the clock) only when every thread waits: let it,
 * every few thousand nodes.  The board preempts.
 */

static void think_poll(void *arg)
{
#ifdef CONFIG_ARCH_SIM
  struct chess_s *c = arg;

  if ((++c->think.polls & 15) == 0)
    {
      usleep(1000);
    }
#endif
}

static void *think_main(void *arg)
{
  struct chess_s *c = arg;
  struct think_s *t = &c->think;
  struct ch_search_s s;                  /* Big: on this thread's stack */
  ch_move_t m;

  memset(&s, 0, sizeof(s));
  s.pos      = t->pos;
  s.tt       = c->tt;
  s.ttn      = c->ttn;
  s.maxdepth = t->depth;
  s.maxms    = t->ms;
  s.seed     = t->seed;
  s.noise    = t->noise;
  s.age      = t->age;
  s.stop     = &t->stop;
  s.now      = desk_ms;
  s.poll     = think_poll;
  s.report   = think_report;
  s.arg      = c;
  s.nhist    = t->nhist;
  memcpy(s.hist, t->hist, t->nhist * sizeof(s.hist[0]));

  m = ch_search(&s);

  pthread_mutex_lock(&t->lock);
  t->best = m;
  t->info = s.info;
  t->done = true;
  pthread_mutex_unlock(&t->lock);
  desk_wake(c->w);
  return NULL;
}

/* Wait for the thread to end (it stops within a thousand nodes) */

static void think_end(struct chess_s *c, bool discard)
{
  if (c->think.running)
    {
      c->think.discard |= discard;
      c->think.stop = true;
      pthread_join(c->think.thread, NULL);
      c->think.running = false;
    }

  c->thinking = false;
}

static void think_start(struct chess_s *c)
{
  const struct level_s *lv = &g_levels[c->level];
  struct think_s *t = &c->think;
  struct game_s *g = &c->g;
  struct sched_param sp;
  pthread_attr_t attr;
  int first;
  int i;

  think_end(c, true);
  t->pos     = g->pos;
  t->depth   = lv->depth;
  t->ms      = lv->ms;
  t->noise   = lv->noise;
  t->seed    = c->seed ^ (uint32_t)g->n * 0x9e3779b9u;
  t->age     = ++c->age;
  t->stop    = false;
  t->discard = false;
  t->done    = false;
  t->news    = false;
  t->polls   = 0;

  /* The positions since the last capture or pawn move, for repetitions */

  first = g->n - g->pos.halfmove;
  first = first < 0 ? 0 : first;
  first = g->n - first > 128 ? g->n - 128 : first;
  t->nhist = 0;
  for (i = first; i < g->n; i++)
    {
      t->hist[t->nhist++] = g->hash[i];
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, sizeof(struct ch_search_s) +
                            CONFIG_FPCDESK_CHESS_STACKSIZE);
  sp.sched_priority = CONFIG_FPCDESK_CHESS_PRIORITY;
  pthread_attr_setschedparam(&attr, &sp);
  if (pthread_create(&t->thread, &attr, think_main, c) == 0)
    {
      pthread_setname_np(t->thread, "chess");
      t->running = true;
      c->thinking = true;
    }

  pthread_attr_destroy(&attr);
}

/* The computer's turn: think */

static void maybe_think(struct chess_s *c)
{
  if (!c->thinking && c->g.result == RES_NONE && !human_turn(c))
    {
      think_start(c);
    }

  panel_update(c);
  kit_flush(&c->kit);
}

/****************************************************************************
 * Private Functions: moves
 ****************************************************************************/

static void position_changed(struct chess_s *c)
{
  c->nlegal   = ch_legal(&c->g.pos, c->legal);
  c->g.result = game_result(&c->g, c->nlegal);
  c->sel      = -1;
  c->promo[0] = CH_NOMOVE;
  c->dragging = false;
  c->press    = -1;
}

static void game_over_ask(struct chess_s *c)
{
  static const char * const buttons[] =
  {
    "New Game", "Close"
  };

  static const char * const titles[] =
  {
    "", "Checkmate", "Stalemate", "Draw", "Draw", "Draw", "Draw"
  };

  const char *what;

  switch (c->g.result)
    {
      case RES_MATE:
        what = c->g.pos.side == CH_WHITE ? "Black wins." : "White wins.";
        break;

      case RES_STALEMATE:
        what = "The game is drawn: no legal move, not in check.";
        break;

      case RES_FIFTY:
        what = "Fifty moves without a capture or a pawn move.";
        break;

      case RES_REPEAT:
        what = "The same position three times.";
        break;

      case RES_MATERIAL:
        what = "Neither side can mate.";
        break;

      default:
        what = "The game is too long to go on.";
        break;
    }

  kit_dialog(&c->kit, DLG_OVER, KIT_DLG_CONFIRM, titles[c->g.result], what,
             buttons, 2);
}

/* Play m (legal) on the board: what changed is drawn again */

static void play(struct chess_s *c, ch_move_t m)
{
  uint64_t dirty = sel_squares(c) | king_squares(c) | move_squares(c, m);

  if (c->g.n > 0)
    {
      dirty |= move_squares(c, c->g.move[c->g.n - 1]);
    }

  game_play(&c->g, m);
  position_changed(c);
  dirty |= king_squares(c);
  board_update(c, dirty, 0, 0, 0, 0);
  if (c->g.result != RES_NONE)
    {
      panel_update(c);
      kit_flush(&c->kit);
      game_over_ask(c);
      kit_flush(&c->kit);
      return;
    }

  maybe_think(c);
}

/* The player's move from s to t: played, or the promotion asked */

static bool try_move(struct chess_s *c, int from, int to)
{
  ch_move_t found[4];
  int n = 0;
  int i;

  for (i = 0; i < c->nlegal && n < 4; i++)
    {
      if (CH_FROM(c->legal[i]) == sq88(from) &&
          CH_TO(c->legal[i]) == sq88(to))
        {
          found[n++] = c->legal[i];
        }
    }

  if (n == 0)
    {
      return false;
    }

  if (n == 1)
    {
      play(c, found[0]);
      return true;
    }

  /* A promotion: ask which, in the order Q, N, R, B */

  for (i = 0; i < n; i++)
    {
      int p = CH_PROMO(found[i]);

      c->promo[p == CH_QUEEN ? 0 : p == CH_KNIGHT ? 1 : p == CH_ROOK ? 2 :
               3] = found[i];
    }

  c->promohot = -1;
  kit_invalidate(&c->kit, W_BOARD);
  kit_flush(&c->kit);
  return true;
}

static void select_sq(struct chess_s *c, int s)
{
  uint64_t dirty = sel_squares(c);

  c->sel = s;
  dirty |= sel_squares(c);
  board_update(c, dirty, 0, 0, 0, 0);
}

/****************************************************************************
 * Private Functions: the pointer on the board
 ****************************************************************************/

static bool movable(struct chess_s *c, int s)
{
  int pc = s >= 0 ? c->g.pos.board[sq88(s)] : 0;

  return pc != CH_EMPTY && CH_SIDE(pc) == c->g.pos.side &&
         human_turn(c) && !c->thinking && c->g.result == RES_NONE;
}

static void drag_to(struct chess_s *c, int x, int y)
{
  int ox = c->dragx;
  int oy = c->dragy;
  int hover = sq_at(c, x, y);
  uint64_t dirty = 0;

  c->dragx = x - c->sq / 2;
  c->dragy = y - c->sq / 2;
  if (hover != c->hover)
    {
      if (c->hover >= 0)
        {
          dirty |= 1ull << c->hover;
        }

      if (hover >= 0)
        {
          dirty |= 1ull << hover;
        }

      c->hover = hover;
    }

  /* Where it was (its squares, cut to it), then where it is */

  board_update(c, dirty, ox, oy, c->sq, c->sq);
  board_update(c, 0, c->dragx, c->dragy, c->sq, c->sq);
}

static void board_mouse(struct chess_s *c, int x, int y, uint8_t buttons)
{
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(c->buttons & NX_MOUSE_LEFTBUTTON);
  bool release = !(buttons & NX_MOUSE_LEFTBUTTON) &&
                 (c->buttons & NX_MOUSE_LEFTBUTTON);
  int s = sq_at(c, x, y);

  c->buttons = buttons;

  /* Choosing a promotion */

  if (c->promo[0] != CH_NOMOVE)
    {
      int item = s >= 0 ? promo_item(c, s) : -1;

      if (press)
        {
          ch_move_t m = item >= 0 ? c->promo[item] : CH_NOMOVE;

          c->promo[0] = CH_NOMOVE;
          c->sel = -1;
          kit_invalidate(&c->kit, W_BOARD);
          if (m != CH_NOMOVE)
            {
              play(c, m);
            }

          kit_flush(&c->kit);
        }
      else if (item != c->promohot)
        {
          c->promohot = item;
          kit_invalidate(&c->kit, W_BOARD);
          kit_flush(&c->kit);
        }

      return;
    }

  if (press)
    {
      if (c->sel >= 0 && s >= 0 && is_target(c, s))
        {
          try_move(c, c->sel, s);
          return;
        }

      if (movable(c, s))
        {
          c->wassel = c->sel == s;
          c->press  = s;
          c->pressx = x;
          c->pressy = y;
          c->hover  = s;
          if (c->sel != s)
            {
              select_sq(c, s);
            }
        }
      else if (c->sel >= 0)
        {
          select_sq(c, -1);
        }

      return;
    }

  if ((buttons & NX_MOUSE_LEFTBUTTON) && c->press >= 0)
    {
      if (!c->dragging && (abs(x - c->pressx) > DRAG_START ||
                           abs(y - c->pressy) > DRAG_START))
        {
          c->dragging = true;
          c->dragx = x - c->sq / 2;
          c->dragy = y - c->sq / 2;
          board_update(c, 1ull << c->press, 0, 0, 0, 0);
        }

      if (c->dragging)
        {
          drag_to(c, x, y);
        }

      return;
    }

  if (release || (buttons == 0 && c->press >= 0))
    {
      int from = c->press;

      c->press = -1;
      if (from < 0)
        {
          return;
        }

      if (c->dragging)
        {
          int dx = c->dragx;
          int dy = c->dragy;
          uint64_t dirty = 1ull << from;

          c->dragging = false;
          if (c->hover >= 0)
            {
              dirty |= 1ull << c->hover;
            }

          c->hover = -1;
          if (s >= 0 && s != from && try_move(c, from, s))
            {
              board_update(c, 0, dx, dy, c->sq, c->sq);
              return;
            }

          board_update(c, dirty, dx, dy, c->sq, c->sq);
          return;
        }

      if (s == from && c->wassel)
        {
          select_sq(c, -1);
        }
    }
}

/****************************************************************************
 * Private Functions: commands
 ****************************************************************************/

static void redraw_all(struct chess_s *c)
{
  kit_invalidate(&c->kit, W_BOARD);
  names_set(c);
  panel_update(c);
}

static void new_game(struct chess_s *c, const struct ch_pos_s *start)
{
  think_end(c, true);
  game_new(&c->g, start);
  position_changed(c);
  ch_tt_clear(c->tt, c->ttn);
  info_set(c, NULL, 0);
  redraw_all(c);
  maybe_think(c);
}

/* Back (or on) a move, or to the player's turn against the computer */

static void step(struct chess_s *c, int dir)
{
  struct game_s *g = &c->g;
  bool was = c->thinking;

  think_end(c, true);
  if (dir < 0)
    {
      if (g->n == 0)
        {
          return;
        }

      game_goto(g, g->n - 1);
      while (!was && c->mode != MODE_TWO && g->n > 0 && !human_turn(c))
        {
          game_goto(g, g->n - 1);
        }
    }
  else
    {
      if (g->total <= g->n)
        {
          return;
        }

      game_goto(g, g->n + 1);
      while (c->mode != MODE_TWO && g->n < g->total && !human_turn(c))
        {
          game_goto(g, g->n + 1);
        }
    }

  position_changed(c);
  info_set(c, NULL, 0);
  kit_invalidate(&c->kit, W_BOARD);

  /* Back to the computer's turn at the start: it moves again, but not
   * after a Redo that left off there
   */

  if (dir < 0 || g->n == g->total)
    {
      maybe_think(c);
    }
  else
    {
      panel_update(c);
      kit_flush(&c->kit);
    }
}

static void load_fen(struct chess_s *c, const char *fen)
{
  struct ch_pos_s p;

  if (!ch_setfen(&p, fen))
    {
      kit_dialog(&c->kit, 0, KIT_DLG_MESSAGE, "Not a position",
                 "The FEN could not be read, or the position is not "
                 "legal.", NULL, 0);
      kit_flush(&c->kit);
      return;
    }

  new_game(c, &p);
}

static void copy_moves(struct chess_s *c)
{
  struct game_s *g = &c->g;
  char *buf = malloc(GAME_MAX * 12 + 16);
  int len = 0;
  int i;

  if (buf == NULL)
    {
      return;
    }

  buf[0] = '\0';
  for (i = 0; i < g->n; i++)
    {
      int ply = i + (g->start.side == CH_BLACK);

      if (ply % 2 == 0 || i == 0)
        {
          len += sprintf(buf + len, "%s%d.%s ", i > 0 ? " " : "",
                         g->start.fullmove + ply / 2,
                         ply % 2 == 1 ? ".." : "");
        }
      else
        {
          buf[len++] = ' ';
        }

      len += sprintf(buf + len, "%s", g->san[i]);
    }

  desk_clip_set(buf, len);
  free(buf);
}

static void chess_event(struct kit_s *k, struct kit_widget_s *wg, int ev)
{
  struct chess_s *c = k->priv;
  int id = wg != NULL ? wg->id : 0;

  switch (ev)
    {
      case KIT_EV_CLICK:
        switch (id)
          {
            case W_NEW:
              {
                struct ch_pos_s p;

                ch_setfen(&p, g_start);
                new_game(c, &p);
              }
              break;

            case W_UNDO:
              step(c, -1);
              break;

            case W_REDO:
              step(c, 1);
              break;

            case W_FLIP:
              c->flip = !c->flip;
              redraw_all(c);
              break;

            case W_STOP:
              c->think.stop = true;
              break;

            case W_MENU:
              kit_menu(k, MENU_MAIN, wg->x + wg->w - 220, wg->y + wg->h + 2,
                       g_menu, 3, 0);
              break;

            default:
              break;
          }
        break;

      case KIT_EV_CHANGE:
        if (id == W_MODE)
          {
            c->mode = wg->value;
            if (c->mode != MODE_TWO)
              {
                c->flip = c->mode == MODE_BLACK;
              }

            /* The computer may be thinking for the side that is now the
             * player's
             */

            if (c->thinking && human_turn(c))
              {
                think_end(c, true);
              }

            redraw_all(c);
            maybe_think(c);
          }
        else if (id == W_LEVEL)
          {
            c->level = wg->value;
            names_set(c);
          }
        else if (id == W_MOVES)
          {
            int off = c->g.start.side == CH_BLACK;

            kit_list_select(k, W_MOVES, (c->g.n + off + 1) / 2 - 1);
          }
        break;

      case KIT_EV_MENU:
        if (k->arg == 0)
          {
            ch_getfen(&c->g.pos, c->answer, sizeof(c->answer));
            kit_dialog(k, DLG_FEN, KIT_DLG_PROMPT, "Position (FEN)",
                       c->answer, NULL, 0);
          }
        else if (k->arg == 1)
          {
            ch_getfen(&c->g.pos, c->answer, sizeof(c->answer));
            desk_clip_set(c->answer, strlen(c->answer));
          }
        else if (k->arg == 2)
          {
            copy_moves(c);
          }
        break;

      case KIT_EV_DIALOG:
        if (k->key == DLG_FEN && k->arg == KIT_ANS_OK)
          {
            load_fen(c, k->answer);
          }
        else if (k->key == DLG_OVER && k->arg == KIT_ANS_OK)
          {
            struct ch_pos_s p;

            ch_setfen(&p, g_start);
            new_game(c, &p);
          }
        break;

      case KIT_EV_MOUSE:
        if (id == W_BOARD)
          {
            board_mouse(c, k->mx, k->my, k->mbuttons);
          }
        break;

      case KIT_EV_KEY:
        if (kit_dialog_open(k))
          {
            break;
          }

        if (k->key == KEY_ESC)
          {
            if (c->promo[0] != CH_NOMOVE)
              {
                c->promo[0] = CH_NOMOVE;
                c->sel = -1;
                kit_invalidate(k, W_BOARD);
              }
            else if (c->sel >= 0)
              {
                select_sq(c, -1);
              }

            k->key = 0;
          }
        else if (k->key == 'f' || k->key == 'F')
          {
            c->flip = !c->flip;
            redraw_all(c);
            k->key = 0;
          }
        else if (k->key == KEY_CTL('l'))
          {
            ch_getfen(&c->g.pos, c->answer, sizeof(c->answer));
            kit_dialog(k, DLG_FEN, KIT_DLG_PROMPT, "Position (FEN)",
                       c->answer, NULL, 0);
            k->key = 0;
          }
        else if (k->key == ' ' && c->thinking)
          {
            c->think.stop = true;
            k->key = 0;
          }
        break;

      default:
        break;
    }
}

static void chess_draw_cb(struct kit_s *k, struct kit_widget_s *wg, int x0,
                          int y0, int x1, int y1)
{
  struct chess_s *c = k->priv;

  switch (wg->id)
    {
      case W_BOARD:
        board_geometry(c);
        board_paint(c, x0, y0, x1, y1);
        break;

      case W_TOPCAPS:
      case W_BOTCAPS:
        if (c->made == 0 || c->surf.lost)
          {
            board_geometry(c);
            surface_make(c);
          }

        caps_draw(c, wg, (wg->id == W_BOTCAPS) != c->flip ? CH_WHITE :
                  CH_BLACK);
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static void chess_build(struct chess_s *c)
{
  struct kit_s *k = &c->kit;
  struct kit_widget_s *wg;
  int i;

  for (i = 0; i < NLEVELS; i++)
    {
      g_levelnames[i] = g_levels[i].name;
    }

  kit_init(k, c->w, c->wg, NWG, chess_event);
  k->priv = c;
  k->draw = chess_draw_cb;

  kit_col(k, KIT_BOX_WINDOW);
    kit_row(k, KIT_BOX_BAR);
      kit_button(k, W_NEW, "New Game", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_tip(k, "A new game (F2)");
      kit_button(k, W_UNDO, "Undo", KIT_ICON_BACK, KIT_BTN_NORMAL);
      kit_tip(k, "Take back a move (Ctrl-Z)");
      kit_button(k, W_REDO, "Redo", KIT_ICON_FORWARD, KIT_BTN_NORMAL);
      kit_tip(k, "Play it again (Ctrl-Y)");
      kit_button(k, W_FLIP, "Flip", KIT_ICON_REFRESH, KIT_BTN_NORMAL);
      kit_tip(k, "Turn the board around (F)");
      kit_spacer(k, 1);
      kit_dropdown(k, W_MODE, g_modes, 3, c->mode);
      kit_tip(k, "Who plays");
      kit_dropdown(k, W_LEVEL, g_levelnames, NLEVELS, c->level);
      kit_tip(k, "How well the computer plays");
      kit_button(k, W_STOP, "Stop", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_tip(k, "Move now (Space)");
      kit_button(k, W_MENU, NULL, KIT_ICON_MENU, KIT_BTN_FLAT);
      kit_tip(k, "Positions and moves");
    kit_end(k);

    wg = kit_row(k, KIT_BOX_NONE);
    wg->padx = KIT_PAD;
    wg->pady = KIT_PAD;
    wg->gap  = KIT_PAD + 4;
    kit_grow(k, 1);
      kit_custom(k, W_BOARD);
      kit_grow(k, 1);

      wg = kit_col(k, KIT_BOX_NONE);
      wg->gap = KIT_GAP + 2;
      kit_size(k, SIDE_W, 0);
        wg = kit_col(k, KIT_BOX_CARD);
        wg->gap = 4;
          kit_label(k, W_TOPNAME, c->topname, KIT_TEXT_BOLD);
          kit_custom(k, W_TOPCAPS);
          kit_size(k, 0, SMALL + 4);
        kit_end(k);
        kit_list(k, W_MOVES, &c->list, KIT_LIST_TABLE);
        kit_grow(k, 1);
        wg = kit_col(k, KIT_BOX_CARD);
        wg->gap = 4;
          kit_custom(k, W_BOTCAPS);
          kit_size(k, 0, SMALL + 4);
          kit_label(k, W_BOTNAME, c->botname, KIT_TEXT_BOLD);
        kit_end(k);
      kit_end(k);
    kit_end(k);

    kit_row(k, KIT_BOX_STATUS);
      kit_label(k, W_STATUS, c->status, KIT_TEXT_BODY);
      kit_grow(k, 1);
      kit_label(k, W_INFO, c->info, KIT_TEXT_DIM);
    kit_end(k);
  kit_end(k);

  kit_shortcut(k, KEY_CTL('z'), W_UNDO);
  kit_shortcut(k, KEY_CTL('y'), W_REDO);
  kit_shortcut(k, KEY_F2, W_NEW);
}

static void chess_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  struct chess_s *c = w->priv;

  if (c != NULL)
    {
      kit_expose(&c->kit, x, y, cw, ch);
    }
}

static void chess_redraw(struct desk_win_s *w)
{
  struct chess_s *c = w->priv;

  if (c != NULL)
    {
      kit_redraw(&c->kit);
    }
}

static void chess_resize(struct desk_win_s *w)
{
  struct chess_s *c = w->priv;

  if (c != NULL)
    {
      c->dragging = false;
      c->press = -1;
      kit_resize(&c->kit);
    }
}

static void chess_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct chess_s *c = w->priv;

  if (c != NULL)
    {
      kit_mouse(&c->kit, x, y, buttons);
    }
}

static void chess_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct chess_s *c = w->priv;

  if (c != NULL)
    {
      kit_key(&c->kit, ch, n);
    }
}

static void chess_tick(struct desk_win_s *w)
{
  struct chess_s *c = w->priv;

  if (c != NULL)
    {
      kit_tick(&c->kit);
    }
}

static void chess_focus(struct desk_win_s *w)
{
  struct chess_s *c = w->priv;

  if (c != NULL)
    {
      kit_activate(&c->kit);
    }
}

/* The engine has news (a depth, or its move), or the surface lost its
 * pixels
 */

static void chess_wake(struct desk_win_s *w)
{
  struct chess_s *c = w->priv;
  struct think_s *t;
  struct ch_info_s info;
  ch_move_t best = CH_NOMOVE;
  bool done;
  bool news;

  if (c == NULL)
    {
      return;
    }

  if (c->surf.lost && c->made != 0)
    {
      surface_make(c);
      kit_invalidate(&c->kit, W_BOARD);
      kit_invalidate(&c->kit, W_TOPCAPS);
      kit_invalidate(&c->kit, W_BOTCAPS);
    }

  t = &c->think;
  pthread_mutex_lock(&t->lock);
  done = t->done && t->running;
  news = t->news;
  info = t->info;
  best = t->best;
  t->news = false;
  pthread_mutex_unlock(&t->lock);

  if (news || done)
    {
      info_set(c, &info, c->g.pos.side);
    }

  if (done)
    {
      bool discard = t->discard;

      pthread_join(t->thread, NULL);
      t->running  = false;
      c->thinking = false;
      if (!discard && best != CH_NOMOVE && c->promo[0] == CH_NOMOVE)
        {
          play(c, best);
          return;
        }

      panel_update(c);
    }

  kit_flush(&c->kit);
}

static void chess_close(struct desk_win_s *w)
{
  struct chess_s *c = w->priv;

  if (c != NULL)
    {
      think_end(c, true);
      pthread_mutex_destroy(&c->think.lock);
      kit_dialog_close(&c->kit);
      kit_surface_close(&c->surf);
      free(c->tt);
      c->tt = NULL;
    }
}

/* The icon: a pawn on a board, on walnut.  The pawn's parts are drawn
 * twice, a pixel larger in dark first: its outline.
 */

static void icon_pawn(struct desk_ops_s *o, int cx, int y, int s, int e,
                      uint16_t c, uint8_t a)
{
  int head = s >= 80 ? 20 : s >= 56 ? 16 : 14;
  int lut = s >= 56 ? LUT_R6 : LUT_R3;
  int r = s >= 56 ? 6 : 3;
  int bw = s * 13 / 32;
  int nw = s * 3 / 16;
  int w2 = s * 9 / 32;

  ops_disc_atlas(o, cx - head / 2 - e, y + s * 5 / 32 - e, head + 2 * e, c,
                 a);
  ops_rrect(o, cx - nw / 2 - e, y + s * 13 / 32 - e, nw + 2 * e,
            s * 7 / 32 + 2 * e, LUT_R3, 3, c, a);
  ops_rrect(o, cx - w2 / 2 - e, y + s * 18 / 32 - e, w2 + 2 * e,
            s * 6 / 32 + 2 * e, lut, r, c, a);
  ops_rrect(o, cx - s / 8 - e, y + s * 12 / 32 - e, s / 4 + 2 * e,
            s / 16 + 2 * e, LUT_R3, 3, c, a);
  ops_rrect(o, cx - bw / 2 - e, y + s * 23 / 32 - e, bw + 2 * e,
            s * 3 / 32 + 2 * e, LUT_R3, 3, c, a);
}

static void chess_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  int b = s * 11 / 16 & ~3;
  int q = b / 4;
  int bx = x + (s - b) / 2;
  int by = y + (s - b) / 2;
  int i;

  icon_tile(o, x, y, s, RGB(0x9c, 0x6a, 0x44), RGB(0x58, 0x34, 0x1e), a);
  for (i = 0; i < 16; i++)
    {
      ops_blend(o, bx + (i % 4) * q, by + (i / 4) * q, q, q,
                (i + i / 4) & 1 ? RGB(0xb5, 0x88, 0x63) :
                                  RGB(0xf0, 0xd9, 0xb5), a);
    }

  ops_rrect(o, x + s / 2 - s * 7 / 32, y + s * 25 / 32, s * 7 / 16, s / 16,
            LUT_R3, 3, C_BLACK, (uint8_t)(a / 3));
  icon_pawn(o, x + s / 2, y, s, 1, RGB(0x26, 0x23, 0x1f), a);
  icon_pawn(o, x + s / 2, y, s, 0, RGB(0xfa, 0xfa, 0xf6), a);
}

static struct desk_win_s *chess_open(void)
{
  struct desk_win_s *w;
  struct chess_s *c;
  struct ch_pos_s p;

  c = calloc(1, sizeof(*c));
  if (c == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_CHESS, "Chess", CHESS_W, CHESS_H);
  if (w == NULL)
    {
      free(c);
      return NULL;
    }

  w->priv   = c;
  c->w      = w;
  c->mode   = MODE_WHITE;
  c->level  = 2;
  c->sel    = -1;
  c->press  = -1;
  c->hover  = -1;
  c->seed   = desk_seed() * 2654435761u;
  pthread_mutex_init(&c->think.lock, NULL);

  /* The table: in the heap (the PSRAM), smaller if need be */

  c->ttn = CONFIG_FPCDESK_CHESS_TT_KB * 1024 / CH_TT_BYTES(1);
  while (c->ttn > 1024 && (c->tt = calloc(c->ttn, CH_TT_BYTES(1))) == NULL)
    {
      c->ttn /= 2;
    }

  if (c->tt == NULL)
    {
      c->ttn = 0;
    }

  c->list.sortcol = -1;
  c->list.ncols   = 3;
  c->list.cols    = g_cols;
  c->list.text    = moves_text;
  c->list.sel     = -1;
  c->list.empty   = "No moves yet";

  ch_init();
  ch_setfen(&p, g_start);
  game_new(&c->g, &p);
  position_changed(c);

  chess_build(c);
  names_set(c);
  kit_layout(&c->kit);
  board_geometry(c);
  panel_update(c);
  kit_flush(&c->kit);
  return w;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_chess =
{
  .name   = "Chess",
  .icon   = chess_icon,
  .open   = chess_open,
  .draw   = chess_redraw,
  .expose = chess_expose,
  .resize = chess_resize,
  .tick   = chess_tick,
  .mouse  = chess_mouse,
  .key    = chess_key,
  .focus  = chess_focus,
  .wake   = chess_wake,
  .close  = chess_close,
};

#endif /* CONFIG_FPCDESK_CHESS */
