/****************************************************************************
 * apps/graphics/fpcdesk/desk_chess_engine.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chess's rules and engine (desk_chess.h).
 *
 * The board: 0x88 with a list of each side's pieces, so moves are made
 * and taken back incrementally, with the hash (Zobrist keys, worked out
 * from the piece and square by a mixing function instead of kept in a
 * table) and the material and piece-square sums (middle game and end
 * game).  Attacks are found with the 0x88 difference table: which kinds
 * of piece can reach a square from another, and in which direction.
 *
 * The search: iterative deepening, aspiration windows, principal
 * variation search with a transposition table (12-byte entries, in a table
 * the caller gives), null-move pruning, late-move reductions, reverse
 * futility pruning, check extensions and mate distance pruning; the moves
 * ordered by the table's move, captures by MVV-LVA, two killers a ply and
 * the history of quiet moves that caused cut-offs; quiescence over
 * captures and promotions, with delta pruning.  Draws by repetition, the
 * fifty-move rule and material are seen in the search.
 *
 * The evaluation: material and piece-square tables tapered from the
 * middle game to the end game by the material left; pawns (doubled,
 * isolated, passed), the bishop pair, rooks on open files, the king's
 * pawn shield and open files near it, and in won endings without pawns,
 * the losing king driven to the edge.  Optional noise (weaker levels):
 * a pseudo-random amount by position and seed, the same every time the
 * position is seen, so the search stays consistent.
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk_chess.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TT_EXACT        1
#define TT_LOWER        2                /* score >= beta */
#define TT_UPPER        3                /* score <= alpha */

#define S_TTMOVE        32000
#define S_CAPTURE       20000
#define S_KILLER1       19000
#define S_KILLER2       18900
#define S_HISTMAX       16000

#define POLL_NODES      1024

/* The attack table's bits: which pieces can go from one square to
 * another in one move, if nothing is in the way
 */

#define A_WPAWN         0x01
#define A_BPAWN         0x02
#define A_KNIGHT        0x04
#define A_BISHOP        0x08
#define A_ROOK          0x10
#define A_QUEEN         0x20
#define A_KING          0x40

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const int8_t g_knight[8] =
{
  33, 31, 18, 14, -14, -18, -31, -33
};

static const int8_t g_king[8] =
{
  17, 16, 15, 1, -1, -15, -16, -17
};

static const int8_t g_diag[4] =
{
  17, 15, -15, -17
};

static const int8_t g_orth[4] =
{
  16, 1, -1, -16
};

/* What a kind of piece is worth: middle game, end game, for ordering */

static const int16_t g_mgval[7] =
{
  0, 90, 320, 335, 480, 960, 0
};

static const int16_t g_egval[7] =
{
  0, 120, 300, 320, 520, 940, 0
};

static const int16_t g_value[7] =
{
  0, 100, 320, 330, 500, 950, 2000
};

static const uint8_t g_phase[7] =
{
  0, 0, 1, 1, 2, 4, 0
};

/* Piece-square tables, as the board is seen by White (a8 first), in
 * centipawns: middle game, end game.  Black's are the same, mirrored.
 */

static const int8_t g_pst_mg[7][64] =
{
  { 0 },
  {                                      /* Pawn */
     0,   0,   0,   0,   0,   0,   0,   0,
    45,  50,  50,  55,  55,  50,  50,  45,
    10,  12,  20,  30,  30,  20,  12,  10,
     4,   6,  10,  24,  24,  10,   6,   4,
     0,   0,   5,  20,  20,   5,   0,   0,
     4,  -2,  -6,   4,   4,  -6,  -2,   4,
     4,   8,   8, -18, -18,   8,   8,   4,
     0,   0,   0,   0,   0,   0,   0,   0
  },
  {                                      /* Knight */
   -60, -40, -30, -30, -30, -30, -40, -60,
   -40, -20,   0,   5,   5,   0, -20, -40,
   -30,   5,  12,  16,  16,  12,   5, -30,
   -30,   5,  16,  22,  22,  16,   5, -30,
   -30,   0,  15,  20,  20,  15,   0, -30,
   -30,   5,  12,  14,  14,  12,   5, -30,
   -40, -20,   0,   5,   5,   0, -20, -40,
   -55, -35, -30, -30, -30, -30, -35, -55
  },
  {                                      /* Bishop */
   -20, -10, -10, -10, -10, -10, -10, -20,
   -10,   0,   0,   0,   0,   0,   0, -10,
   -10,   0,   5,  10,  10,   5,   0, -10,
   -10,   5,   6,  12,  12,   6,   5, -10,
   -10,   2,  12,  12,  12,  12,   2, -10,
   -10,  10,  10,  10,  10,  10,  10, -10,
   -10,  12,   2,   2,   2,   2,  12, -10,
   -20, -10, -12, -10, -10, -12, -10, -20
  },
  {                                      /* Rook */
     0,   0,   0,   2,   2,   0,   0,   0,
    10,  14,  14,  14,  14,  14,  14,  10,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -5,   0,   0,   0,   0,   0,   0,  -5,
    -8,  -2,   0,   0,   0,   0,  -2,  -8,
    -4,   0,   4,   8,   8,   6,   0,  -4
  },
  {                                      /* Queen */
   -20, -10, -10,  -5,  -5, -10, -10, -20,
   -10,   0,   0,   0,   0,   0,   0, -10,
   -10,   0,   5,   5,   5,   5,   0, -10,
    -5,   0,   5,   5,   5,   5,   0,  -5,
    -5,   0,   5,   5,   5,   5,   0,  -5,
   -10,   2,   5,   5,   5,   5,   0, -10,
   -10,   0,   2,   0,   0,   0,   0, -10,
   -20, -10, -10,  -2,  -5, -10, -10, -20
  },
  {                                      /* King */
   -40, -50, -50, -60, -60, -50, -50, -40,
   -40, -50, -50, -60, -60, -50, -50, -40,
   -40, -50, -50, -60, -60, -50, -50, -40,
   -40, -50, -50, -60, -60, -50, -50, -40,
   -30, -40, -40, -50, -50, -40, -40, -30,
   -20, -30, -30, -40, -40, -30, -30, -20,
    10,  10, -10, -20, -20, -10,  10,  10,
    20,  35,  15, -10,   0,  -5,  35,  20
  }
};

static const int8_t g_pst_eg[7][64] =
{
  { 0 },
  {                                      /* Pawn */
     0,   0,   0,   0,   0,   0,   0,   0,
    90,  90,  85,  80,  80,  85,  90,  90,
    55,  55,  50,  45,  45,  50,  55,  55,
    30,  30,  25,  20,  20,  25,  30,  30,
    14,  14,  10,  10,  10,  10,  14,  14,
     4,   4,   2,   2,   2,   2,   4,   4,
     0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0
  },
  {                                      /* Knight */
   -50, -35, -25, -20, -20, -25, -35, -50,
   -35, -15,  -5,   0,   0,  -5, -15, -35,
   -25,  -5,  10,  12,  12,  10,  -5, -25,
   -20,   0,  12,  18,  18,  12,   0, -20,
   -20,   0,  12,  18,  18,  12,   0, -20,
   -25,  -5,  10,  12,  12,  10,  -5, -25,
   -35, -15,  -5,   0,   0,  -5, -15, -35,
   -50, -35, -25, -20, -20, -25, -35, -50
  },
  {                                      /* Bishop */
   -15, -10,  -8,  -5,  -5,  -8, -10, -15,
   -10,  -3,   0,   2,   2,   0,  -3, -10,
    -8,   0,   5,   6,   6,   5,   0,  -8,
    -5,   2,   6,  10,  10,   6,   2,  -5,
    -5,   2,   6,  10,  10,   6,   2,  -5,
    -8,   0,   5,   6,   6,   5,   0,  -8,
   -10,  -3,   0,   2,   2,   0,  -3, -10,
   -15, -10,  -8,  -5,  -5,  -8, -10, -15
  },
  {                                      /* Rook */
     8,  10,  10,  10,  10,  10,  10,   8,
    10,  12,  12,  12,  12,  12,  12,  10,
     4,   4,   4,   4,   4,   4,   4,   4,
     0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,
    -2,  -2,  -2,  -2,  -2,  -2,  -2,  -2,
    -4,  -4,  -4,  -4,  -4,  -4,  -4,  -4,
    -6,  -4,  -2,   0,   0,  -2,  -4,  -6
  },
  {                                      /* Queen */
   -20, -10, -10,  -5,  -5, -10, -10, -20,
   -10,   0,   5,   5,   5,   5,   0, -10,
   -10,   5,  10,  12,  12,  10,   5, -10,
    -5,   5,  12,  16,  16,  12,   5,  -5,
    -5,   5,  12,  16,  16,  12,   5,  -5,
   -10,   5,  10,  12,  12,  10,   5, -10,
   -10,   0,   5,   5,   5,   5,   0, -10,
   -20, -10, -10,  -5,  -5, -10, -10, -20
  },
  {                                      /* King */
   -50, -35, -25, -20, -20, -25, -35, -50,
   -30, -15,  -5,   0,   0,  -5, -15, -30,
   -25,  -5,  15,  22,  22,  15,  -5, -25,
   -25,  -5,  22,  30,  30,  22,  -5, -25,
   -25,  -5,  22,  30,  30,  22,  -5, -25,
   -25,  -5,  15,  22,  22,  15,  -5, -25,
   -30, -20,  -5,   0,   0,  -5, -20, -30,
   -50, -35, -30, -25, -25, -30, -35, -50
  }
};

