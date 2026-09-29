/****************************************************************************
 * apps/graphics/fpcdesk/desk_chess.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chess: the rules and the engine (desk_chess_engine.c), and the pieces'
 * pictures (desk_chess_pieces.c).  Plain C with no NuttX in it, so that it
 * also builds on a PC (perft and benchmarks against the same code).
 *
 * The board is 0x88: square = rank * 16 + file, rank 0 being White's
 * first; a square is off the board when (square & 0x88) != 0.  Each side
 * keeps a list of its pieces' squares.  Moves are pseudo-legal until made
 * (a move that leaves its king in check is taken back).
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_CHESS_H
#define __APPS_GRAPHICS_FPCDESK_DESK_CHESS_H

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CH_WHITE        0
#define CH_BLACK        1

#define CH_EMPTY        0
#define CH_PAWN         1
#define CH_KNIGHT       2
#define CH_BISHOP       3
#define CH_ROOK         4
#define CH_QUEEN        5
#define CH_KING         6

#define CH_PC(side, t)  ((uint8_t)((side) << 3 | (t)))
#define CH_TYPE(pc)     ((pc) & 7)
#define CH_SIDE(pc)     ((pc) >> 3)

#define CH_SQ(f, r)     ((r) * 16 + (f))
#define CH_FILE(sq)     ((sq) & 7)
#define CH_RANK(sq)     ((sq) >> 4)
#define CH_SQ64(sq)     (CH_RANK(sq) * 8 + CH_FILE(sq))
#define CH_OFF(sq)      (((sq) & 0x88) != 0)
#define CH_NOSQ         0x7f             /* No en passant square */

/* A move: from, to (0x88), a promotion's piece type, flags */

typedef uint32_t ch_move_t;

#define CH_F_CAPTURE    1
#define CH_F_EP         2                /* En passant */
#define CH_F_CASTLE     4
#define CH_F_DOUBLE     8                /* A pawn's two squares */

#define CH_MOVE(f, t, p, fl) ((ch_move_t)((f) | (t) << 7 | (p) << 14 | \
                                          (fl) << 17))
#define CH_FROM(m)      ((m) & 0x7f)
#define CH_TO(m)        (((m) >> 7) & 0x7f)
#define CH_PROMO(m)     (((m) >> 14) & 7)
#define CH_FLAGS(m)     ((m) >> 17)
#define CH_NOMOVE       0

/* Castling rights */

#define CH_WK           1
#define CH_WQ           2
#define CH_BK           4
#define CH_BQ           8

#define CH_MAXMOVES     256              /* Moves of a position, at most */
#define CH_MAXPLY       64               /* The search's depth, at most */
#define CH_MOVESTACK    1536             /* Moves of all the search's plies */
#define CH_HISTMAX      (128 + CH_MAXPLY)  /* Positions for repetitions */

#define CH_INF          32000
#define CH_MATE         31000            /* Mate in n plies: CH_MATE - n */
#define CH_ISMATE(s)    ((s) > CH_MATE - 500 || (s) < -CH_MATE + 500)

/* The transposition table's entries (12 bytes) */

#define CH_TT_BYTES(n)  ((n) * 12)

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ch_pos_s
{
  uint8_t  board[128];                   /* Pieces, 0: empty */
  uint8_t  index[128];                   /* A piece's place in its list */
  uint8_t  list[2][16];                  /* Each side's pieces' squares */
  uint8_t  count[2];
  uint8_t  king[2];
  uint8_t  side;                         /* To move */
  uint8_t  castle;                       /* CH_WK ... */
  uint8_t  ep;                           /* En passant target, CH_NOSQ */
  uint8_t  halfmove;                     /* Since a capture or pawn move */
  uint16_t fullmove;
  int16_t  mg[2];                        /* Material and squares, */
  int16_t  eg[2];                        /* middle game and end game */
  uint8_t  phase;                        /* 24: all pieces, 0: none */
  uint8_t  npieces[16];                  /* Pieces of each kind (pc) */
  uint8_t  pawns[2][8];                  /* Each file's pawns: bit rank */
  uint64_t hash;
};

/* What make() changes that unmake() cannot work out */

struct ch_undo_s
{
  uint64_t hash;
  uint8_t  captured;
  uint8_t  castle;
  uint8_t  ep;
  uint8_t  halfmove;
};

struct ch_tt_s
{
  uint32_t key;                          /* The hash's top half */
  uint16_t move;                         /* ch_pack() */
  int16_t  score;
  int16_t  eval;
  uint8_t  depth;
  uint8_t  flags;                        /* Bound (2 bits), age */
};

