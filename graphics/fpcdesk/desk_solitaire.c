/****************************************************************************
 * apps/graphics/fpcdesk/desk_solitaire.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Solitaire: Klondike.  Seven columns to build down in alternate colours
 * (a King on an empty one), four foundations to build up by suit from the
 * Ace, and the stock, dealt one or three at a time (the bar's button; it
 * deals a new game) as often as it takes.
 *
 * Drag a card, or a column's cards from a face-up one down, with the
 * mouse; double-click a card to send it to its foundation; click the stock
 * to deal.  Undo takes back any move, all the way to the deal.  Keys:
 * Ctrl-Z undo, F2 a new game, Space deals.  When all the cards are home
 * they bounce off the foundations, one by one, leaving their trails, as
 * they always have.
 *
 * Drawing: the suits (drawn as shapes: a heart is the curve
 * (x^2 + y^2 - 1)^3 = x^2 y^3, a club three discs, ...) and the back's
 * pattern are drawn once with GPU operations into a hidden surface; a card
 * is then a shadow, a round white rectangle, its rank from the font and
 * one or two COPYs of suits: about seven operations.  The table is drawn
 * again only where it changes: the columns a move touches, and under a
 * dragged stack, the part of the table it leaves (clipped), then the stack
 * where it is now.
 *
 * A program of its own (/mnt/sd0/bin/solitaire, apps/graphics/deskapps):
 * built into the desktop (DESK_PROGRAM_STUB) this file is only the icon
 * and the name the dock shows (desk_program.c).
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

#define CARD_W          72
#define CARD_H          100
#define SOL_GAP         12
#define SOL_MARGIN      16
#define SOL_TOP         16
#define SOL_BAR_H       44
#define SOL_W           (2 * SOL_MARGIN + 7 * CARD_W + 6 * SOL_GAP)
#define SOL_H           600
#define SOL_DOWN_DY     8               /* Face-down cards in a column */
#define SOL_UP_DY       24              /* Face-up cards, at most */
#define SOL_UP_MIN      12
#define SOL_FAN_DX      16              /* The waste, dealt three at a time */
#define SOL_UNDO_MAX    512
#define SOL_CLICK_MS    400

/* Piles */

#define P_STOCK         0
#define P_WASTE         1
#define P_FOUND         2               /* 2-5 */
#define P_TABLEAU       6               /* 6-12 */
#define P_N             13

/* Cards: rank 0-12 (Ace to King), suit 0-3 (clubs, diamonds, hearts,
 * spades: red are 1 and 2), face up
 */

#define CARD(r, s)      ((uint8_t)((r) | (s) << 4))
#define RANK(c)         ((c) & 0x0f)
#define SUIT(c)         (((c) >> 4) & 3)
#define UP              0x80
#define ISRED(c)        (SUIT(c) == 1 || SUIT(c) == 2)

/* The surface: the back's panel, then the suits in three sizes */

#define SPR_BACK_W      (CARD_W - 8)
#define SPR_BACK_H      (CARD_H - 8)
#define SPR_SUIT_X      (SPR_BACK_W + 4)
#define SPR_SUIT_DX     50
#define SPR_HUGE        44
#define SPR_BIG         28
#define SPR_SMALL       14
#define SPR_HUGE_Y      0
#define SPR_BIG_Y       (SPR_HUGE + 2)
#define SPR_SMALL_Y     (SPR_BIG_Y + SPR_BIG + 2)
#define SPR_W           (SPR_SUIT_X + 4 * SPR_SUIT_DX)
#define SPR_H           SPR_BACK_H

#define C_FELT0         RGB(0x1c, 0x6a, 0x3c)
#define C_FELT1         RGB(0x0c, 0x40, 0x22)
#define C_FACE          RGB(0xfc, 0xfc, 0xfa)
#define C_EDGE          RGB(0x98, 0x98, 0xa0)
#define C_RED           RGB(0xd4, 0x22, 0x2e)
#define C_BLACKSUIT     RGB(0x1e, 0x1e, 0x26)
#define C_BACK0         RGB(0x3c, 0x6c, 0xd8)
#define C_BACK1         RGB(0x1c, 0x34, 0x8c)

/* The bar's buttons */

#define B_NEW           1
#define B_UNDO          2
#define B_DRAW          3

/* Moves, for undo */

#define M_MOVE          0               /* n cards from -> to */
#define M_DEAL          1               /* n cards stock -> waste */
#define M_REDEAL        2               /* n cards waste -> stock */
#define M_TURN          3               /* A column's top turned up */

#ifndef DESK_PROGRAM_STUB

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sol_pile_s
{
  uint8_t n;
  uint8_t c[52];
};

struct sol_move_s
{
  uint8_t kind;                         /* M_* */
  uint8_t from;
  uint8_t to;
  uint8_t n;
  bool turned;                          /* from's new top was turned up */
};

struct sol_rect_s
{
  int x;
  int y;
  int w;
  int h;
};

struct sol_bounce_s
{
  int x;                                /* In 1/16 pixel */
  int y;
  int vx;
  int vy;
  uint8_t card;
  bool live;
};

struct sol_s
{
  /* The game */

  struct sol_pile_s pile[P_N];
  int draw;                             /* 1 or 3 */
  struct sol_move_s undo[SOL_UNDO_MAX];
  int nundo;
  unsigned int moves;
  uint32_t seconds;                     /* Played so far */
  bool started;                         /* The clock runs */
  bool won;
  uint32_t seed;

  /* The layout */

  int cw;
  int ch;
  int x0;                               /* The table's left, frame coords */
  int y0;
  int bottom;                           /* The bar's top */

  /* The pointer */

  uint8_t buttons;
  int presspile;                        /* The press: a pile and a card */
  int pressidx;
  int pressx;
  int pressy;
  uint32_t clickms;                     /* The last click, for double ones */
  int clickpile;
  int clickidx;

  /* A dragged stack: its cards, where it came from, where it is */

  struct sol_pile_s drag;
  int dragfrom;                         /* -1: none */
  int grabx;                            /* The pointer within its top card */
  int graby;
  struct sol_rect_s dragrect;           /* Where it is drawn */

  /* Won: cards bouncing off the foundations */

  struct sol_bounce_s bounce;
  int bouncefound;                      /* The foundation to take the next */
  int bounced;                          /* Cards gone */
  uint32_t lastframe;

