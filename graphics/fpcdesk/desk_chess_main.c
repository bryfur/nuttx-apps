/****************************************************************************
 * apps/graphics/fpcdesk/desk_chess_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * chess: the Chess application's engine from the command line.
 *
 *   chess perft              the standard positions' move counts, checked
 *   chess perft DEPTH [FEN]  counts for one position, by first move
 *   chess bench [DEPTH]      searches of a few positions: nodes a second
 *   chess go DEPTH|Nms [FEN] the engine's move, and what it saw
 *
 * Plain C: it also builds on a PC with desk_chess_engine.c
 * (cc -O2 -DCHESS_HOST desk_chess_main.c desk_chess_engine.c).
 ****************************************************************************/

#ifndef CHESS_HOST
#  include <nuttx/config.h>
#endif

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "desk_chess.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_FPCDESK_CHESS_TT_KB
#  define TT_KB         CONFIG_FPCDESK_CHESS_TT_KB
#else
#  define TT_KB         512
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct perft_s
{
  const char *name;
  const char *fen;
  uint64_t    counts[5];                 /* Depth 1 .. */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The positions every move generator is checked against (the Chess
 * Programming Wiki's "Perft Results"), to the depths the simulator counts
 * in a few seconds
 */

static const struct perft_s g_perft[] =
{
  {
    "start",
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    { 20, 400, 8902, 197281 }
  },
  {
    "kiwipete",
    "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    { 48, 2039, 97862 }
  },
  {
    "position 3",
    "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
    { 14, 191, 2812, 43238 }
  },
  {
    "position 4",
    "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
    { 6, 264, 9467 }
  },
  {
    "position 5",
    "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
    { 44, 1486, 62379 }
  },
  {
    "position 6",
    "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
    { 46, 2079, 89890 }
  },
};

/* Benchmark positions: the start, a middle game, an ending */

static const char * const g_bench[] =
{
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
  "r1bq1rk1/pp2bppp/2n1pn2/3p4/2PP4/2N1PN2/PP2BPPP/R2QKB1R w KQ - 0 8",
  "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
  "8/pp3k2/2p1p1p1/3pP1P1/3P4/2P2K2/PP6/8 w - - 0 1",
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int perft_all(void)
{
  struct ch_pos_s p;
  uint64_t nodes = 0;
  uint32_t t0 = now_ms();
  int bad = 0;
  int i;
  int d;

  for (i = 0; i < (int)(sizeof(g_perft) / sizeof(g_perft[0])); i++)
    {
      if (!ch_setfen(&p, g_perft[i].fen))
        {
          printf("perft %s: bad FEN\n", g_perft[i].name);
          bad++;
          continue;
        }

      for (d = 1; d <= 5 && g_perft[i].counts[d - 1] != 0; d++)
        {
          uint64_t n = ch_perft(&p, d);
          bool ok = n == g_perft[i].counts[d - 1];

          printf("perft %-10s %d %9" PRIu64 " %s\n", g_perft[i].name, d, n,
                 ok ? "ok" : "WRONG");
          nodes += n;
          bad += !ok;
        }
    }

  printf("perft: %s, %" PRIu64 " positions in %" PRIu32 " ms\n",
         bad == 0 ? "all ok" : "FAILED", nodes, now_ms() - t0);
  return bad == 0 ? 0 : 1;
}

static int perft_one(int depth, const char *fen)
{
  ch_move_t m[CH_MAXMOVES];
  struct ch_pos_s p;
  struct ch_undo_s u;
  uint64_t total = 0;
  uint32_t t0 = now_ms();
  int n;
  int i;

  if (!ch_setfen(&p, fen))
    {
      printf("chess: bad FEN\n");
      return 1;
    }

  n = ch_legal(&p, m);
  for (i = 0; i < n; i++)
    {
      char buf[8];
      uint64_t c;

      ch_make(&p, m[i], &u);
      c = depth > 1 ? ch_perft(&p, depth - 1) : 1;
      ch_unmake(&p, m[i], &u);
      ch_coord(m[i], buf);
      printf("%s: %" PRIu64 "\n", buf, c);
      total += c;
    }

  printf("perft %d: %" PRIu64 " in %" PRIu32 " ms\n", depth, total,
         now_ms() - t0);
  return 0;
}

static void report(void *arg, const struct ch_info_s *info)
{
  char buf[8];

  if (arg != NULL)
    {
      ch_coord(info->best, buf);
      printf("  depth %2d score %5d nodes %8" PRIu32 " %6" PRIu32
             " ms  %s\n", info->depth, info->score, info->nodes, info->ms,
             buf);
    }
}

static int go(struct ch_search_s *s, const char *fen, int depth,
              uint32_t ms, bool verbose)
{
  char san[12];
  ch_move_t m;

  if (!ch_setfen(&s->pos, fen))
    {
      printf("chess: bad FEN\n");
      return -1;
    }

  s->maxdepth = depth;
  s->maxms    = ms;
  s->nhist    = 0;
  s->now      = now_ms;
  s->report   = report;
  s->arg      = verbose ? s : NULL;
  m = ch_search(s);
  if (m == CH_NOMOVE)
    {
      printf("no move\n");
      return 0;
    }

  ch_san(&s->pos, m, san);
  if (verbose)
    {
      printf("bestmove %s (depth %d, score %d, %" PRIu32 " nodes, %" PRIu32
             " ms)\n", san, s->info.depth, s->info.score, s->info.nodes,
             s->info.ms);
    }

  return 0;
}

static void usage(void)
{
  printf("usage: chess perft [DEPTH [FEN]]\n"
         "       chess bench [DEPTH]\n"
         "       chess go DEPTH|MSms [FEN]\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  static const char *start =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
  struct ch_search_s *s;
  int ret = 0;

  ch_init();
  if (argc < 2)
    {
      usage();
      return 1;
    }

  if (strcmp(argv[1], "perft") == 0)
    {
      if (argc == 2)
        {
          return perft_all();
        }

      return perft_one(atoi(argv[2]), argc > 3 ? argv[3] : start);
    }

  if (strcmp(argv[1], "bench") != 0 && strcmp(argv[1], "go") != 0)
    {
      usage();
      return 1;
    }

  s = calloc(1, sizeof(*s));
  if (s == NULL)
    {
      printf("chess: no memory\n");
      return 1;
    }

  s->ttn = TT_KB * 1024 / CH_TT_BYTES(1);
  s->tt  = malloc(CH_TT_BYTES(s->ttn));
  if (s->tt == NULL)
    {
      s->ttn = 0;
    }

  if (strcmp(argv[1], "bench") == 0)
    {
      int depth = argc > 2 ? atoi(argv[2]) : 7;
      uint64_t nodes = 0;
      uint32_t ms = 0;
      int i;

      for (i = 0; i < (int)(sizeof(g_bench) / sizeof(g_bench[0])); i++)
        {
          ch_tt_clear(s->tt, s->ttn);
          go(s, g_bench[i], depth, 0, false);
          printf("bench %d: depth %d score %5d %8" PRIu32 " nodes %6"
                 PRIu32 " ms\n", i + 1, s->info.depth, s->info.score,
                 s->info.nodes, s->info.ms);
          nodes += s->info.nodes;
          ms += s->info.ms;
        }

      printf("bench: %" PRIu64 " nodes in %" PRIu32 " ms, %" PRIu64
             " nodes/s\n", nodes, ms, ms > 0 ? nodes * 1000 / ms : 0);
    }
  else
    {
      const char *arg = argc > 2 ? argv[2] : "6";
      int len = strlen(arg);
      bool timed = len > 2 && strcmp(arg + len - 2, "ms") == 0;

      ch_tt_clear(s->tt, s->ttn);
      ret = go(s, argc > 3 ? argv[3] : start, timed ? 0 : atoi(arg),
               timed ? (uint32_t)atoi(arg) : 0, true) < 0;
    }

  free(s->tt);
  free(s);
  return ret;
}
