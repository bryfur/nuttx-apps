/****************************************************************************
 * apps/games/quake/fpc_drawq.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The world's spans on the other core.  Drawing a surface's spans (its
 * texture and its depth) takes the renderer about a quarter of its time and
 * decides nothing: D_DrawSurfaces notes each textured surface here (its
 * spans, its surface cache block and its gradients, dspanctx_t) and a thread
 * on the other core draws them, while the game builds the next surfaces'
 * cache blocks and draws the sky and the water itself.  A batch's surfaces
 * cover pixels of their own (the edges were sorted), so nothing is drawn
 * over anything else whatever the order.  D_DrawSurfaces waits for the last
 * of them before it returns, drawing what is still queued itself (either
 * core takes the next entry): its spans are reused after it, and the
 * models and particles drawn after it read the depth.  The surface cache gives
 * nothing back that a queued surface still reads (fpcq_dq_guard(), from
 * D_SCAlloc).  The game stays on the first core.
 *
 * Like NXDoom's draw queue (r_draw.c): the ring in the SRAM, spun on by
 * both cores, the drawer asleep between batches.
 *
 * D_DrawSurfaces queues the surfaces whose cache blocks are built first, so
 * that the drawer has spans to draw while the game builds the others; and
 * the drawer, whenever its queue is empty, draws columns of the surface the
 * game is building (fpcq_cols_draw(): a surface's columns of blocks are
 * each their own pixels, the light grid and the texture are read only),
 * on its own interpolators.
 *
 * With the GPU drawing the world (fpc_gpu3d.c) the other core builds the
 * surfaces' cache blocks, uploads them and gives the GPU their spans
 * (fpcq_g3_queue()), while the game draws the models, particles and the
 * status bar into the frame: those entries draw nothing into the frame,
 * so D_DrawSurfaces does not wait for them, the frame's trip to the GPU
 * does (fpcq_dq_wait()).  Their spans are copied (D_DrawSurfaces' are
 * reused) and the gradients and texture frame taken as they were.  Only
 * the other core builds surfaces then: the game's own (a surface the atlas
 * cannot take) waits for it first (fpcq_g3_cpu()).
 ****************************************************************************/

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <syslog.h>

#include <nuttx/atomic.h>
#include <nuttx/kmalloc.h>