  /* The bar */

  struct ui_zones_s zones;
  struct ui_keys_s keys;
  char drawnstatus[48];

  struct desk_ops_s o;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static NXWINDOW g_sol_sprite;
static volatile bool g_sol_lost;
static bool g_sol_made;

static const char * const g_sol_rank[13] =
{
  "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"
};

/****************************************************************************
 * Private Functions: the suits, as shapes
 ****************************************************************************/

struct sol_shape_s
{
  int suit;
  int w;                                /* The box, pixels */
  int h;
};

/* The heart's curve, (x^2 + y^2 - 1)^3 <= x^2 y^3, over about -1.15 to
 * 1.15 wide, -1 to 1.25 high
 */

static bool sol_heart(float x, float y)
{
  float a = x * x + y * y - 1.0f;

  return a * a * a - x * x * y * y * y <= 0.0f;
}

/* The stem of the spade and the club: a narrow triangle to the bottom */

static bool sol_stem(float u, float v)
{
  float half = 0.035f + (v - 0.55f) * 0.42f;
  float d = u - 0.5f;

  return v >= 0.55f && v <= 1.0f && d <= half && -d <= half;
}

static bool sol_disc(float u, float v, float cx, float cy, float r)
{
  return (u - cx) * (u - cx) + (v - cy) * (v - cy) <= r * r;
}

static bool sol_suit_inside(void *arg, int x, int y)
{
  struct sol_shape_s *sh = arg;
  float u = x / (16.0f * sh->w);
  float v = y / (16.0f * sh->h);

  switch (sh->suit)
    {
      case 0:                           /* Clubs */
        return sol_disc(u, v, 0.5f, 0.25f, 0.215f) ||
               sol_disc(u, v, 0.26f, 0.56f, 0.215f) ||
               sol_disc(u, v, 0.74f, 0.56f, 0.215f) ||
               sol_disc(u, v, 0.5f, 0.5f, 0.13f) || sol_stem(u, v);

      case 1:                           /* Diamonds */
        {
          float du = u - 0.5f;
          float dv = v - 0.5f;

          du = du < 0 ? -du : du;
          dv = dv < 0 ? -dv : dv;
          return du / 0.40f + dv / 0.5f <= 1.0f;
        }

      case 2:                           /* Hearts */
        return sol_heart((u - 0.5f) * 2.36f, 1.2f - v * 2.3f);

      default:                          /* Spades: a heart upside down */
        return (v <= 0.8f && sol_heart((u - 0.5f) * 2.36f,
                                       v * 2.9f - 1.25f)) ||
               sol_stem(u, v);
    }
}

/* The back's pattern: a lattice of small diamonds */

static bool sol_lattice_inside(void *arg, int x, int y)
{
  int p = 8 * 16;
  int a = (x + y) % p;
  int b = (x - y + 64 * p) % p;

  a = a > p / 2 ? p - a : a;
  b = b > p / 2 ? p - b : b;
  return a < 12 || b < 12;
}

/****************************************************************************
 * Private Functions: the surface
 ****************************************************************************/

static void sprite_make(struct sol_s *s)
{
  struct desk_ops_s *o = &s->o;
  struct sol_shape_s sh;
  int i;

  if (g_sol_sprite == NULL)
    {
      g_sol_sprite = desk_surface_open(SPR_W, SPR_H, &g_sol_lost);
      if (g_sol_sprite == NULL)
        {
          return;
        }
    }

  g_sol_lost = false;
  ops_reset(o);
  ops_fill(o, 0, 0, SPR_W, SPR_H, C_FACE);

  /* The back's panel: a gradient, the lattice, a light edge */

  ops_rrect_vgrad(o, 0, 0, SPR_BACK_W, SPR_BACK_H, LUT_R3, 3, C_BACK0,
                  C_BACK1, 255);
  ops_draw(g_sol_sprite, o);
  ops_shape(g_sol_sprite, o, 3, 3, SPR_BACK_W - 6, SPR_BACK_H - 6, C_WHITE,
            34, sol_lattice_inside, NULL);
  ops_draw(g_sol_sprite, o);
  ops_blend(o, 3, 2, SPR_BACK_W - 6, 1, C_WHITE, 70);

  /* The suits */

  for (i = 0; i < 4; i++)
    {
      uint16_t c = i == 1 || i == 2 ? C_RED : C_BLACKSUIT;
      int x = SPR_SUIT_X + i * SPR_SUIT_DX;

      sh.suit = i;
      sh.w = sh.h = SPR_HUGE;
      ops_shape(g_sol_sprite, o, x, SPR_HUGE_Y, SPR_HUGE, SPR_HUGE, c, 255,
                sol_suit_inside, &sh);
      sh.w = sh.h = SPR_BIG;
      ops_shape(g_sol_sprite, o, x, SPR_BIG_Y, SPR_BIG, SPR_BIG, c, 255,
                sol_suit_inside, &sh);
      sh.w = sh.h = SPR_SMALL;
      ops_shape(g_sol_sprite, o, x, SPR_SMALL_Y, SPR_SMALL, SPR_SMALL, c,
                255, sol_suit_inside, &sh);
    }

  ops_draw(g_sol_sprite, o);
  g_sol_made = true;
}

/****************************************************************************
 * Private Functions: the game
 ****************************************************************************/

static uint32_t sol_rand(struct sol_s *s)
{
  s->seed = s->seed * 1103515245 + 12345;
  return s->seed >> 16;
}

static void sol_deal(struct sol_s *s)
{
  uint8_t deck[52];
  int i;
  int j;
  int k = 0;

  for (i = 0; i < 52; i++)
    {
      deck[i] = CARD(i % 13, i / 13);
    }

  s->seed ^= desk_seed();
  for (i = 51; i > 0; i--)
    {
      uint8_t t;

      j = sol_rand(s) % (i + 1);
      t = deck[i];
      deck[i] = deck[j];
      deck[j] = t;
    }

  memset(s->pile, 0, sizeof(s->pile));
  for (i = 0; i < 7; i++)
    {
      for (j = 0; j <= i; j++)
        {
          s->pile[P_TABLEAU + i].c[j] = deck[k++] | (j == i ? UP : 0);
        }

      s->pile[P_TABLEAU + i].n = i + 1;
    }

  for (i = 0; k < 52; i++)
    {
      s->pile[P_STOCK].c[i] = deck[k++];
    }

  s->pile[P_STOCK].n = i;
  s->nundo    = 0;
  s->moves    = 0;
  s->seconds  = 0;
  s->started  = false;
  s->won      = false;
  s->dragfrom = -1;
  s->bounced  = 0;
  s->bounce.live = false;
}

static uint8_t sol_top(struct sol_s *s, int p)
{
  struct sol_pile_s *pl = &s->pile[p];

  return pl->n > 0 ? pl->c[pl->n - 1] : 0;
}

/* Whether cards (first: the bottom one of them) may go on pile p */

static bool sol_can(struct sol_s *s, int p, uint8_t first, int n)
{
  struct sol_pile_s *pl = &s->pile[p];
  uint8_t top = sol_top(s, p);

  if (p >= P_FOUND && p < P_TABLEAU)
    {
      if (n != 1)
        {
          return false;
        }

      return pl->n == 0 ? RANK(first) == 0 :
             SUIT(top) == SUIT(first) && RANK(first) == RANK(top) + 1;
    }

  if (p >= P_TABLEAU)
    {
      return pl->n == 0 ? RANK(first) == 12 :
             (top & UP) && ISRED(top) != ISRED(first) &&
             RANK(first) + 1 == RANK(top);
    }

  return false;
}

static void sol_record(struct sol_s *s, int kind, int from, int to, int n,
                       bool turned)
{
  struct sol_move_s *m;

  if (s->nundo == SOL_UNDO_MAX)
    {
      memmove(s->undo, s->undo + 1, sizeof(s->undo[0]) * (SOL_UNDO_MAX - 1));
      s->nundo--;
    }

  m = &s->undo[s->nundo++];
  m->kind   = kind;
  m->from   = from;
  m->to     = to;
  m->n      = n;
  m->turned = turned;
  s->moves++;
  s->started = true;
}

/* A column's top face down after a move: it turns up */

static bool sol_turn(struct sol_s *s, int p)
{
  struct sol_pile_s *pl = &s->pile[p];

  if (p >= P_TABLEAU && pl->n > 0 && !(pl->c[pl->n - 1] & UP))
    {
      pl->c[pl->n - 1] |= UP;
      return true;
    }

  return false;
}

static void sol_append(struct sol_pile_s *to, const uint8_t *c, int n)
{
  memcpy(to->c + to->n, c, n);
  to->n += n;
}

static bool sol_won(struct sol_s *s)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      if (s->pile[P_FOUND + i].n != 13)
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Private Functions: where things are
 ****************************************************************************/

static int sol_col_x(struct sol_s *s, int col)
{
  return s->x0 + col * (CARD_W + SOL_GAP);
}

static void sol_slot(struct sol_s *s, int p, int *x, int *y)
{
  int col = p == P_STOCK ? 0 : p == P_WASTE ? 1 :
            p < P_TABLEAU ? 3 + p - P_FOUND : p - P_TABLEAU;

  *x = sol_col_x(s, col);
  *y = p < P_TABLEAU ? s->y0 : s->y0 + CARD_H + 18;
}

/* The step between a column's face-up cards: less when it is long */

static int sol_up_dy(struct sol_s *s, int p)
{
  struct sol_pile_s *pl = &s->pile[p];
  int x;
  int y;
  int downs = 0;
  int ups;
  int room;
  int i;

  if (p < P_TABLEAU || pl->n < 2)
    {
      return SOL_UP_DY;
    }

  sol_slot(s, p, &x, &y);
  for (i = 0; i < pl->n - 1 && !(pl->c[i] & UP); i++)
    {
      downs++;
    }

  ups  = pl->n - 1 - downs;
  room = s->bottom - 8 - y - CARD_H - downs * SOL_DOWN_DY;
  if (ups == 0 || ups * SOL_UP_DY <= room)
    {
      return SOL_UP_DY;
    }

  i = room / ups;
  return i < SOL_UP_MIN ? SOL_UP_MIN : i;
}

/* Where card i of pile p is */

static void sol_card_pos(struct sol_s *s, int p, int i, int *x, int *y)
{
  struct sol_pile_s *pl = &s->pile[p];
  int k;

  sol_slot(s, p, x, y);
  if (p >= P_TABLEAU)
    {
      int up = sol_up_dy(s, p);

      for (k = 0; k < i; k++)
        {
          *y += pl->c[k] & UP ? up : SOL_DOWN_DY;
        }
    }
  else if (p == P_WASTE && s->draw == 3)
    {
      /* The last three fan out */

      int fan = i - (pl->n - 3);

      *x += fan > 0 ? fan * SOL_FAN_DX : 0;
    }
}

/* All of a pile, where it is drawn now */

static struct sol_rect_s sol_pile_rect(struct sol_s *s, int p)
{
  struct sol_rect_s r;
  int x;
  int y;