/* What a search reports after each depth, and at the end */

struct ch_info_s
{
  int       depth;
  int       score;                       /* Centipawns, the mover's view */
  uint32_t  nodes;
  uint32_t  ms;
  ch_move_t best;
};

/* A search.  Big (about 14 KB): the engine's thread keeps it on its own
 * stack, so that on the board it is in the SRAM, not the PSRAM.
 */

struct ch_search_s
{
  struct ch_pos_s pos;

  /* Set by the caller */

  struct ch_tt_s *tt;                    /* The table, or NULL */
  uint32_t  ttn;                         /* Its entries */
  int       maxdepth;                    /* 1 .. CH_MAXPLY - 1 */
  uint32_t  maxms;                       /* Time for the move, 0: none */
  uint32_t  seed;                        /* For noise */
  int       noise;                       /* +- centipawns, 0: none */
  volatile bool *stop;                   /* Set: stop now (may be NULL) */
  uint32_t  (*now)(void);                /* Milliseconds (may be NULL) */
  void      (*poll)(void *arg);          /* Every few thousand nodes */
  void      (*report)(void *arg, const struct ch_info_s *info);
  void     *arg;

  /* The positions before this one since the last irreversible move, for
   * repetitions (the caller's); the search adds its own after them
   */

  uint64_t  hist[CH_HISTMAX];
  int       nhist;

  /* The search's */

  uint8_t   age;
  bool      stopped;
  uint32_t  nodes;
  uint32_t  t0;
  uint32_t  softms;
  int       sp;                          /* Move stack in use */
  ch_move_t moves[CH_MOVESTACK];
  int16_t   scores[CH_MOVESTACK];
  ch_move_t killer[CH_MAXPLY][2];
  int16_t   history[16][64];             /* By piece and target */
  struct ch_info_s info;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Positions */

void ch_init(void);
bool ch_setfen(struct ch_pos_s *p, const char *fen);
void ch_getfen(const struct ch_pos_s *p, char *buf, int size);
void ch_startpos(struct ch_pos_s *p);

/* Moves: pseudo-legal ones, legal ones (m: room for CH_MAXMOVES), making
 * and taking back
 */

int  ch_generate(const struct ch_pos_s *p, ch_move_t *m, bool captures);
int  ch_legal(struct ch_pos_s *p, ch_move_t *m);
bool ch_make(struct ch_pos_s *p, ch_move_t m, struct ch_undo_s *u);
void ch_unmake(struct ch_pos_s *p, ch_move_t m, const struct ch_undo_s *u);
bool ch_incheck(const struct ch_pos_s *p, int side);
bool ch_attacked(const struct ch_pos_s *p, int sq, int by);
uint16_t ch_pack(ch_move_t m);

/* Notation: SAN (p is the position before the move) and coordinates */

void ch_san(struct ch_pos_s *p, ch_move_t m, char *buf);
void ch_coord(ch_move_t m, char *buf);
ch_move_t ch_parse(struct ch_pos_s *p, const char *s);   /* SAN or e2e4 */

/* The rules' draws: not enough to mate (either side) */

bool ch_insufficient(const struct ch_pos_s *p);

/* Counting and evaluating */

uint64_t ch_perft(struct ch_pos_s *p, int depth);
int  ch_evaluate(const struct ch_pos_s *p);

/* Searching: s->pos, the limits and the history set; returns the best
 * move (CH_NOMOVE if there is none), s->info says more
 */

ch_move_t ch_search(struct ch_search_s *s);
void ch_tt_clear(struct ch_tt_s *tt, uint32_t n);

/* The pieces' pictures (desk_chess_pieces.c): for piece type t (CH_PAWN
 * .. CH_KING), four coverage masks of size x size, one after the other in
 * out (4 * size * size bytes): the body, its shading, the outlines and
 * the inner lines, to be blended in that order in the piece's colours.
 * weight: 0 for the board, 1 for small pieces (thicker lines).
 */

#define CH_MASKS        4

void ch_piece_masks(int t, int size, int weight, uint8_t *out);

/* A few more pictures, size x size: a legal move's dot, a capture's
 * ring, the glow of a king in check
 */

#define CH_MARK_DOT     0
#define CH_MARK_RING    1
#define CH_MARK_CHECK   2

void ch_mark_mask(int kind, int size, uint8_t *out);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_CHESS_H */