#include "fpc_gpu.h"
#include "fpc_sio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DQ_MOST     128                   /* Entries, a power of two */
#define DQ_LEAST    16
#define DQ_STACK    2048                  /* The drawer's */
#define DQ_SPANS    8192                  /* Spans copied for the GPU's entries */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct dq_s
{
  dspanctx_t c;
  espan_t *spans;
  const byte *from;                       /* The cache block it reads */
  const byte *to;
  qboolean zonly;                         /* The depth only (the GPU textures it) */
  msurface_t *g3face;                     /* The GPU's: built, uploaded, its spans given */
  int g3mip;
  texture_t *g3tex;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct
{
  struct dq_s *ring;                      /* kmm_malloc(): the SRAM */
  uint32_t size;                          /* Entries, a power of two */
  uint32_t head;                          /* The game's: entries noted */
  uint32_t claim;                         /* The next to draw (either core) */
  uint32_t done;                          /* Entries drawn */
  bool batch;                             /* In D_DrawSurfaces */
  bool ending;                            /* ... its end: nothing more */
  bool cpu;                               /* Entries that draw into the frame */
  espan_t *spans;                         /* The GPU's entries' spans (the game's heap) */
  int nspans;
  bool quit;
  bool tried;
  const rcols_t *cols;                    /* The surface being built (fpcq_cols_draw()) */
  uint32_t colword;                       /* ... its number << 16 | the next column (0xffff: none) */
  uint32_t coldone;                       /* ... columns drawn */
  uint16_t colgen;                        /* The game's: the next build's number */
  bool nohelp;                            /* -nohelp */
  pthread_t thread;
  void *stack;                            /* The game's heap: the PSRAM */
  sem_t go;
} g_dq FPC_SRAM_DATA("quake");                    /* In the SRAM: spun on
                                                   * by both cores */

/* g_dq's read-modify-writes (the claim, the counts, the column word) are
 * <nuttx/atomic.h>'s, which look at the address: LDREX/STREX in the SRAM,
 * SIO spinlock 31 elsewhere.  The loader puts .sram.quake in the PSRAM
 * when the SRAM is short (fpc_modheap.c), and there an exclusive store
 * always fails: the compiler's __atomic read-modify-writes spun forever.
 * Loads stay __atomic_load_n (plain loads, anywhere); the column word's
 * stores race its compare-exchange, so they take the same way.
 */

struct dqt_s
{
  uint64_t build, upload, spans;          /* us: the GPU's entries' parts */
};

static struct dqt_s g_dqt;

unsigned int fpcq_tsum[FPCQ_T_N];
unsigned int fpcq_xmisses;
unsigned int fpcq_xipcount;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The next entry, if there is one, drawn by this core.  While the game
 * notes entries only the drawer takes them, in order: an entry's slot is
 * free again once done has passed it.  The game takes them too once it has
 * noted the last (fpcq_dq_wait()).
 */

FPCQ_FAST static qboolean dq_take(qboolean game)
{
  uint32_t c = __atomic_load_n(&g_dq.claim, __ATOMIC_ACQUIRE);
  const struct dq_s *e;
  unsigned int t0;

  if (c == __atomic_load_n(&g_dq.head, __ATOMIC_ACQUIRE) ||
      (game && g_dq.ring[c & (g_dq.size - 1)].g3face != NULL) ||   /* (the drawer's only) */
      !atomic_cmpxchg(&g_dq.claim, &c, c + 1))
    {
      return false;
    }

  e = &g_dq.ring[c & (g_dq.size - 1)];
  if (e->g3face != NULL)
    {
      uint32_t t0 = fpc_gpu_game_us();
      surfcache_t *sc = D_CacheSurfaceTex(e->g3face, e->g3mip, e->g3tex);
      uint32_t t1 = fpc_gpu_game_us();
      qboolean ok = fpcq_g3_surface(sc);
      uint32_t t2 = fpc_gpu_game_us();

      if (ok)
        {
          fpcq_g3_spans_ctx(e->spans, sc, &e->c);
        }

      g_dqt.build += t1 - t0;
      g_dqt.upload += t2 - t1;
      g_dqt.spans += fpc_gpu_game_us() - t2;

      atomic_add(&g_dq.done, 1);
      return true;
    }

  t0 = fpcq_us();
  if (!e->zonly)
    {
      D_DrawSpans8Ctx(&e->c, e->spans);
    }

  D_DrawZSpansCtx(&e->c, e->spans);
  fpcq_tsum[game ? FPCQ_T_SPANS0 : FPCQ_T_SPANS1] += fpcq_us() - t0;
  atomic_add(&g_dq.done, 1);
  return true;
}

/* A column of the surface the game is building, if there is one not taken
 * (fpcq_cols_draw()): its number and the next column are one word, so a
 * column of a build that has ended is never taken
 */

FPCQ_FAST static qboolean dq_column(qboolean game)
{
  uint32_t w = __atomic_load_n(&g_dq.colword, __ATOMIC_ACQUIRE);
  const rcols_t *c;
  uint32_t t0;

  if ((w & 0xffff) == 0xffff)
    {
      return false;
    }

  c = g_dq.cols;
  if ((int)(w & 0xffff) >= c->n ||
      !atomic_cmpxchg(&g_dq.colword, &w, w + 1))
    {
      return false;
    }

  t0 = fpcq_us();
  R_DrawSurfaceColumn(c, w & 0xffff);
  fpcq_tsum[game ? FPCQ_T_COLS0 : FPCQ_T_COLS1] +=
    fpcq_us() - t0;
  atomic_add(&g_dq.coldone, 1);
  return true;
}

/* The drawer: asleep between batches, spinning within one */

FPCQ_FAST static void *dq_main(void *arg)
{
  (void)arg;
  fpcq_sio_claim("the drawer");           /* (pinned to its core) */
  for (; ; )
    {
      sem_wait(&g_dq.go);
      for (; ; )
        {
          if (dq_take(false) || dq_column(false))
            {
              continue;
            }

          if (__atomic_load_n(&g_dq.quit, __ATOMIC_RELAXED))
            {
              fpcq_sio_release();
              return NULL;
            }

          if (__atomic_load_n(&g_dq.ending, __ATOMIC_ACQUIRE) &&
              __atomic_load_n(&g_dq.claim, __ATOMIC_ACQUIRE) ==
              __atomic_load_n(&g_dq.head, __ATOMIC_ACQUIRE))
            {
              break;
            }
        }
    }
}

/* The next free entry (waiting for the drawer while the ring is full) */

FPCQ_FAST static struct dq_s *dq_slot(void)
{
  uint32_t h = g_dq.head;

  if (h - __atomic_load_n(&g_dq.done, __ATOMIC_ACQUIRE) >= g_dq.size)
    {
      uint32_t t0 = fpcq_us();

      while (h - __atomic_load_n(&g_dq.done, __ATOMIC_ACQUIRE) >= g_dq.size);
      fpcq_tsum[FPCQ_T_FULL] += fpcq_us() - t0;
    }

  return &g_dq.ring[h & (g_dq.size - 1)];
}

FPCQ_FAST static void dq_push(void)
{
  __atomic_store_n(&g_dq.head, g_dq.head + 1, __ATOMIC_RELEASE);
}

/* The ring and the drawer, at the first batch (the game on one core, the
 * drawer on the other, a step below the game so that the rest of the
 * system still gets that core when it wants it).  The ring gets what SRAM
 * the game has left; the drawer's stack, which it hardly touches, comes
 * from the game's heap.
 */

static qboolean dq_start(void)
{
  struct sched_param param;
  pthread_attr_t attr;
  cpu_set_t cpus;
  int policy;

  g_dq.tried = true;
  if (COM_CheckParm("-onecore"))
    {
      return false;
    }

  for (g_dq.size = DQ_MOST; g_dq.size >= DQ_LEAST; g_dq.size /= 2)
    {
      g_dq.ring = kmm_malloc(g_dq.size * sizeof(struct dq_s));
      if (g_dq.ring != NULL)
        {
          break;
        }
    }

  if (g_dq.ring == NULL)
    {
      syslog(LOG_WARNING, "quake: no SRAM for the draw queue\n");
      return false;
    }

  g_dq.head = 0;
  g_dq.claim = 0;
  g_dq.done = 0;
  g_dq.quit = false;
  g_dq.colword = 0xffff;                  /* (no build) */
  g_dq.nohelp = COM_CheckParm("-nohelp") != 0;
  sem_init(&g_dq.go, 0, 0);

  CPU_ZERO(&cpus);
  CPU_SET(0, &cpus);
  sched_setaffinity(0, sizeof(cpus), &cpus);
  fpcq_sio_claim("the game");             /* (pinned to the first core) */

  pthread_getschedparam(pthread_self(), &policy, &param);
  param.sched_priority--;
  g_dq.stack = malloc(DQ_STACK);
  g_dq.spans = NULL;                      /* (the GPU's entries': later) */
  g_dq.nspans = 0;

  pthread_attr_init(&attr);
  if (g_dq.stack != NULL)
    {
      pthread_attr_setstack(&attr, g_dq.stack, DQ_STACK);
    }

  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, policy);
  pthread_attr_setschedparam(&attr, &param);
#ifdef CONFIG_SMP
  CPU_ZERO(&cpus);
  CPU_SET(1, &cpus);
  pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
#else
  (void)cpus;                             /* (one CPU: it shares it) */
#endif
  if (pthread_create(&g_dq.thread, &attr, dq_main, NULL) != 0)
    {
      pthread_attr_destroy(&attr);
      sem_destroy(&g_dq.go);
      free(g_dq.stack);
      g_dq.stack = NULL;
      kmm_free(g_dq.ring);
      g_dq.ring = NULL;
      syslog(LOG_WARNING, "quake: no drawer thread\n");
      return false;
    }

  pthread_attr_destroy(&attr);
  pthread_setname_np(g_dq.thread, "quake draw");
  syslog(LOG_INFO, "quake: the world's spans on the other core, %u "
         "surfaces queued at most (SRAM left: %u KB, %u KB at most)\n",
         (unsigned)g_dq.size, (unsigned)(kmm_mallinfo().fordblks / 1024),
         (unsigned)(kmm_mallinfo().mxordblk / 1024));
  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* A textured surface's spans, from D_DrawSurfaces with its gradients set:
 * true if they are queued (D_DrawSpans8's and D_DrawZSpans' work), false
 * if the caller is to draw them
 */

FPCQ_FAST qboolean fpcq_dq_surface(espan_t *spans, surfcache_t *cache)
{
  struct dq_s *e;

  if (d_drawspans != D_DrawSpans8 ||
      (g_dq.ring == NULL && (g_dq.tried || !dq_start())))
    {
      return false;
    }

  if (!g_dq.batch)
    {
      g_dq.batch = true;
      __atomic_store_n(&g_dq.ending, false, __ATOMIC_RELEASE);
      sem_post(&g_dq.go);
    }

  e = dq_slot();
  D_SpanCtx(&e->c);
  e->spans = spans;
  e->from  = (const byte *)cache;
  e->to    = (const byte *)cache + cache->size;
  e->zonly = false;
  e->g3face = NULL;
  g_dq.cpu = true;
  dq_push();
  return true;
}

/* A surface's depth only (its texture the GPU's: fpc_gpu3d.c), queued as
 * fpcq_dq_surface() queues; false if the caller is to draw it
 */

FPCQ_FAST qboolean fpcq_dq_zspans(espan_t *spans)
{
  struct dq_s *e;

  if (g_dq.ring == NULL && (g_dq.tried || !dq_start()))
    {
      return false;
    }

  if (!g_dq.batch)
    {
      g_dq.batch = true;
      __atomic_store_n(&g_dq.ending, false, __ATOMIC_RELEASE);
      sem_post(&g_dq.go);
    }

  e = dq_slot();
  D_SpanCtx(&e->c);
  e->spans = spans;
  e->from  = NULL;                        /* (reads no cache block) */
  e->to    = NULL;
  e->zonly = true;
  e->g3face = NULL;
  dq_push();
  return true;
}

/* A surface for the GPU, from D_DrawSurfaces with its gradients set: built
 * (its cache block), uploaded and its spans given to the GPU by the other
 * core; false if the caller is to do it
 */

FPCQ_FAST qboolean fpcq_g3_queue(espan_t *spans, msurface_t *pface,
                                 int miplevel, texture_t *texture)
{
  struct dq_s *e;
  espan_t *s;
  espan_t *first = NULL;
  espan_t **link = &first;
  int n = 0;

  if (g_dq.ring == NULL && (g_dq.tried || !dq_start()))
    {
      return false;
    }

  /* The copies of the GPU's entries' spans, taken at its first entry:
   * 128 KB of the game's heap, which at 320 x 200 does not have them (it
   * peaks within 100 KB of its end), so the drawer must not need them for
   * the frame's own spans
   */

  if (g_dq.spans == NULL)
    {
      g_dq.spans = malloc(DQ_SPANS * sizeof(espan_t));
      if (g_dq.spans == NULL)
        {
          return false;
        }
    }

  for (s = spans; s != NULL; s = s->pnext)
    {
      n++;
    }

  if (g_dq.nspans + n > DQ_SPANS)
    {
      fpcq_dq_wait();                     /* (the copies free again) */
      if (n > DQ_SPANS)
        {
          return false;
        }
    }

  for (s = spans; s != NULL; s = s->pnext)
    {
      espan_t *d = &g_dq.spans[g_dq.nspans++];

      *d = *s;
      *link = d;
      link = &d->pnext;
    }

  *link = NULL;
  if (!g_dq.batch)
    {
      g_dq.batch = true;
      __atomic_store_n(&g_dq.ending, false, __ATOMIC_RELEASE);
      sem_post(&g_dq.go);
    }

  e = dq_slot();
  D_SpanCtx(&e->c);
  e->spans = first;
  e->from  = NULL;
  e->to    = NULL;
  e->zonly = false;
  e->g3face = pface;
  e->g3mip = miplevel;
  e->g3tex = texture;
  dq_push();
  return true;
}

/* Room in the queue for another surface (D_DrawSurfaces builds one meanwhile
 * if not)
 */

qboolean fpcq_dq_room(void)
{
  return g_dq.ring == NULL ||
         g_dq.head - __atomic_load_n(&g_dq.done, __ATOMIC_ACQUIRE) <
         g_dq.size;
}

/* R_DrawSurface's columns of blocks: shared with the drawer while it is
 * awake (a batch), which takes columns when its queue is empty; each
 * column is its own (its pixels, its part of the light), the light and
 * the texture are read only.  Done when this returns.
 */

void fpcq_cols_draw(const rcols_t *c)
{
  int u;

  if (!g_dq.batch || g_dq.nohelp || c->n < 2 || fpcq_g3_drawing())
    {
      for (u = 0; u < c->n; u++)
        {
          R_DrawSurfaceColumn(c, u);
        }

      return;
    }

  g_dq.cols = c;
  g_dq.coldone = 0;
  atomic_set(&g_dq.colword, (uint32_t)g_dq.colgen << 16);
  while (dq_column(true));
  while (__atomic_load_n(&g_dq.coldone, __ATOMIC_ACQUIRE) != (uint32_t)c->n);
  atomic_set(&g_dq.colword, 0xffff);
  g_dq.colgen = (g_dq.colgen + 1) & 0x7fff;
}

/* D_DrawSurfaces starting: the drawer woken (a batch), to take columns of
 * the surfaces built before any is queued; fpcq_dq_on(): it is (else the
 * order of the surfaces gains nothing)
 */

FPCQ_FAST qboolean fpcq_dq_on(void)
{
  return g_dq.batch;
}


void fpcq_dq_begin(void)
{
  if (d_drawspans != D_DrawSpans8 || fpcq_g3_drawing() ||
      (g_dq.ring == NULL && (g_dq.tried || !dq_start())) || g_dq.batch)
    {
      return;
    }

  g_dq.batch = true;
  g_dq.cpu = true;                        /* (fpcq_dq_batch_end() waits) */
  __atomic_store_n(&g_dq.ending, false, __ATOMIC_RELEASE);
  sem_post(&g_dq.go);
}

/* The game about to build a surface itself: the other core's done first */

FPCQ_FAST void fpcq_g3_cpu(void)
{
  fpcq_dq_wait();
}

/* D_DrawSurfaces' end: drawn, what draws into the frame (the GPU's entries
 * may go on while the game draws its models)
 */

FPCQ_FAST void fpcq_dq_batch_end(void)
{
  if (g_dq.cpu)
    {
      fpcq_dq_wait();
    }
}

/* The batch drawn (D_DrawSurfaces' end) */

FPCQ_FAST void fpcq_dq_wait(void)
{
  if (!g_dq.batch)
    {
      return;
    }

  __atomic_store_n(&g_dq.ending, true, __ATOMIC_RELEASE);
  while (dq_take(true));
  while (__atomic_load_n(&g_dq.done, __ATOMIC_ACQUIRE) != g_dq.head);
  g_dq.batch = false;
  g_dq.cpu = false;
  g_dq.nspans = 0;
}

/* The surface cache about to reuse n bytes at p: the queued surfaces that
 * read any of them drawn first
 */

FPCQ_FAST void fpcq_dq_guard(const void *p, int n)
{
  const byte *from = p;
  const byte *to = from + n;
  uint32_t t;
  uint32_t h = g_dq.head;

  if (!g_dq.batch)
    {
      return;
    }

  for (t = __atomic_load_n(&g_dq.done, __ATOMIC_ACQUIRE); t != h; t++)
    {
      const struct dq_s *e = &g_dq.ring[t & (g_dq.size - 1)];

      if (e->from < to && from < e->to)
        {
          while (__atomic_load_n(&g_dq.done, __ATOMIC_ACQUIRE) != h);
          return;
        }
    }
}

/* The drawer gone (the game's end: it runs the game's code) */

void fpcq_dq_shutdown(void)
{
  if (g_dq.ring == NULL)
    {
      return;
    }

  if (g_dqt.build)
    {
      printf("quake: the other core's GPU work: building %u ms, uploading "
             "%u ms, spans %u ms\n", (unsigned)(g_dqt.build / 1000),
             (unsigned)(g_dqt.upload / 1000), (unsigned)(g_dqt.spans / 1000));
    }

  fpcq_dq_wait();
  __atomic_store_n(&g_dq.quit, true, __ATOMIC_RELEASE);
  sem_post(&g_dq.go);
  pthread_join(g_dq.thread, NULL);
  fpcq_sio_release();                     /* (the game's) */
  sem_destroy(&g_dq.go);
  free(g_dq.stack);
  g_dq.stack = NULL;
  free(g_dq.spans);
  g_dq.spans = NULL;
  kmm_free(g_dq.ring);
  g_dq.ring = NULL;
}

/* Where the timedemo's time went (fpc_quake.h's FPCQ_T_*), us a frame */

void fpcq_times_report(unsigned int frames)
{
  static const char *const names[FPCQ_T_N] =
  {
    "view", "world", "bmodels", "scan", "surfaces", "models", "particles",
    "send", "frame", "spans0", "spans1", "full", "cols0", "cols1"
  };

  int i;

  if (frames == 0)
    {
      return;
    }

  printf("quake: us a frame:");
  for (i = 0; i < FPCQ_T_N; i++)
    {
      printf(" %s %u", names[i], fpcq_tsum[i] / frames);
    }

  if (fpcq_xipcount)
    {
      printf(", XIP misses %u", fpcq_xmisses / frames);
    }

  printf("\n");
}