/* Passed pawns, by how far they have gone (0: their first rank) */

static const int16_t g_passed_mg[8] =
{
  0, 2, 5, 10, 20, 35, 55, 0
};

static const int16_t g_passed_eg[8] =
{
  0, 8, 12, 25, 45, 75, 115, 0
};

/* The 0x88 difference tables, and the castling rights a square keeps */

static uint8_t g_attack[240];
static int8_t  g_step[240];
static uint8_t g_keep[128];
static bool    g_ready;

/****************************************************************************
 * Private Functions: keys
 ****************************************************************************/

/* A Zobrist key: pieces 0..15 x 64 squares, then castling (16), en
 * passant files (8) and the side.  Two rounds of a 32-bit mixer (the
 * finaliser of MurmurHash3) make its halves.
 */

#define K_CASTLE        1024
#define K_EP            1040
#define K_SIDE          1048

static inline uint32_t mix32(uint32_t h)
{
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

static inline uint64_t zkey(int i)
{
  uint32_t a = mix32((uint32_t)i * 2654435761u + 0x9e3779b9u);
  uint32_t b = mix32(a ^ ((uint32_t)i * 0x27d4eb2fu + 0x165667b1u));

  return (uint64_t)a << 32 | b;
}

static inline uint64_t pkey(int pc, int sq)
{
  return zkey(pc * 64 + CH_SQ64(sq));
}

/****************************************************************************
 * Private Functions: the board
 ****************************************************************************/

static inline int pst_index(int side, int sq)
{
  /* White: the tables' first row is rank 8 */

  return side == CH_WHITE ? (7 - CH_RANK(sq)) * 8 + CH_FILE(sq) :
                            CH_RANK(sq) * 8 + CH_FILE(sq);
}

static inline void put(struct ch_pos_s *p, int pc, int sq, const bool keys)
{
  int side = CH_SIDE(pc);
  int t = CH_TYPE(pc);
  int i = pst_index(side, sq);

  p->board[sq] = pc;
  p->index[sq] = p->count[side];
  p->list[side][p->count[side]++] = sq;
  p->mg[side] += g_mgval[t] + g_pst_mg[t][i];
  p->eg[side] += g_egval[t] + g_pst_eg[t][i];
  p->phase += g_phase[t];
  p->npieces[pc]++;
  if (t == CH_PAWN)
    {
      p->pawns[side][CH_FILE(sq)] |= 1 << CH_RANK(sq);
    }

  if (keys)
    {
      p->hash ^= pkey(pc, sq);
    }

  if (t == CH_KING)
    {
      p->king[side] = sq;
    }
}

static inline void take(struct ch_pos_s *p, int sq, const bool keys)
{
  int pc = p->board[sq];
  int side = CH_SIDE(pc);
  int t = CH_TYPE(pc);
  int i = pst_index(side, sq);
  int last = p->list[side][--p->count[side]];

  p->list[side][p->index[sq]] = last;
  p->index[last] = p->index[sq];
  p->board[sq] = CH_EMPTY;
  p->mg[side] -= g_mgval[t] + g_pst_mg[t][i];
  p->eg[side] -= g_egval[t] + g_pst_eg[t][i];
  p->phase -= g_phase[t];
  p->npieces[pc]--;
  if (t == CH_PAWN)
    {
      p->pawns[side][CH_FILE(sq)] &= ~(1 << CH_RANK(sq));
    }

  if (keys)
    {
      p->hash ^= pkey(pc, sq);
    }
}

static inline void shift(struct ch_pos_s *p, int from, int to,
                         const bool keys)
{
  int pc = p->board[from];
  int side = CH_SIDE(pc);
  int t = CH_TYPE(pc);
  int i0 = pst_index(side, from);
  int i1 = pst_index(side, to);

  p->board[to] = pc;
  p->board[from] = CH_EMPTY;
  p->index[to] = p->index[from];
  p->list[side][p->index[to]] = to;
  p->mg[side] += g_pst_mg[t][i1] - g_pst_mg[t][i0];
  p->eg[side] += g_pst_eg[t][i1] - g_pst_eg[t][i0];
  if (t == CH_PAWN)
    {
      p->pawns[side][CH_FILE(from)] &= ~(1 << CH_RANK(from));
      p->pawns[side][CH_FILE(to)] |= 1 << CH_RANK(to);
    }

  if (keys)
    {
      p->hash ^= pkey(pc, from) ^ pkey(pc, to);
    }

  if (t == CH_KING)
    {
      p->king[side] = to;
    }
}

static void clear(struct ch_pos_s *p)
{
  memset(p, 0, sizeof(*p));
  p->ep = CH_NOSQ;
  p->fullmove = 1;
}

/* The en passant square counts only if a pawn could take there */

static bool ep_useful(const struct ch_pos_s *p, int ep)
{
  int pawn = CH_PC(p->side, CH_PAWN);
  int back = p->side == CH_WHITE ? -16 : 16;

  return (!CH_OFF(ep + back - 1) && p->board[ep + back - 1] == pawn) ||
         (!CH_OFF(ep + back + 1) && p->board[ep + back + 1] == pawn);
}

/****************************************************************************
 * Public Functions: positions
 ****************************************************************************/

void ch_init(void)
{
  int from;
  int d;
  int i;

  if (g_ready)
    {
      return;
    }

  memset(g_attack, 0, sizeof(g_attack));
  memset(g_step, 0, sizeof(g_step));
  from = CH_SQ(3, 3) + 0;
  for (d = 0; d < 8; d++)
    {
      int to = from + g_knight[d];

      g_attack[to - from + 119] |= A_KNIGHT;
      g_step[to - from + 119] = g_knight[d];
      g_attack[from + g_king[d] - from + 119] |= A_KING;
    }

  g_attack[15 + 119] |= A_WPAWN;
  g_attack[17 + 119] |= A_WPAWN;
  g_attack[-15 + 119] |= A_BPAWN;
  g_attack[-17 + 119] |= A_BPAWN;

  for (d = 0; d < 4; d++)
    {
      for (i = 1; i < 8; i++)
        {
          g_attack[g_diag[d] * i + 119] |= A_BISHOP | A_QUEEN;
          g_step[g_diag[d] * i + 119] = g_diag[d];
          g_attack[g_orth[d] * i + 119] |= A_ROOK | A_QUEEN;
          g_step[g_orth[d] * i + 119] = g_orth[d];
        }
    }

  for (i = 0; i < 8; i++)
    {
      g_step[g_king[i] + 119] = g_king[i];
    }

  memset(g_keep, 0x0f, sizeof(g_keep));
  g_keep[CH_SQ(4, 0)] = CH_BK | CH_BQ;
  g_keep[CH_SQ(7, 0)] = (uint8_t)~CH_WK & 0x0f;
  g_keep[CH_SQ(0, 0)] = (uint8_t)~CH_WQ & 0x0f;
  g_keep[CH_SQ(4, 7)] = CH_WK | CH_WQ;
  g_keep[CH_SQ(7, 7)] = (uint8_t)~CH_BK & 0x0f;
  g_keep[CH_SQ(0, 7)] = (uint8_t)~CH_BQ & 0x0f;
  g_ready = true;
}

bool ch_setfen(struct ch_pos_s *p, const char *fen)
{
  static const char pieces[] = " pnbrqk";
  int kings[2] = { 0, 0 };
  int f = 0;
  int r = 7;
  const char *s = fen;

  ch_init();
  clear(p);
  while (*s == ' ')
    {
      s++;
    }

  for (; *s != '\0' && *s != ' '; s++)
    {
      if (*s == '/')
        {
          if (f != 8 || r == 0)
            {
              return false;
            }

          f = 0;
          r--;
        }
      else if (*s >= '1' && *s <= '8')
        {
          f += *s - '0';
          if (f > 8)
            {
              return false;
            }
        }
      else
        {
          const char *c = strchr(pieces, *s | 0x20);
          int side = (*s & 0x20) ? CH_BLACK : CH_WHITE;

          if (c == NULL || c == pieces || f > 7 || p->count[side] >= 16)
            {
              return false;
            }

          if (c - pieces == CH_KING)
            {
              kings[side]++;
            }

          if (c - pieces == CH_PAWN && (r == 0 || r == 7))
            {
              return false;
            }

          put(p, CH_PC(side, c - pieces), CH_SQ(f, r), true);
          f++;
        }
    }

  if (r != 0 || f != 8 || kings[0] != 1 || kings[1] != 1)
    {
      return false;
    }

  while (*s == ' ')
    {
      s++;
    }

  if (*s == 'b')
    {
      p->side = CH_BLACK;
      p->hash ^= zkey(K_SIDE);
    }
  else if (*s != 'w' && *s != '\0')
    {
      return false;
    }

  if (*s != '\0')
    {
      s++;
    }

  while (*s == ' ')
    {
      s++;
    }

  for (; *s != '\0' && *s != ' '; s++)
    {
      int bit = *s == 'K' ? CH_WK : *s == 'Q' ? CH_WQ :
                *s == 'k' ? CH_BK : *s == 'q' ? CH_BQ : 0;

      /* A right counts only with the king and the rook at home */

      if ((bit == CH_WK && p->board[CH_SQ(4, 0)] == CH_PC(0, CH_KING) &&
           p->board[CH_SQ(7, 0)] == CH_PC(0, CH_ROOK)) ||
          (bit == CH_WQ && p->board[CH_SQ(4, 0)] == CH_PC(0, CH_KING) &&
           p->board[CH_SQ(0, 0)] == CH_PC(0, CH_ROOK)) ||
          (bit == CH_BK && p->board[CH_SQ(4, 7)] == CH_PC(1, CH_KING) &&
           p->board[CH_SQ(7, 7)] == CH_PC(1, CH_ROOK)) ||
          (bit == CH_BQ && p->board[CH_SQ(4, 7)] == CH_PC(1, CH_KING) &&
           p->board[CH_SQ(0, 7)] == CH_PC(1, CH_ROOK)))
        {
          p->castle |= bit;
        }
    }

  p->hash ^= zkey(K_CASTLE + p->castle);
  while (*s == ' ')
    {
      s++;
    }

  if (s[0] >= 'a' && s[0] <= 'h' && s[1] >= '1' && s[1] <= '8')
    {
      int ep = CH_SQ(s[0] - 'a', s[1] - '1');

      if (CH_RANK(ep) == (p->side == CH_WHITE ? 5 : 2) && ep_useful(p, ep))
        {
          p->ep = ep;
          p->hash ^= zkey(K_EP + CH_FILE(ep));
        }

      s += 2;
    }
  else if (*s == '-')
    {
      s++;
    }

  while (*s == ' ')
    {
      s++;
    }

  if (*s >= '0' && *s <= '9')
    {
      p->halfmove = (uint8_t)(atoi(s) > 200 ? 200 : atoi(s));
      while (*s >= '0' && *s <= '9')
        {
          s++;
        }

      while (*s == ' ')
        {
          s++;
        }

      if (*s >= '1' && *s <= '9')
        {
          p->fullmove = (uint16_t)atoi(s);
        }
    }

  /* The side not to move may not be in check */

  return !ch_incheck(p, p->side ^ 1);
}

void ch_getfen(const struct ch_pos_s *p, char *buf, int size)
{
  static const char pieces[] = " PNBRQK  pnbrqk";
  char out[100];
  int n = 0;
  int f;
  int r;

  for (r = 7; r >= 0; r--)
    {
      int empty = 0;

      for (f = 0; f < 8; f++)
        {
          int pc = p->board[CH_SQ(f, r)];

          if (pc == CH_EMPTY)
            {
              empty++;
              continue;
            }

          if (empty > 0)
            {
              out[n++] = '0' + empty;
              empty = 0;
            }

          out[n++] = pieces[pc];
        }

      if (empty > 0)
        {
          out[n++] = '0' + empty;
        }

      if (r > 0)
        {
          out[n++] = '/';
        }
    }

  out[n++] = ' ';
  out[n++] = p->side == CH_WHITE ? 'w' : 'b';
  out[n++] = ' ';
  if (p->castle == 0)
    {
      out[n++] = '-';
    }

  if (p->castle & CH_WK)
    {
      out[n++] = 'K';
    }

  if (p->castle & CH_WQ)
    {
      out[n++] = 'Q';
    }

  if (p->castle & CH_BK)
    {
      out[n++] = 'k';
    }

  if (p->castle & CH_BQ)
    {
      out[n++] = 'q';
    }

  out[n++] = ' ';
  if (p->ep == CH_NOSQ)
    {
      out[n++] = '-';
    }
  else
    {
      out[n++] = 'a' + CH_FILE(p->ep);
      out[n++] = '1' + CH_RANK(p->ep);
    }

  out[n] = '\0';
  snprintf(buf, size, "%s %d %d", out, p->halfmove, p->fullmove);
}

void ch_startpos(struct ch_pos_s *p)
{
  ch_setfen(p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

/****************************************************************************
 * Public Functions: attacks
 ****************************************************************************/

bool ch_attacked(const struct ch_pos_s *p, int sq, int by)
{
  const uint8_t *l = p->list[by];
  int n = p->count[by];
  int i;

  for (i = 0; i < n; i++)
    {
      int from = l[i];
      int d = sq - from + 119;
      int t;
      uint8_t a = g_attack[d];

      if (a == 0)
        {
          continue;
        }

      t = CH_TYPE(p->board[from]);
      switch (t)
        {
          case CH_PAWN:
            if (a & (by == CH_WHITE ? A_WPAWN : A_BPAWN))
              {
                return true;
              }
            break;

          case CH_KNIGHT:
            if (a & A_KNIGHT)
              {
                return true;
              }
            break;

          case CH_KING:
            if (a & A_KING)
              {
                return true;
              }
            break;

          default:
            if (a & (t == CH_BISHOP ? A_BISHOP : t == CH_ROOK ? A_ROOK :
                     A_QUEEN))
              {
                int step = g_step[d];
                int s = from + step;

                while (s != sq && p->board[s] == CH_EMPTY)
                  {
                    s += step;
                  }

                if (s == sq)
                  {
                    return true;
                  }
              }
            break;
        }
    }

  return false;
}

bool ch_incheck(const struct ch_pos_s *p, int side)
{
  return ch_attacked(p, p->king[side], side ^ 1);
}

/****************************************************************************
 * Public Functions: moves
 ****************************************************************************/

static inline int add(ch_move_t *m, int n, int from, int to, int promo,
                      int flags)
{
  m[n] = CH_MOVE(from, to, promo, flags);
  return n + 1;
}

static int add_pawn(ch_move_t *m, int n, int from, int to, int flags,
                    bool last, bool captures)
{
  if (last)
    {
      n = add(m, n, from, to, CH_QUEEN, flags);
      if (!captures || (flags & CH_F_CAPTURE))
        {
          n = add(m, n, from, to, CH_ROOK, flags);
          n = add(m, n, from, to, CH_BISHOP, flags);
          n = add(m, n, from, to, CH_KNIGHT, flags);
        }

      return n;
    }

  return add(m, n, from, to, 0, flags);
}

int ch_generate(const struct ch_pos_s *p, ch_move_t *m, bool captures)
{
  int side = p->side;
  int other = side ^ 1;
  int up = side == CH_WHITE ? 16 : -16;
  int n = 0;
  int i;
  int d;

  for (i = 0; i < p->count[side]; i++)
    {
      int from = p->list[side][i];
      int t = CH_TYPE(p->board[from]);

      switch (t)
        {
          case CH_PAWN:
            {
              int to = from + up;
              int r = CH_RANK(from);
              bool last = r == (side == CH_WHITE ? 6 : 1);
              int c;

              if (p->board[to] == CH_EMPTY && (!captures || last))
                {
                  n = add_pawn(m, n, from, to, 0, last, captures);
                  if (!captures && r == (side == CH_WHITE ? 1 : 6) &&
                      p->board[to + up] == CH_EMPTY)
                    {
                      n = add(m, n, from, to + up, 0, CH_F_DOUBLE);
                    }
                }

              for (c = -1; c <= 1; c += 2)
                {
                  int t2 = to + c;

                  if (CH_OFF(t2))
                    {
                      continue;
                    }

                  if (p->board[t2] != CH_EMPTY &&
                      CH_SIDE(p->board[t2]) == other)
                    {
                      n = add_pawn(m, n, from, t2, CH_F_CAPTURE, last,
                                   captures);
                    }
                  else if (t2 == p->ep)
                    {
                      n = add(m, n, from, t2, 0, CH_F_CAPTURE | CH_F_EP);
                    }
                }
            }
            break;

          case CH_KNIGHT:
          case CH_KING:
            {
              const int8_t *dir = t == CH_KNIGHT ? g_knight : g_king;

              for (d = 0; d < 8; d++)
                {
                  int to = from + dir[d];
                  int pc;

                  if (CH_OFF(to))
                    {
                      continue;
                    }

                  pc = p->board[to];
                  if (pc == CH_EMPTY)
                    {
                      if (!captures)
                        {
                          n = add(m, n, from, to, 0, 0);
                        }
                    }
                  else if (CH_SIDE(pc) == other)
                    {
                      n = add(m, n, from, to, 0, CH_F_CAPTURE);
                    }
                }

              /* Castling: the squares between empty, the king not in
               * check nor passing an attacked square (where it lands is
               * checked when the move is made)
               */

              if (t == CH_KING && !captures)
                {
                  int k = side == CH_WHITE ? CH_SQ(4, 0) : CH_SQ(4, 7);
                  int kr = side == CH_WHITE ? CH_WK : CH_BK;
                  int qr = side == CH_WHITE ? CH_WQ : CH_BQ;

                  if (from == k && (p->castle & (kr | qr)) != 0 &&
                      !ch_attacked(p, k, other))
                    {
                      if ((p->castle & kr) && p->board[k + 1] == 0 &&
                          p->board[k + 2] == 0 &&
                          !ch_attacked(p, k + 1, other))
                        {
                          n = add(m, n, k, k + 2, 0, CH_F_CASTLE);
                        }

                      if ((p->castle & qr) && p->board[k - 1] == 0 &&
                          p->board[k - 2] == 0 && p->board[k - 3] == 0 &&
                          !ch_attacked(p, k - 1, other))
                        {
                          n = add(m, n, k, k - 2, 0, CH_F_CASTLE);
                        }
                    }
                }
            }
            break;

          default:
            {
              int d0 = t == CH_ROOK ? 4 : 0;
              int d1 = t == CH_BISHOP ? 4 : 8;

              for (d = d0; d < d1; d++)
                {
                  int step = d < 4 ? g_diag[d] : g_orth[d - 4];
                  int to = from + step;

                  while (!CH_OFF(to))
                    {
                      int pc = p->board[to];

                      if (pc == CH_EMPTY)
                        {
                          if (!captures)
                            {
                              n = add(m, n, from, to, 0, 0);
                            }
                        }
                      else
                        {
                          if (CH_SIDE(pc) == other)
                            {
                              n = add(m, n, from, to, 0, CH_F_CAPTURE);
                            }

                          break;
                        }

                      to += step;
                    }
                }
            }
            break;
        }
    }

  return n;
}

/* Make m; legal: whether it left the king safe.  check: whether the side
 * was in check (-1: not known).  Only a king move, en passant, a move out
 * of check or one from a square in line with the king can leave it
 * attacked: the others need no test.
 */

static bool make(struct ch_pos_s *p, ch_move_t m, struct ch_undo_s *u,
                 int check)
{
  int from = CH_FROM(m);
  int to = CH_TO(m);
  int flags = CH_FLAGS(m);
  int side = p->side;
  int pc = p->board[from];

  u->hash     = p->hash;
  u->captured = CH_EMPTY;
  u->castle   = p->castle;
  u->ep       = p->ep;
  u->halfmove = p->halfmove;

  if (p->ep != CH_NOSQ)
    {
      p->hash ^= zkey(K_EP + CH_FILE(p->ep));
      p->ep = CH_NOSQ;
    }

  p->halfmove++;
  if (flags & CH_F_EP)
    {
      int cap = to + (side == CH_WHITE ? -16 : 16);

      u->captured = p->board[cap];
      take(p, cap, true);
    }
  else if (p->board[to] != CH_EMPTY)
    {
      u->captured = p->board[to];
      take(p, to, true);
    }

  if (u->captured != CH_EMPTY || CH_TYPE(pc) == CH_PAWN)
    {
      p->halfmove = 0;
    }

  if (CH_PROMO(m) != 0)
    {
      take(p, from, true);
      put(p, CH_PC(side, CH_PROMO(m)), to, true);
    }
  else
    {
      shift(p, from, to, true);
    }

  if (flags & CH_F_CASTLE)
    {
      if (to > from)
        {
          shift(p, to + 1, to - 1, true);
        }
      else
        {
          shift(p, to - 2, to + 1, true);
        }
    }

  if ((flags & CH_F_DOUBLE))
    {
      int ep = (from + to) / 2;

      p->side = side ^ 1;
      if (ep_useful(p, ep))
        {
          p->ep = ep;
          p->hash ^= zkey(K_EP + CH_FILE(ep));
        }

      p->side = side;
    }

  if (p->castle != 0)
    {
      uint8_t c = p->castle & g_keep[from] & g_keep[to];

      if (c != p->castle)
        {
          p->hash ^= zkey(K_CASTLE + p->castle) ^ zkey(K_CASTLE + c);
          p->castle = c;
        }
    }

  p->side = side ^ 1;
  p->hash ^= zkey(K_SIDE);
  if (side == CH_BLACK)
    {
      p->fullmove++;
    }

  if (check == 0 && CH_TYPE(pc) != CH_KING && (flags & CH_F_EP) == 0 &&
      (g_attack[p->king[side] - from + 119] & A_QUEEN) == 0)
    {
      return true;
    }

  return !ch_attacked(p, p->king[side], side ^ 1);
}

bool ch_make(struct ch_pos_s *p, ch_move_t m, struct ch_undo_s *u)
{
  return make(p, m, u, -1);
}

void ch_unmake(struct ch_pos_s *p, ch_move_t m, const struct ch_undo_s *u)
{
  int from = CH_FROM(m);
  int to = CH_TO(m);
  int flags = CH_FLAGS(m);
  int side = p->side ^ 1;

  p->side = side;
  if (side == CH_BLACK)
    {
      p->fullmove--;
    }

  if (flags & CH_F_CASTLE)
    {
      if (to > from)
        {
          shift(p, to - 1, to + 1, false);
        }
      else
        {
          shift(p, to + 1, to - 2, false);
        }
    }

  if (CH_PROMO(m) != 0)
    {
      take(p, to, false);
      put(p, CH_PC(side, CH_PAWN), from, false);
    }
  else
    {
      shift(p, to, from, false);
    }

  if (u->captured != CH_EMPTY)
    {
      put(p, u->captured, (flags & CH_F_EP) ?
          to + (side == CH_WHITE ? -16 : 16) : to, false);
    }

  p->castle   = u->castle;
  p->ep       = u->ep;
  p->halfmove = u->halfmove;
  p->hash     = u->hash;
}

int ch_legal(struct ch_pos_s *p, ch_move_t *m)
{
  struct ch_undo_s u;
  int n = ch_generate(p, m, false);
  int k = 0;
  int i;

  /* The legal ones kept in place: no second list on the stack */

  for (i = 0; i < n; i++)
    {
      ch_move_t mv = m[i];

      if (ch_make(p, mv, &u))
        {
          m[k++] = mv;
        }

      ch_unmake(p, mv, &u);
    }

  return k;
}

uint16_t ch_pack(ch_move_t m)
{
  return (uint16_t)(CH_SQ64(CH_FROM(m)) | CH_SQ64(CH_TO(m)) << 6 |
                    CH_PROMO(m) << 12);
}

bool ch_insufficient(const struct ch_pos_s *p)
{
  int s;
  int minors[2];

  for (s = 0; s < 2; s++)
    {
      if (p->npieces[CH_PC(s, CH_PAWN)] || p->npieces[CH_PC(s, CH_ROOK)] ||
          p->npieces[CH_PC(s, CH_QUEEN)])
        {
          return false;
        }

      minors[s] = p->npieces[CH_PC(s, CH_KNIGHT)] +
                  p->npieces[CH_PC(s, CH_BISHOP)];
    }

  if (minors[0] + minors[1] <= 1)
    {
      return true;                       /* K v K, K + minor v K */
    }

  /* Bishops only, all on squares of one colour */

  if (p->npieces[CH_PC(0, CH_KNIGHT)] == 0 &&
      p->npieces[CH_PC(1, CH_KNIGHT)] == 0)
    {
      int colours = 0;
      int i;

      for (s = 0; s < 2; s++)
        {
          for (i = 0; i < p->count[s]; i++)
            {
              int sq = p->list[s][i];

              if (CH_TYPE(p->board[sq]) == CH_BISHOP)
                {
                  colours |= 1 << ((CH_FILE(sq) + CH_RANK(sq)) & 1);
                }
            }
        }

      return colours != 3;
    }

  return false;
}

/****************************************************************************
 * Public Functions: notation
 ****************************************************************************/

void ch_coord(ch_move_t m, char *buf)
{
  int from = CH_FROM(m);
  int to = CH_TO(m);

  buf[0] = 'a' + CH_FILE(from);
  buf[1] = '1' + CH_RANK(from);
  buf[2] = 'a' + CH_FILE(to);
  buf[3] = '1' + CH_RANK(to);
  buf[4] = CH_PROMO(m) != 0 ? " pnbrqk"[CH_PROMO(m)] : '\0';
  buf[5] = '\0';
}

void ch_san(struct ch_pos_s *p, ch_move_t m, char *buf)
{
  static const char names[] = "  NBRQK";
  ch_move_t legal[CH_MAXMOVES];
  struct ch_undo_s u;
  int from = CH_FROM(m);
  int to = CH_TO(m);
  int t = CH_TYPE(p->board[from]);
  int n = 0;
  int k;
  int i;

  if (CH_FLAGS(m) & CH_F_CASTLE)
    {
      strcpy(buf, to > from ? "O-O" : "O-O-O");
      n = strlen(buf);
    }
  else
    {
      if (t == CH_PAWN)
        {
          if (CH_FLAGS(m) & CH_F_CAPTURE)
            {
              buf[n++] = 'a' + CH_FILE(from);
            }
        }
      else
        {
          bool file = false;
          bool rank = false;
          bool other = false;

          buf[n++] = names[t];
          k = ch_legal(p, legal);
          for (i = 0; i < k; i++)
            {
              int f2 = CH_FROM(legal[i]);

              if ((int)CH_TO(legal[i]) == to && f2 != from &&
                  CH_TYPE(p->board[f2]) == t)
                {
                  other = true;
                  if (CH_FILE(f2) == CH_FILE(from))
                    {
                      file = true;
                    }

                  if (CH_RANK(f2) == CH_RANK(from))
                    {
                      rank = true;
                    }
                }
            }

          if (other)
            {
              if (!file)
                {
                  buf[n++] = 'a' + CH_FILE(from);
                }
              else if (!rank)
                {
                  buf[n++] = '1' + CH_RANK(from);
                }
              else
                {
                  buf[n++] = 'a' + CH_FILE(from);
                  buf[n++] = '1' + CH_RANK(from);
                }
            }
        }

      if (CH_FLAGS(m) & CH_F_CAPTURE)
        {
          buf[n++] = 'x';
        }

      buf[n++] = 'a' + CH_FILE(to);
      buf[n++] = '1' + CH_RANK(to);
      if (CH_PROMO(m) != 0)
        {
          buf[n++] = '=';
          buf[n++] = names[CH_PROMO(m)];
        }
    }

  /* Check, or mate */

  ch_make(p, m, &u);
  if (ch_incheck(p, p->side))
    {
      buf[n++] = ch_legal(p, legal) == 0 ? '#' : '+';
    }

  ch_unmake(p, m, &u);
  buf[n] = '\0';
}

ch_move_t ch_parse(struct ch_pos_s *p, const char *s)
{
  ch_move_t legal[CH_MAXMOVES];
  char buf[12];
  char in[12];
  int n = ch_legal(p, legal);
  int k = 0;
  int i;

  /* The input without check marks, and castling with zeros */

  for (i = 0; s[i] != '\0' && k < 10; i++)
    {
      if (s[i] != '+' && s[i] != '#' && s[i] != '!' && s[i] != '?')
        {
          in[k++] = s[i] == '0' ? 'O' : s[i];
        }
    }

  in[k] = '\0';
  for (i = 0; i < n; i++)
    {
      int len;

      ch_coord(legal[i], buf);
      if (strcmp(buf, in) == 0)
        {
          return legal[i];
        }

      ch_san(p, legal[i], buf);
      len = strlen(buf);
      while (len > 0 && (buf[len - 1] == '+' || buf[len - 1] == '#'))
        {
          buf[--len] = '\0';
        }

      if (strcmp(buf, in) == 0)
        {
          return legal[i];
        }
    }

  return CH_NOMOVE;
}

/****************************************************************************
 * Public Functions: perft
 ****************************************************************************/

uint64_t ch_perft(struct ch_pos_s *p, int depth)
{
  ch_move_t m[CH_MAXMOVES];
  struct ch_undo_s u;
  uint64_t total = 0;
  int n;
  int i;

  if (depth == 0)
    {
      return 1;
    }

  n = ch_generate(p, m, false);
  for (i = 0; i < n; i++)
    {
      if (ch_make(p, m[i], &u))
        {
          total += depth == 1 ? 1 : ch_perft(p, depth - 1);
        }

      ch_unmake(p, m[i], &u);
    }

  return total;
}

/****************************************************************************
 * Private Functions: evaluation
 ****************************************************************************/

static int distance(int a, int b)
{
  int df = abs(CH_FILE(a) - CH_FILE(b));
  int dr = abs(CH_RANK(a) - CH_RANK(b));

  return df > dr ? df : dr;
}

static int centre_distance(int sq)
{
  int f = CH_FILE(sq);
  int r = CH_RANK(sq);
  int df = f < 4 ? 3 - f : f - 4;
  int dr = r < 4 ? 3 - r : r - 4;

  return df + dr;
}

/* The pawns, rooks on files, the king's shelter: White's view.  The
 * pawns come from their rank masks (a bit a rank, a byte a file), so
 * isolated, doubled and passed pawns are a few bit operations a file.
 */

static inline int rel(int side, int rank)
{
  return side == CH_WHITE ? rank : 7 - rank;
}

static void eval_structure(const struct ch_pos_s *p, int *mg, int *eg)
{
  int s;
  int f;
  int i;

  for (s = 0; s < 2; s++)
    {
      const uint8_t *own = p->pawns[s];
      const uint8_t *their = p->pawns[s ^ 1];
      int o = s ^ 1;
      int smg = 0;
      int seg = 0;

      for (f = 0; f < 8; f++)
        {
          unsigned int bits = own[f];
          unsigned int left = f > 0 ? own[f - 1] : 0;
          unsigned int right = f < 7 ? own[f + 1] : 0;
          unsigned int ahead;
          unsigned int enemy;
          int n;
          int top;
          int r;

          if (bits == 0)
            {
              continue;
            }

          n = __builtin_popcount(bits);
          if (left == 0 && right == 0)
            {
              smg -= 10 * n;                   /* Isolated */
              seg -= 14 * n;
            }

          smg -= 8 * (n - 1);                  /* Doubled */
          seg -= 18 * (n - 1);

          /* Passed: the front pawn, with no enemy pawn ahead of it on this
           * file or the ones beside it
           */

          top   = s == CH_WHITE ? 31 - __builtin_clz(bits) :
                                  __builtin_ctz(bits);
          ahead = s == CH_WHITE ? ~((2u << top) - 1) & 0xff :
                                  (1u << top) - 1;
          enemy = their[f] | (f > 0 ? their[f - 1] : 0) |
                  (f < 7 ? their[f + 1] : 0);
          if ((enemy & ahead) == 0)
            {
              int sq = CH_SQ(f, top);
              int front = sq + (s == CH_WHITE ? 16 : -16);

              r = rel(s, top);
              smg += g_passed_mg[r];
              seg += g_passed_eg[r];

              /* Blocked by anything: worth less */

              if (p->board[front] != CH_EMPTY)
                {
                  seg -= g_passed_eg[r] / 3;
                }

              /* The kings: ours near it, theirs far */

              seg += (distance(p->king[o], front) -
                      distance(p->king[s], front)) * r;
            }
        }

      if (p->npieces[CH_PC(s, CH_BISHOP)] >= 2)
        {
          smg += 30;
          seg += 50;
        }

      /* Rooks on files without their own pawns */

      if (p->npieces[CH_PC(s, CH_ROOK)] > 0)
        {
          for (i = 0; i < p->count[s]; i++)
            {
              int sq = p->list[s][i];

              if (CH_TYPE(p->board[sq]) == CH_ROOK &&
                  own[CH_FILE(sq)] == 0)
                {
                  smg += their[CH_FILE(sq)] == 0 ? 22 : 10;
                  seg += their[CH_FILE(sq)] == 0 ? 10 : 5;
                }
            }
        }

      /* The king's shelter: its own pawns in front of it, no open files
       * beside it; worth something only while the queens are on
       */

      if (p->npieces[CH_PC(o, CH_QUEEN)] > 0)
        {
          int k = p->king[s];
          int kf = CH_FILE(k);
          int kr = rel(s, CH_RANK(k));
          int shelter = 0;

          if (kr <= 1)
            {
              for (f = kf - 1; f <= kf + 1; f++)
                {
                  unsigned int bits;
                  int back;

                  if (f < 0 || f > 7)
                    {
                      continue;
                    }

                  bits = own[f];
                  if (bits == 0)
                    {
                      shelter -= their[f] == 0 ? 30 : 20;
                      continue;
                    }

                  back = rel(s, s == CH_WHITE ? __builtin_ctz(bits) :
                                                31 - __builtin_clz(bits));
                  shelter += back == kr + 1 ? 12 : back == kr + 2 ? 5 : -8;
                }
            }
          else
            {
              shelter -= 20 + 6 * kr;            /* Out in the open */
            }

          smg += shelter;
        }

      if (s == CH_WHITE)
        {
          *mg += smg;
          *eg += seg;
        }
      else
        {
          *mg -= smg;
          *eg -= seg;
        }
    }
}

/* White's view of the position, in centipawns */

static int eval_white(const struct ch_pos_s *p)
{
  int mg = p->mg[CH_WHITE] - p->mg[CH_BLACK];
  int eg = p->eg[CH_WHITE] - p->eg[CH_BLACK];
  int phase = p->phase > 24 ? 24 : p->phase;
  int score;
  int s;

  eval_structure(p, &mg, &eg);
  score = (mg * phase + eg * (24 - phase)) / 24;

  /* Won endings without pawns: drive the king to the edge; drawn ones:
   * nearly nothing
   */

  for (s = 0; s < 2; s++)
    {
      int sign = s == CH_WHITE ? 1 : -1;
      int o = s ^ 1;

      if (p->npieces[CH_PC(s, CH_PAWN)] == 0 &&
          (score * sign) > 0)
        {
          int mat = p->npieces[CH_PC(s, CH_KNIGHT)] * 3 +
                    p->npieces[CH_PC(s, CH_BISHOP)] * 3 +
                    p->npieces[CH_PC(s, CH_ROOK)] * 5 +
                    p->npieces[CH_PC(s, CH_QUEEN)] * 9;
          int omat = p->npieces[CH_PC(o, CH_KNIGHT)] * 3 +
                     p->npieces[CH_PC(o, CH_BISHOP)] * 3 +
                     p->npieces[CH_PC(o, CH_ROOK)] * 5 +
                     p->npieces[CH_PC(o, CH_QUEEN)] * 9;

          if (mat - omat <= 3 && p->npieces[CH_PC(o, CH_PAWN)] == 0)
            {
              score /= 8;                /* A minor piece up: a draw */
            }
          else if (mat - omat >= 4 && p->npieces[CH_PC(o, CH_PAWN)] == 0)
            {
              score += sign * (centre_distance(p->king[o]) * 12 +
                               (14 - distance(p->king[s], p->king[o]) * 2) *
                               4);
            }
        }
    }

  if (ch_insufficient(p))
    {
      return 0;
    }

  return score;
}

int ch_evaluate(const struct ch_pos_s *p)
{
  int score = eval_white(p);

  return (p->side == CH_WHITE ? score : -score) + 10;
}

/****************************************************************************
 * Private Functions: the search
 ****************************************************************************/

static int evaluate(struct ch_search_s *s)
{
  int e = ch_evaluate(&s->pos);

  if (s->noise > 0)
    {
      uint32_t h = mix32((uint32_t)(s->pos.hash >> 17) ^ s->seed);

      e += (int)(h % (uint32_t)(2 * s->noise + 1)) - s->noise;
    }

  return e;
}

static void check_time(struct ch_search_s *s)
{
  if (s->poll != NULL)
    {
      s->poll(s->arg);
    }

  if (s->stop != NULL && *s->stop)
    {
      s->stopped = true;
    }

  if (s->maxms != 0 && s->now != NULL && s->info.depth > 0 &&
      s->now() - s->t0 >= s->maxms)
    {
      s->stopped = true;
    }
}

static inline void count_node(struct ch_search_s *s)
{
  if ((++s->nodes & (POLL_NODES - 1)) == 0)
    {
      check_time(s);
    }
}

/* Repetitions: the same position since the last irreversible move, with
 * the same side to move (every other one)
 */

static bool repeated(struct ch_search_s *s)
{
  int n = s->pos.halfmove;
  int i;

  for (i = s->nhist - 2; i >= 0 && i >= s->nhist - n; i -= 2)
    {
      if (s->hist[i] == s->pos.hash)
        {
          return true;
        }
    }

  return false;
}

/* The table: mate scores are kept relative to the position */

static struct ch_tt_s *tt_probe(struct ch_search_s *s)
{
  struct ch_tt_s *e;

  if (s->tt == NULL)
    {
      return NULL;
    }

  e = &s->tt[(uint32_t)(((uint64_t)(uint32_t)s->pos.hash * s->ttn) >> 32)];
  return e->key == (uint32_t)(s->pos.hash >> 32) && e->flags != 0 ? e :
         NULL;
}

static void tt_store(struct ch_search_s *s, int ply, int depth, int score,
                     int bound, ch_move_t m, int eval)
{
  struct ch_tt_s *e;
  uint32_t key = (uint32_t)(s->pos.hash >> 32);

  if (s->tt == NULL)
    {
      return;
    }

  e = &s->tt[(uint32_t)(((uint64_t)(uint32_t)s->pos.hash * s->ttn) >> 32)];

  /* Keep a deeper result of this search for another position */

  if (e->key != key && (e->flags >> 2) == s->age && e->depth > depth &&
      bound != TT_EXACT)
    {
      return;
    }

  if (score > CH_MATE - 500)
    {
      score += ply;
    }
  else if (score < -CH_MATE + 500)
    {
      score -= ply;
    }

  if (m != CH_NOMOVE || e->key != key)
    {
      e->move = m != CH_NOMOVE ? ch_pack(m) : 0;
    }

  e->key   = key;
  e->score = (int16_t)score;
  e->eval  = (int16_t)eval;
  e->depth = (uint8_t)(depth < 0 ? 0 : depth);
  e->flags = (uint8_t)(bound | s->age << 2);
}

static int tt_score(int score, int ply)
{
  if (score > CH_MATE - 500)
    {
      return score - ply;
    }

  if (score < -CH_MATE + 500)
    {
      return score + ply;
    }

  return score;
}

/* Order: the table's move, captures (most valuable victim, least
 * valuable attacker), promotions, killers, then by history
 */

static void score_moves(struct ch_search_s *s, ch_move_t *m, int16_t *sc,
                        int n, uint16_t ttmove, int ply)
{
  const struct ch_pos_s *p = &s->pos;
  int i;

  for (i = 0; i < n; i++)
    {
      ch_move_t mv = m[i];
      int from = CH_FROM(mv);
      int to = CH_TO(mv);
      int attacker = CH_TYPE(p->board[from]);

      if (ttmove != 0 && ch_pack(mv) == ttmove)
        {
          sc[i] = S_TTMOVE;
        }
      else if (CH_FLAGS(mv) & CH_F_CAPTURE)
        {
          int victim = (CH_FLAGS(mv) & CH_F_EP) ? CH_PAWN :
                       CH_TYPE(p->board[to]);

          sc[i] = S_CAPTURE + victim * 64 - attacker +
                  (CH_PROMO(mv) == CH_QUEEN ? 512 : 0);
        }
      else if (CH_PROMO(mv) != 0)
        {
          sc[i] = CH_PROMO(mv) == CH_QUEEN ? S_CAPTURE + 300 : -1000;
        }
      else if (ply < CH_MAXPLY && mv == s->killer[ply][0])
        {
          sc[i] = S_KILLER1;
        }
      else if (ply < CH_MAXPLY && mv == s->killer[ply][1])
        {
          sc[i] = S_KILLER2;
        }
      else
        {
          sc[i] = s->history[p->board[from]][CH_SQ64(to)];
        }
    }
}

/* The next best move, by selection */

static ch_move_t next_move(ch_move_t *m, int16_t *sc, int n, int i)
{
  int best = i;
  int j;

  for (j = i + 1; j < n; j++)
    {
      if (sc[j] > sc[best])
        {
          best = j;
        }
    }

  if (best != i)
    {
      ch_move_t tm = m[i];
      int16_t ts = sc[i];

      m[i] = m[best];
      sc[i] = sc[best];
      m[best] = tm;
      sc[best] = ts;
    }

  return m[i];
}

static int quiesce(struct ch_search_s *s, int alpha, int beta, int ply)
{
  struct ch_pos_s *p = &s->pos;
  struct ch_undo_s u;
  ch_move_t *m;
  int16_t *sc;
  int stand;
  int best;
  int check;
  int n;
  int i;

  count_node(s);
  if (s->stopped)
    {
      return 0;
    }

  stand = evaluate(s);
  if (ply >= CH_MAXPLY - 1 || s->sp + CH_MAXMOVES > CH_MOVESTACK)
    {
      return stand;
    }

  if (stand >= beta)
    {
      return stand;
    }

  if (stand > alpha)
    {
      alpha = stand;
    }

  best = stand;
  m  = &s->moves[s->sp];
  sc = &s->scores[s->sp];
  n  = ch_generate(p, m, true);
  if (n == 0)
    {
      return best;
    }

  check = ch_incheck(p, p->side);
  score_moves(s, m, sc, n, 0, CH_MAXPLY);
  s->sp += n;

  for (i = 0; i < n; i++)
    {
      ch_move_t mv = next_move(m, sc, n, i);
      int victim = (CH_FLAGS(mv) & CH_F_EP) ? CH_PAWN :
                   CH_TYPE(p->board[CH_TO(mv)]);
      int score;

      /* Delta pruning: even winning this cannot bring the score up */

      if (CH_PROMO(mv) == 0 && stand + g_value[victim] + 180 <= alpha)
        {
          continue;
        }

      /* Under-promotions are never worth looking at here */

      if (CH_PROMO(mv) != 0 && CH_PROMO(mv) != CH_QUEEN)
        {
          continue;
        }

      if (!make(p, mv, &u, check))
        {
          ch_unmake(p, mv, &u);
          continue;
        }

      score = -quiesce(s, -beta, -alpha, ply + 1);
      ch_unmake(p, mv, &u);
      if (s->stopped)
        {
          break;
        }

      if (score > best)
        {
          best = score;
          if (score > alpha)
            {
              alpha = score;
              if (score >= beta)
                {
                  break;
                }
            }
        }
    }

  s->sp -= n;
  return best;
}

static bool has_pieces(const struct ch_pos_s *p, int side)
{
  return p->npieces[CH_PC(side, CH_KNIGHT)] +
         p->npieces[CH_PC(side, CH_BISHOP)] +
         p->npieces[CH_PC(side, CH_ROOK)] +
         p->npieces[CH_PC(side, CH_QUEEN)] > 0;
}

static int search(struct ch_search_s *s, int alpha, int beta, int depth,
                  int ply, bool null)
{
  struct ch_pos_s *p = &s->pos;
  struct ch_undo_s u;
  struct ch_tt_s *e;
  uint16_t ttmove = 0;
  ch_move_t bestmove = CH_NOMOVE;
  ch_move_t *m;
  int16_t *sc;
  bool pv = beta - alpha > 1;
  bool check;
  int oldalpha = alpha;
  int eval;
  int best = -CH_INF;
  int legal = 0;
  int n;
  int i;

  if (ply > 0)
    {
      if (p->halfmove >= 100 || repeated(s) || ch_insufficient(p))
        {
          return 0;
        }

      /* Mate distance: no line can be better than mating now */

      if (alpha < -CH_MATE + ply)
        {
          alpha = -CH_MATE + ply;
        }

      if (beta > CH_MATE - ply - 1)
        {
          beta = CH_MATE - ply - 1;
        }

      if (alpha >= beta)
        {
          return alpha;
        }
    }

  check = ch_incheck(p, p->side);
  if (check)
    {
      depth++;
    }

  if (depth <= 0)
    {
      return quiesce(s, alpha, beta, ply);
    }

  count_node(s);
  if (s->stopped)
    {
      return 0;
    }

  if (ply >= CH_MAXPLY - 1 || s->sp + CH_MAXMOVES > CH_MOVESTACK)
    {
      return evaluate(s);
    }

  e = tt_probe(s);
  if (e != NULL)
    {
      int ts = tt_score(e->score, ply);
      int bound = e->flags & 3;

      ttmove = e->move;
      if (!pv && e->depth >= depth &&
          (bound == TT_EXACT || (bound == TT_LOWER && ts >= beta) ||
           (bound == TT_UPPER && ts <= alpha)))
        {
          return ts;
        }

      eval = e->eval;
    }
  else
    {
      eval = check ? -CH_INF : evaluate(s);
    }

  if (!check && !pv)
    {
      /* Reverse futility: far above beta, with little depth left */

      if (depth <= 3 && eval - 110 * depth >= beta && !CH_ISMATE(beta))
        {
          return eval;
        }

      /* The null move: passing still keeps us above beta */

      if (null && depth >= 3 && eval >= beta && has_pieces(p, p->side))
        {
          int r = depth >= 7 ? 3 : 2;
          uint8_t ep = p->ep;
          int score;

          s->hist[s->nhist++] = p->hash;
          if (ep != CH_NOSQ)
            {
              p->hash ^= zkey(K_EP + CH_FILE(ep));
              p->ep = CH_NOSQ;
            }

          p->side ^= 1;
          p->hash ^= zkey(K_SIDE);
          p->halfmove++;
          score = -search(s, -beta, -beta + 1, depth - 1 - r, ply + 1,
                          false);
          p->halfmove--;
          s->nhist--;
          p->side ^= 1;
          p->hash ^= zkey(K_SIDE);
          if (ep != CH_NOSQ)
            {
              p->ep = ep;
              p->hash ^= zkey(K_EP + CH_FILE(ep));
            }

          if (s->stopped)
            {
              return 0;
            }

          if (score >= beta)
            {
              return CH_ISMATE(score) ? beta : score;
            }
        }
    }

  m  = &s->moves[s->sp];
  sc = &s->scores[s->sp];
  n  = ch_generate(p, m, false);
  score_moves(s, m, sc, n, ttmove, ply);
  s->sp += n;
  s->hist[s->nhist++] = p->hash;

  for (i = 0; i < n; i++)
    {
      ch_move_t mv = next_move(m, sc, n, i);
      bool quiet = (CH_FLAGS(mv) & CH_F_CAPTURE) == 0 && CH_PROMO(mv) == 0;
      int score;
      int r = 0;

      if (!make(p, mv, &u, check))
        {
          ch_unmake(p, mv, &u);
          continue;
        }

      legal++;

      /* Late quiet moves of a node not in check, not checking: less
       * depth, or none at the frontier if hopeless
       */

      if (quiet && !check && legal > 1 && sc[i] < S_KILLER2)
        {
          bool gives = ch_incheck(p, p->side);

          if (!gives)
            {
              if (!pv && depth <= 2 && eval != -CH_INF &&
                  eval + 90 + 110 * depth <= alpha && !CH_ISMATE(alpha))
                {
                  ch_unmake(p, mv, &u);
                  continue;
                }

              if (depth >= 3 && legal > 3)
                {
                  r = 1 + (legal > 8) + (depth >= 8 && !pv);
                  if (r > depth - 2)
                    {
                      r = depth - 2;
                    }
                }
            }
        }

      if (legal == 1)
        {
          score = -search(s, -beta, -alpha, depth - 1, ply + 1, true);
        }
      else
        {
          score = -search(s, -alpha - 1, -alpha, depth - 1 - r, ply + 1,
                          true);
          if (score > alpha && r > 0)
            {
              score = -search(s, -alpha - 1, -alpha, depth - 1, ply + 1,
                              true);
            }

          if (score > alpha && score < beta)
            {
              score = -search(s, -beta, -alpha, depth - 1, ply + 1, true);
            }
        }

      ch_unmake(p, mv, &u);
      if (s->stopped)
        {
          s->sp -= n;
          s->nhist--;
          return 0;
        }

      if (score > best)
        {
          best = score;
          bestmove = mv;
          if (score > alpha)
            {
              alpha = score;
              if (score >= beta)
                {
                  if (quiet)
                    {
                      int16_t *h = &s->history[p->board[CH_FROM(mv)]]
                                              [CH_SQ64(CH_TO(mv))];

                      if (s->killer[ply][0] != mv)
                        {
                          s->killer[ply][1] = s->killer[ply][0];
                          s->killer[ply][0] = mv;
                        }

                      *h += depth * depth;
                      if (*h > S_HISTMAX)
                        {
                          int a;
                          int b;

                          for (a = 0; a < 16; a++)
                            {
                              for (b = 0; b < 64; b++)
                                {
                                  s->history[a][b] /= 2;
                                }
                            }
                        }
                    }

                  break;
                }
            }
        }
    }

  s->sp -= n;
  s->nhist--;

  if (legal == 0)
    {
      return check ? -CH_MATE + ply : 0;
    }

  tt_store(s, ply, depth, best, best >= beta ? TT_LOWER :
           best > oldalpha ? TT_EXACT : TT_UPPER, bestmove,
           check ? 0 : eval);
  return best;
}

/* The root: every legal move, the best of the last depth first */

static int search_root(struct ch_search_s *s, ch_move_t *m, int n,
                       int alpha, int beta, int depth)
{
  struct ch_pos_s *p = &s->pos;
  struct ch_undo_s u;
  int best = -CH_INF;
  int i;

  s->hist[s->nhist++] = p->hash;
  for (i = 0; i < n; i++)
    {
      int score;

      ch_make(p, m[i], &u);
      if (i == 0)
        {
          score = -search(s, -beta, -alpha, depth - 1, 1, true);
        }
      else
        {
          score = -search(s, -alpha - 1, -alpha, depth - 1, 1, true);
          if (score > alpha && score < beta)
            {
              score = -search(s, -beta, -alpha, depth - 1, 1, true);
            }
        }

      ch_unmake(p, m[i], &u);
      if (s->stopped)
        {
          break;
        }

      if (score > best)
        {
          ch_move_t mv = m[i];

          best = score;

          /* The best first next time */

          memmove(&m[1], &m[0], i * sizeof(m[0]));
          m[0] = mv;
          if (score > alpha)
            {
              alpha = score;
              s->info.best = mv;
              if (score >= beta)
                {
                  break;
                }
            }
        }
    }

  s->nhist--;
  return best;
}

/****************************************************************************
 * Public Functions: the search
 ****************************************************************************/

void ch_tt_clear(struct ch_tt_s *tt, uint32_t n)
{
  if (tt != NULL)
    {
      memset(tt, 0, CH_TT_BYTES(n));
    }
}

ch_move_t ch_search(struct ch_search_s *s)
{
  ch_move_t root[CH_MAXMOVES];
  struct ch_info_s done;
  int maxdepth = s->maxdepth;
  int score = 0;
  int depth;
  int n;

  ch_init();
  s->stopped = false;
  s->nodes   = 0;
  s->sp      = 0;
  s->age    &= 0x3f;
  s->t0      = s->now != NULL ? s->now() : 0;
  memset(s->killer, 0, sizeof(s->killer));
  memset(s->history, 0, sizeof(s->history));
  memset(&s->info, 0, sizeof(s->info));
  memset(&done, 0, sizeof(done));
  if (maxdepth < 1 || maxdepth > CH_MAXPLY - 1)
    {
      maxdepth = CH_MAXPLY - 1;
    }

  if (s->nhist > CH_HISTMAX - CH_MAXPLY - 2)
    {
      memmove(s->hist, s->hist + s->nhist - 100, 100 * sizeof(s->hist[0]));
      s->nhist = 100;
    }

  n = ch_legal(&s->pos, root);
  if (n == 0)
    {
      return CH_NOMOVE;
    }

  s->info.best = root[0];
  done.best = root[0];
  for (depth = 1; depth <= maxdepth; depth++)
    {
      int alpha = -CH_INF;
      int beta = CH_INF;
      int delta = 35;

      if (depth >= 5 && !CH_ISMATE(score))
        {
          alpha = score - delta;
          beta = score + delta;
        }

      for (; ; )
        {
          score = search_root(s, root, n, alpha, beta, depth);
          if (s->stopped)
            {
              break;
            }

          if (score <= alpha)
            {
              alpha = alpha - delta * 3 < -CH_INF ? -CH_INF :
                      alpha - delta * 3;
              delta *= 3;
            }
          else if (score >= beta)
            {
              beta = beta + delta * 3 > CH_INF ? CH_INF : beta + delta * 3;
              delta *= 3;
            }
          else
            {
              break;
            }
        }

      /* A depth left half way: its best move is good if one was found
       * (the last depth's best was searched first)
       */

      if (s->stopped)
        {
          if (s->info.best != done.best && depth > 1)
            {
              done.best = s->info.best;
            }

          break;
        }

      s->info.depth = depth;
      s->info.score = score;
      s->info.best  = root[0];
      s->info.nodes = s->nodes;
      s->info.ms    = s->now != NULL ? s->now() - s->t0 : 0;
      done = s->info;
      if (s->report != NULL)
        {
          s->report(s->arg, &done);
        }

      /* One legal move; a mate found; more than half the time gone */

      if (n == 1 || (CH_ISMATE(score) && depth > 4 &&
                     CH_MATE - abs(score) < depth))
        {
          break;
        }

      if (s->maxms != 0 && done.ms * 2 > s->maxms)
        {
          break;
        }
    }

  done.nodes = s->nodes;
  done.ms    = s->now != NULL ? s->now() - s->t0 : 0;
  s->info    = done;
  return done.best;
}