  sol_slot(s, p, &r.x, &r.y);
  r.w = CARD_W;
  r.h = CARD_H;
  if (s->pile[p].n > 0)
    {
      sol_card_pos(s, p, s->pile[p].n - 1, &x, &y);
      r.w = x + CARD_W - r.x;
      r.h = y + CARD_H - r.y;
    }

  if (p >= P_TABLEAU)
    {
      r.h = s->bottom - r.y;           /* Its column, to the bottom */
    }

  r.h += 4;                             /* The shadow */
  return r;
}

static bool sol_in(int x, int y, int rx, int ry, int rw, int rh)
{
  return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* The card under (x, y) (frame coordinates): its pile and index (-1: the
 * pile's empty slot); false if none
 */

static bool sol_hit(struct sol_s *s, int x, int y, int *pp, int *pi)
{
  int p;
  int i;

  for (p = 0; p < P_N; p++)
    {
      struct sol_pile_s *pl = &s->pile[p];
      int cx;
      int cy;

      for (i = pl->n - 1; i >= 0; i--)
        {
          sol_card_pos(s, p, i, &cx, &cy);
          if (sol_in(x, y, cx, cy, CARD_W, CARD_H))
            {
              *pp = p;
              *pi = i;
              return true;
            }
        }

      sol_slot(s, p, &cx, &cy);
      if (sol_in(x, y, cx, cy, CARD_W, CARD_H))
        {
          *pp = p;
          *pi = -1;
          return true;
        }
    }

  return false;
}

/****************************************************************************
 * Private Functions: drawing helpers (cards, the felt, the bar)
 ****************************************************************************/

static void sol_copy(struct desk_ops_s *o, int x, int y, int w, int h,
                     int sx, int sy)
{
  struct nx_gpuop_s *op = ops_add(o);

  op->op  = NX_GPUOP_COPY;
  op->x   = x;
  op->y   = y;
  op->w   = w;
  op->h   = h;
  op->src = g_sol_sprite;
  op->sx  = sx;
  op->sy  = sy;
}

/* A card: its shadow (lift: a lifted card's, larger; < 0: none), a round
 * white rectangle with an edge, then its face (rank and suits) or its
 * back
 */

static void card_draw(struct desk_ops_s *o, int x, int y, uint8_t c,
                      int lift)
{
  int suit = SUIT(c);
  int rank = RANK(c);
  int sx = SPR_SUIT_X + suit * SPR_SUIT_DX;
  uint16_t col = ISRED(c) ? C_RED : C_BLACKSUIT;
  const struct desk_font_s *f = &g_desk_font_title;

  if (lift >= 0)
    {
      ops_rrect(o, x - lift / 2, y + 2 + lift, CARD_W + lift, CARD_H,
                LUT_R6, 6, C_BLACK, lift > 0 ? 60 : 44);
    }

  ops_rrect(o, x, y, CARD_W, CARD_H, LUT_R6, 6, C_EDGE, 255);
  ops_rrect(o, x + 1, y + 1, CARD_W - 2, CARD_H - 2, LUT_R6, 6, C_FACE,
            255);

  if (!(c & UP))
    {
      sol_copy(o, x + 4, y + 4, SPR_BACK_W, SPR_BACK_H, 0, 0);
      return;
    }

  /* The rank at the top left, the suit at the top right */

  ops_text(o, f, x + 6, y + 3, g_sol_rank[rank], col, 255);
  sol_copy(o, x + CARD_W - 6 - SPR_SMALL, y + 7, SPR_SMALL, SPR_SMALL,
           sx, SPR_SMALL_Y);

  if (rank == 0)
    {
      sol_copy(o, x + (CARD_W - SPR_HUGE) / 2, y + 36, SPR_HUGE, SPR_HUGE,
               sx, SPR_HUGE_Y);
    }
  else if (rank >= 10)
    {
      /* Court cards: the letter, large, in a tinted frame */

      ops_rrect(o, x + 9, y + 30, CARD_W - 18, CARD_H - 38, LUT_R6, 6, col,
                28);
      ops_text_centre(o, &g_desk_font_big, x + CARD_W / 2, y + 29,
                      g_sol_rank[rank], col, 255);
      sol_copy(o, x + (CARD_W - SPR_SMALL) / 2, y + CARD_H - 12 - SPR_SMALL,
               SPR_SMALL, SPR_SMALL, sx, SPR_SMALL_Y);
    }
  else
    {
      sol_copy(o, x + (CARD_W - SPR_BIG) / 2, y + 46, SPR_BIG, SPR_BIG, sx,
               SPR_BIG_Y);
    }
}

/* The felt: a gradient over the table, the same wherever it is cut */

static void felt_draw(struct sol_s *s, struct desk_ops_s *o, int x, int y,
                      int w, int h)
{
  struct nx_gpuop_s *op;
  int len = s->bottom - TITLE_H;

  ops_fill(o, x, y, w, h, C_FELT0);
  op = ops_add(o);
  op->op     = NX_GPUOP_BLEND;
  op->x      = x;
  op->y      = y;
  op->w      = w;
  op->h      = h;
  op->colour = C_FELT1;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_DITHER | NX_BLEND_MOD_VRAMP;
  op->p[0]   = TITLE_H;
  op->p[1]   = len > 1 ? (255 * 256) / (len - 1) : 0;
  op->p[2]   = 0;
}

/* An empty place: a darker round rectangle with a light edge */

static void slot_draw(struct desk_ops_s *o, int x, int y,
                      const struct desk_font_s *f, const char *label)
{
  ops_rrect(o, x, y, CARD_W, CARD_H, LUT_R6, 6, C_WHITE, 40);
  ops_rrect(o, x + 2, y + 2, CARD_W - 4, CARD_H - 4, LUT_R6, 6, C_BLACK,
            60);
  if (label != NULL)
    {
      ops_text_centre(o, f, x + CARD_W / 2,
                      y + (CARD_H - text_height(f)) / 2, label, C_WHITE,
                      90);
    }
}

/* The bar at the bottom: New Game, Undo, Draw 1 / 3; moves and time */

static void bar_status(struct sol_s *s, char *buf, size_t size)
{
  snprintf(buf, size, "Moves %u   %u:%02u", s->moves,
           (unsigned)(s->seconds / 60), (unsigned)(s->seconds % 60));
}

static void bar_draw(struct sol_s *s, struct desk_ops_s *o, bool all,
                     bool buttons)
{
  static const char * const labels[3] =
  {
    "New Game", "Undo", NULL
  };

  int y = s->bottom;
  int x = 12;
  int i;
  char buf[48];

  if (all)
    {
      ops_fill(o, 0, y, s->cw, SOL_BAR_H, C_UI_BAR);
      ops_fill(o, 0, y, s->cw, 1, C_UI_LINE);
    }

  if (buttons)
    {
      ui_zones_clear(&s->zones, 1);
    }

  for (i = 0; i < 3; i++)
    {
      const char *label = labels[i] != NULL ? labels[i] :
                          s->draw == 3 ? "Draw 3" : "Draw 1";
      int bw = ui_button_width(label);
      bool enabled = i != 1 || s->nundo > 0;

      if (buttons)
        {
          ui_zone_add(&s->zones, 1, B_NEW + i, x, y + 8 - TITLE_H, bw, 28,
                      enabled);
          ops_fill(o, x, y + 8, bw, 28, C_UI_BAR);
          ui_button(o, x, y + 8, bw, 28, label,
                    ui_look(&s->zones, B_NEW + i, enabled));
        }

      x += bw + 8;
    }

  bar_status(s, buf, sizeof(buf));
  ops_fill(o, x, y + 8, s->cw - 12 - x, 28, C_UI_BAR);
  ops_text(o, &g_desk_font_ui, s->cw - 12 - text_width(&g_desk_font_ui,
           buf), y + (SOL_BAR_H - text_height(&g_desk_font_ui)) / 2, buf,
           C_TEXT_DIM, 255);
  strlcpy(s->drawnstatus, buf, sizeof(s->drawnstatus));
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static bool rect_meet(const struct sol_rect_s *a, int x, int y, int w,
                      int h)
{
  return a->x < x + w && x < a->x + a->w && a->y < y + h && y < a->y + a->h;
}

/* One element, clipped to r, sent when the list is full */

static void sol_clip(NXWINDOW hwnd, struct desk_ops_s *o, int from,
                     const struct sol_rect_s *r)
{
  ui_clip(o, from, r->x, r->y, r->x + r->w, r->y + r->h);
  ui_flush(hwnd, o, 24);
}

/* The table within r: the felt, the empty places, every card that shows
 * there (not a dragged stack)
 */

static void sol_paint(struct desk_win_s *w, struct sol_rect_s r)
{
  struct sol_s *s = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  struct desk_ops_s *o = &s->o;
  int from;
  int p;
  int i;

  /* Within the table */

  if (r.y < TITLE_H)
    {
      r.h -= TITLE_H - r.y;
      r.y  = TITLE_H;
    }

  if (r.y + r.h > s->bottom)
    {
      r.h = s->bottom - r.y;
    }

  if (r.x < 0)
    {
      r.w += r.x;
      r.x  = 0;
    }

  if (r.x + r.w > s->cw)
    {
      r.w = s->cw - r.x;
    }

  if (r.w <= 0 || r.h <= 0)
    {
      return;
    }

  ui_flush(hwnd, o, 24);
  felt_draw(s, o, r.x, r.y, r.w, r.h);

  for (p = 0; p < P_N; p++)
    {
      struct sol_pile_s *pl = &s->pile[p];
      int first = 0;
      int x;
      int y;

      sol_slot(s, p, &x, &y);
      if (rect_meet(&r, x, y, CARD_W, CARD_H + 4) &&
          (pl->n == 0 || p == P_STOCK))
        {
          from = o->n;
          if (p == P_STOCK)
            {
              slot_draw(o, x, y, &g_desk_font_small,
                        s->pile[P_WASTE].n > 0 ? "Deal again" : NULL);
            }
          else
            {
              slot_draw(o, x, y, &g_desk_font_title,
                        p >= P_FOUND && p < P_TABLEAU ? "A" : NULL);
            }
          sol_clip(hwnd, o, from, &r);
        }

      /* The stock and the foundations: only the top shows; the waste:
       * the top three
       */

      if (p < P_TABLEAU && pl->n > 0)
        {
          first = p == P_WASTE && s->draw == 3 ? pl->n - 3 : pl->n - 1;
          first = first < 0 ? 0 : first;
        }

      for (i = first; i < pl->n; i++)
        {
          sol_card_pos(s, p, i, &x, &y);
          if (!rect_meet(&r, x - 4, y, CARD_W + 8, CARD_H + 8))
            {
              continue;
            }

          from = o->n;
          card_draw(o, x, y, pl->c[i], 0);
          sol_clip(hwnd, o, from, &r);
        }
    }
}

/* The dragged stack, where it is */

static struct sol_rect_s drag_rect(struct sol_s *s, int x, int y)
{
  struct sol_rect_s r;
  int i;
  int h = CARD_H;

  for (i = 0; i + 1 < s->drag.n; i++)
    {
      h += SOL_UP_DY;
    }

  r.x = x - 4;
  r.y = y;
  r.w = CARD_W + 8;
  r.h = h + 10;
  return r;
}

static void drag_draw(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  struct desk_ops_s *o = &s->o;
  struct sol_rect_s table;
  int x = s->dragrect.x + 4;
  int y = s->dragrect.y;
  int from;
  int i;

  table.x = 0;
  table.y = TITLE_H;
  table.w = s->cw;
  table.h = s->bottom - TITLE_H;
  for (i = 0; i < s->drag.n; i++)
    {
      from = o->n;
      card_draw(o, x, y + i * SOL_UP_DY, s->drag.c[i], i == 0 ? 6 : 0);
      sol_clip(hwnd, o, from, &table);
    }
}

/* Draw these parts of the table again */

static void sol_repaint(struct desk_win_s *w, const struct sol_rect_s *r,
                        int n)
{
  struct sol_s *s = w->priv;
  int i;

  for (i = 0; i < n; i++)
    {
      sol_paint(w, r[i]);
    }

  if (s->dragfrom >= 0)
    {
      drag_draw(w);
    }

  ops_draw((NXWINDOW)w->hwnd, &s->o);
}

static void sol_bar(struct desk_win_s *w, bool all, bool buttons)
{
  struct sol_s *s = w->priv;

  ui_flush((NXWINDOW)w->hwnd, &s->o, 128);
  bar_draw(s, &s->o, all, buttons);
  ops_draw((NXWINDOW)w->hwnd, &s->o);
}

static void sol_layout(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;
  int tw = 7 * CARD_W + 6 * SOL_GAP;

  s->cw     = w->csize.w;
  s->ch     = w->csize.h;
  s->x0     = s->cw > tw + 2 * SOL_MARGIN ? (s->cw - tw) / 2 : SOL_MARGIN;
  s->y0     = TITLE_H + SOL_TOP;
  s->bottom = TITLE_H + s->ch - SOL_BAR_H;
}

static void sol_draw(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;
  struct sol_rect_s all;

  if (s->cw != w->csize.w || s->ch != w->csize.h)
    {
      sol_layout(w);
    }

  if (!g_sol_made || g_sol_lost)
    {
      sprite_make(s);
    }

  all.x = 0;
  all.y = TITLE_H;
  all.w = s->cw;
  all.h = s->bottom - TITLE_H;
  ops_reset(&s->o);
  sol_repaint(w, &all, 1);
  sol_bar(w, true, true);
}

/* A change to piles a and b (-1: none): their places are drawn again,
 * as they were (before) and are (after)
 */

struct sol_dirty_s
{
  struct sol_rect_s r[6];
  int n;
};

static void dirty_pile(struct sol_s *s, struct sol_dirty_s *d, int p)
{
  if (p >= 0 && d->n < 6)
    {
      d->r[d->n++] = sol_pile_rect(s, p);
    }
}

static void sol_after(struct desk_win_s *w, struct sol_dirty_s *d, int a,
                      int b)
{
  struct sol_s *s = w->priv;

  dirty_pile(s, d, a);
  dirty_pile(s, d, b);
  sol_repaint(w, d->r, d->n);
  sol_bar(w, false, true);
}

/* Moves */

static bool sol_move(struct desk_win_s *w, int from, int idx, int to)
{
  struct sol_s *s = w->priv;
  struct sol_pile_s *pf = &s->pile[from];
  struct sol_dirty_s d;
  int n = pf->n - idx;
  bool turned;

  if (n <= 0 || !sol_can(s, to, pf->c[idx], n))
    {
      return false;
    }

  d.n = 0;
  dirty_pile(s, &d, from);
  dirty_pile(s, &d, to);
  sol_append(&s->pile[to], pf->c + idx, n);
  pf->n = idx;
  turned = sol_turn(s, from);
  sol_record(s, M_MOVE, from, to, n, turned);
  sol_after(w, &d, from, to);
  return true;
}

static void sol_deal_stock(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;
  struct sol_pile_s *st = &s->pile[P_STOCK];
  struct sol_pile_s *wa = &s->pile[P_WASTE];
  struct sol_dirty_s d;
  int n = 0;

  d.n = 0;
  dirty_pile(s, &d, P_STOCK);
  dirty_pile(s, &d, P_WASTE);
  if (st->n == 0)
    {
      /* The waste back, face down */

      if (wa->n == 0)
        {
          return;
        }

      while (wa->n > 0)
        {
          st->c[st->n++] = wa->c[--wa->n] & ~UP;
          n++;
        }

      sol_record(s, M_REDEAL, P_WASTE, P_STOCK, n, false);
    }
  else
    {
      while (n < s->draw && st->n > 0)
        {
          wa->c[wa->n++] = st->c[--st->n] | UP;
          n++;
        }

      sol_record(s, M_DEAL, P_STOCK, P_WASTE, n, false);
    }

  sol_after(w, &d, P_STOCK, P_WASTE);
}

static void sol_undo(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;
  struct sol_move_s *m;
  struct sol_pile_s *pf;
  struct sol_pile_s *pt;
  struct sol_dirty_s d;
  int i;

  if (s->nundo == 0 || s->won)
    {
      return;
    }

  m  = &s->undo[--s->nundo];
  pf = &s->pile[m->from];
  pt = &s->pile[m->to];
  d.n = 0;
  dirty_pile(s, &d, m->from);
  dirty_pile(s, &d, m->to);

  switch (m->kind)
    {
      case M_MOVE:
        if (m->turned)
          {
            pf->c[pf->n - 1] &= ~UP;
          }

        sol_append(pf, pt->c + pt->n - m->n, m->n);
        pt->n -= m->n;
        break;

      case M_TURN:
        pf->c[pf->n - 1] &= ~UP;
        break;

      case M_DEAL:
      case M_REDEAL:
        for (i = 0; i < m->n; i++)
          {
            uint8_t c = pt->c[--pt->n];

            pf->c[pf->n++] = m->kind == M_DEAL ? c & ~UP : c | UP;
          }
        break;

      default:
        break;
    }

  s->moves++;
  sol_after(w, &d, m->from, m->to);
}

/* Where a dragged stack is dropped: the pile it covers most that takes
 * it
 */

static int sol_target(struct sol_s *s)
{
  int best = -1;
  int bestarea = 0;
  int p;

  for (p = P_FOUND; p < P_N; p++)
    {
      struct sol_rect_s r;
      int x;
      int y;
      int ox;
      int oy;

      if (p == s->dragfrom || !sol_can(s, p, s->drag.c[0], s->drag.n))
        {
          continue;
        }

      if (s->pile[p].n > 0)
        {
          sol_card_pos(s, p, s->pile[p].n - 1, &x, &y);
        }
      else
        {
          sol_slot(s, p, &x, &y);
        }

      r.x = x;
      r.y = y;
      r.w = CARD_W;
      r.h = CARD_H + (p >= P_TABLEAU ? SOL_UP_DY : 0);
      ox = (r.x + r.w < s->dragrect.x + 4 + CARD_W ? r.x + r.w :
            s->dragrect.x + 4 + CARD_W) -
           (r.x > s->dragrect.x + 4 ? r.x : s->dragrect.x + 4);
      oy = (r.y + r.h < s->dragrect.y + CARD_H ? r.y + r.h :
            s->dragrect.y + CARD_H) -
           (r.y > s->dragrect.y ? r.y : s->dragrect.y);
      if (ox > 0 && oy > 0 && ox * oy > bestarea)
        {
          bestarea = ox * oy;
          best = p;
        }
    }

  return best;
}

static void sol_drop(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;
  struct sol_dirty_s d;
  int to = sol_target(s);
  int from = s->dragfrom;
  bool turned = false;

  d.n = 0;
  d.r[d.n++] = s->dragrect;
  s->dragfrom = -1;
  if (to < 0)
    {
      /* Back where it came from */

      sol_append(&s->pile[from], s->drag.c, s->drag.n);
      sol_after(w, &d, from, -1);
      return;
    }

  dirty_pile(s, &d, to);
  sol_append(&s->pile[to], s->drag.c, s->drag.n);
  turned = sol_turn(s, from);
  sol_record(s, M_MOVE, from, to, s->drag.n, turned);
  sol_after(w, &d, from, to);
}

/* A card to its foundation, if it can go */

static bool sol_home(struct desk_win_s *w, int p)
{
  struct sol_s *s = w->priv;
  int i;

  if (s->pile[p].n == 0 || !(sol_top(s, p) & UP) || (p >= P_FOUND &&
                                                     p < P_TABLEAU))
    {
      return false;
    }

  for (i = 0; i < 4; i++)
    {
      if (sol_move(w, p, s->pile[p].n - 1, P_FOUND + i))
        {
          return true;
        }
    }

  return false;
}

/* All home: the cards bounce away */

static void sol_check_won(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;

  if (!s->won && sol_won(s))
    {
      s->won         = true;
      s->started     = false;
      s->bounced     = 0;
      s->bouncefound = 3;
      s->bounce.live = false;
      s->lastframe   = desk_ms();
      w->frames      = true;
    }
}

static void sol_new(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;

  w->frames = false;
  sol_deal(s);
  sol_draw(w);
}

/****************************************************************************
 * Private Functions: the win
 ****************************************************************************/

/* A message over the table */

static void sol_message(struct desk_win_s *w, const char *title,
                        const char *hint)
{
  struct sol_s *s = w->priv;
  struct desk_ops_s *o = &s->o;
  int bw = 300;
  int bh = 96;
  int x = (s->cw - bw) / 2;
  int y = TITLE_H + (s->bottom - TITLE_H - bh) / 2;

  ops_rrect(o, x, y + 4, bw, bh, LUT_R12, 12, C_BLACK, 80);
  ops_rrect(o, x, y, bw, bh, LUT_R12, 12, RGB(0x24, 0x24, 0x2a), 240);
  ops_text_centre(o, &g_desk_font_title, x + bw / 2, y + 22, title,
                  C_TEXT, 255);
  ops_text_centre(o, &g_desk_font_ui, x + bw / 2, y + 58, hint,
                  C_TEXT_DIM, 255);
  ops_draw((NXWINDOW)w->hwnd, o);
}

/* A frame of the bounce: the card moves on (in steps of at most a few
 * pixels, so that its trail is whole), drawn where it gets to and never
 * taken away
 */

static void sol_bounce_frame(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;
  struct sol_bounce_s *b = &s->bounce;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  struct desk_ops_s *o = &s->o;
  struct sol_rect_s table;
  int floor = (s->bottom - CARD_H) * 16;
  int steps;
  int i;

  table.x = 0;
  table.y = TITLE_H;
  table.w = s->cw;
  table.h = s->bottom - TITLE_H;

  if (!b->live)
    {
      struct sol_pile_s *f;
      struct sol_dirty_s d;
      int k;
      int x;
      int y;

      /* The next card: the top of the next foundation that has one */

      for (k = 0; k < 4; k++)
        {
          f = &s->pile[P_FOUND + (s->bouncefound + 4 - k) % 4];
          if (f->n > 0)
            {
              break;
            }
        }

      if (k == 4)
        {
          w->frames = false;
          sol_message(w, "You won!", "Click for a new game");
          return;
        }

      s->bouncefound = (s->bouncefound + 4 - k) % 4;
      sol_card_pos(s, P_FOUND + s->bouncefound, f->n - 1, &x, &y);
      b->card = f->c[f->n - 1];
      b->x    = x * 16;
      b->y    = y * 16;
      b->vx   = (sol_rand(s) % 2 ? 1 : -1) * (48 + sol_rand(s) % 96);
      b->vy   = -(int)(sol_rand(s) % 128);
      b->live = true;
      s->bouncefound = (s->bouncefound + 3) % 4;

      d.n = 0;
      dirty_pile(s, &d, P_FOUND + (s->bouncefound + 1) % 4);
      f->n--;
      sol_repaint(w, d.r, d.n);
    }

  /* A frame's worth: 16 ms; steps of at most 4 pixels */

  steps = (b->vx < 0 ? -b->vx : b->vx) / 64 + 1;
  steps = steps < 1 ? 1 : steps;
  for (i = 0; i < steps; i++)
    {
      b->x  += b->vx / steps;
      b->y  += b->vy / steps;
      b->vy += 10 / steps + (i < 10 % steps ? 1 : 0);
      if (b->y > floor)
        {
          b->y  = floor;
          b->vy = -b->vy * 3 / 4;
        }

      ui_flush(hwnd, o, 24);
      {
        int from = o->n;

        card_draw(o, b->x / 16, b->y / 16, b->card | UP, -1);
        sol_clip(hwnd, o, from, &table);
      }
    }

  ops_draw(hwnd, o);
  if (b->x / 16 + CARD_W < 0 || b->x / 16 > s->cw)
    {
      b->live = false;
      s->bounced++;
    }
}

static void sol_frame(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;

  if (s->won)
    {
      sol_bounce_frame(w);
    }
  else
    {
      w->frames = false;
    }
}

/****************************************************************************
 * Private Functions: input
 ****************************************************************************/

static void sol_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct sol_s *s = w->priv;
  bool down = (buttons & NX_MOUSE_LEFTBUTTON) != 0;
  bool press = down && !(s->buttons & NX_MOUSE_LEFTBUTTON);
  bool release = !down && (s->buttons & NX_MOUSE_LEFTBUTTON);
  struct ui_zone_s *z = ui_zone_at(&s->zones, x, y);
  int fy = y + TITLE_H;
  int hot = z != NULL && z->enabled ? z->id : 0;
  int p;
  int i;

  s->buttons = buttons;

  /* The bar's buttons (not while dragging a card) */

  if (down && !press && s->zones.pressed == 0)
    {
      hot = 0;
    }

  if (hot != s->zones.hot || (press && hot != 0) ||
      (release && s->zones.pressed != 0))
    {
      int pressed = s->zones.pressed;

      s->zones.hot = hot;
      if (press)
        {
          s->zones.pressed = hot;
        }

      if (release)
        {
          s->zones.pressed = 0;
        }

      sol_bar(w, false, true);
      if (release && pressed != 0 && pressed == hot)
        {
          switch (hot)
            {
              case B_NEW:
                sol_new(w);
                break;

              case B_UNDO:
                sol_undo(w);
                break;

              case B_DRAW:
                s->draw = s->draw == 1 ? 3 : 1;
                sol_new(w);
                break;

              default:
                break;
            }

          return;
        }
    }

  if (s->zones.pressed != 0 || (press && hot != 0))
    {
      return;
    }

  /* Won: a click deals again */

  if (s->won)
    {
      if (press)
        {
          sol_new(w);
        }

      return;
    }

  if (press)
    {
      s->presspile = -1;
      if (!sol_hit(s, x, fy, &p, &i))
        {
          return;
        }

      if (p == P_STOCK)
        {
          sol_deal_stock(w);
          return;
        }

      /* A face-up card that can be moved: it may be dragged */

      if (i >= 0 && (s->pile[p].c[i] & UP) &&
          (p >= P_TABLEAU || i == s->pile[p].n - 1))
        {
          s->presspile = p;
          s->pressidx  = i;
          s->pressx    = x;
          s->pressy    = fy;
        }
      else if (i >= 0 && p >= P_TABLEAU && i == s->pile[p].n - 1)
        {
          /* A face-down top turns up */

          struct sol_dirty_s d;

          d.n = 0;
          sol_turn(s, p);
          sol_record(s, M_TURN, p, p, 0, true);
          sol_after(w, &d, p, -1);
        }

      return;
    }

  if (down && s->presspile >= 0 && s->dragfrom < 0 &&
      (x - s->pressx) * (x - s->pressx) +
      (fy - s->pressy) * (fy - s->pressy) > 16)
    {
      /* Lift the cards */

      struct sol_pile_s *pl = &s->pile[s->presspile];
      int cx;
      int cy;

      sol_card_pos(s, s->presspile, s->pressidx, &cx, &cy);
      s->drag.n = pl->n - s->pressidx;
      memcpy(s->drag.c, pl->c + s->pressidx, s->drag.n);
      s->dragfrom = s->presspile;
      s->grabx    = s->pressx - cx;
      s->graby    = s->pressy - cy;

      /* Where the stack was: the table without it */

      s->dragrect = drag_rect(s, cx, cy);
      {
        struct sol_rect_s was = sol_pile_rect(s, s->dragfrom);

        pl->n = s->pressidx;
        sol_paint(w, was);
      }
    }

  if (down && s->dragfrom >= 0)
    {
      /* The stack follows the pointer: the table again where it was,
       * then the stack where it is
       */

      struct sol_rect_s was = s->dragrect;

      s->dragrect = drag_rect(s, x - s->grabx, fy - s->graby);
      sol_repaint(w, &was, 1);
      return;
    }

  if (release)
    {
      if (s->dragfrom >= 0)
        {
          sol_drop(w);
          sol_check_won(w);
          return;
        }

      /* A click; twice: the card goes home */

      if (s->presspile >= 0)
        {
          uint32_t now = desk_ms();

          if (s->clickpile == s->presspile && s->clickidx == s->pressidx &&
              now - s->clickms < SOL_CLICK_MS)
            {
              s->clickpile = -1;
              if (s->pressidx == s->pile[s->presspile].n - 1)
                {
                  sol_home(w, s->presspile);
                  sol_check_won(w);
                }
            }
          else
            {
              s->clickpile = s->presspile;
              s->clickidx  = s->pressidx;
              s->clickms   = now;
            }
        }

      s->presspile = -1;
    }
}

static void sol_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct sol_s *s = w->priv;
  int keys[16];
  int nk = ui_keys(&s->keys, ch, n, keys, 16);
  int i;

  for (i = 0; i < nk; i++)
    {
      switch (keys[i])
        {
          case KEY_CTL('z'):
          case 0x7f:
          case 0x08:
            if (s->dragfrom < 0)
              {
                sol_undo(w);
              }
            break;

          case KEY_F2:
          case KEY_CTL('n'):
            sol_new(w);
            break;

          case ' ':
            if (s->dragfrom < 0 && !s->won)
              {
                sol_deal_stock(w);
              }
            break;

          default:
            break;
        }
    }
}

/* The clock: a second of play (not while the desktop's clock is frozen,
 * for tests)
 */

static void sol_tick(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;
  char buf[48];

  if (s->started && w->active && !desk_frozen())
    {
      s->seconds++;
    }

  bar_status(s, buf, sizeof(buf));
  if (strcmp(buf, s->drawnstatus) != 0)
    {
      sol_bar(w, false, false);
    }
}

static void sol_focus(struct desk_win_s *w)
{
  struct sol_s *s = w->priv;

  /* The bounce stops while the window is in the background */

  if (s->won && s->bounced < 52)
    {
      w->frames = w->active;
    }
}

static void sol_resize(struct desk_win_s *w)
{
  sol_layout(w);
  sol_draw(w);
}

static void sol_close(struct desk_win_s *w)
{
  w->frames = false;
  desk_surface_close(g_sol_sprite);
  g_sol_sprite = NULL;
  g_sol_made   = false;
}

#endif /* !DESK_PROGRAM_STUB */

/* The icon: two cards, fanned */

static void sol_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  int cw = s * 2 / 5;
  int ch = s * 11 / 20;
  int cy = y + (s - ch) / 2;
  const struct desk_font_s *f = s >= 64 ? &g_desk_font_title :
                                          &g_desk_font_uib;

  icon_tile(o, x, y, s, RGB(0x2e, 0x9a, 0x58), RGB(0x10, 0x5a, 0x30), a);
  ops_rrect(o, x + s / 5, cy - s / 20, cw, ch, LUT_R3, 3, C_BLACK,
            (uint8_t)(a / 5));
  ops_rrect(o, x + s / 5, cy - s / 16, cw, ch, LUT_R3, 3, C_BACK0, a);
  ops_rrect(o, x + s / 5 + 2, cy - s / 16 + 2, cw - 4, ch - 4, LUT_R3, 3,
            C_BACK1, a);
  ops_rrect(o, x + s * 2 / 5, cy + s / 16 + 1, cw, ch, LUT_R3, 3, C_BLACK,
            (uint8_t)(a / 4));
  ops_rrect(o, x + s * 2 / 5, cy + s / 16, cw, ch, LUT_R3, 3, C_FACE, a);
  ops_text(o, f, x + s * 2 / 5 + 3, cy + s / 16 + 1, "A", C_RED, a);
}

#ifndef DESK_PROGRAM_STUB
static struct desk_win_s *sol_open(void)
{
  struct desk_win_s *w;
  struct sol_s *s;

  s = calloc(1, sizeof(*s));
  if (s == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_SOLITAIRE, "Solitaire", SOL_W, SOL_H);
  if (w == NULL)
    {
      free(s);
      return NULL;
    }

  w->priv      = s;
  s->draw      = 1;
  s->seed      = desk_seed() * 2654435761u;
  s->clickpile = -1;
  s->presspile = -1;
  sol_deal(s);
  sol_layout(w);
  sprite_make(s);
  sol_draw(w);
  return w;
}

#endif /* !DESK_PROGRAM_STUB */

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef DESK_PROGRAM_STUB
DESK_PROGRAM_APP(g_app_solitaire, "Solitaire", sol_icon, "solitaire", true);
#else
const struct desk_app_s g_app_solitaire =
{
  .name   = "Solitaire",
  .icon   = sol_icon,
  .open   = sol_open,
  .draw   = sol_draw,
  .resize = sol_resize,
  .tick   = sol_tick,
  .mouse  = sol_mouse,
  .key    = sol_key,
  .close  = sol_close,
  .frame  = sol_frame,
  .focus  = sol_focus,
  .single = true,
};
#endif
