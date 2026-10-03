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
 ****************************************************************************/

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <syslog.h>

#include <nuttx/kmalloc.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DQ_MOST     128                   /* Entries, a power of two */
#define DQ_LEAST    16
#define DQ_STACK    2048                  /* The drawer's */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct dq_s
{
  dspanctx_t c;
  espan_t *spans;
  const byte *from;                       /* The cache block it reads */
  const byte *to;
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
  bool quit;
  bool tried;
  pthread_t thread;
  void *stack;                            /* The game's heap: the PSRAM */
  sem_t go;
} g_dq __attribute__((section(".sram.quake")));   /* In the SRAM: spun on
                                                   * by both cores, and
                                                   * exclusive access (the
                                                   * claim) does not work
                                                   * through the XIP cache */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The next entry, if there is one, drawn by this core.  While the game
 * notes entries only the drawer takes them, in order: an entry's slot is
 * free again once done has passed it.  The game takes them too once it has
 * noted the last (fpcq_dq_wait()).
 */

FPCQ_FAST static qboolean dq_take(void)
{
  uint32_t c = __atomic_load_n(&g_dq.claim, __ATOMIC_ACQUIRE);
  const struct dq_s *e;

  if (c == __atomic_load_n(&g_dq.head, __ATOMIC_ACQUIRE) ||
      !__atomic_compare_exchange_n(&g_dq.claim, &c, c + 1, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    {
      return false;
    }

  e = &g_dq.ring[c & (g_dq.size - 1)];
  D_DrawSpans8Ctx(&e->c, e->spans);
  D_DrawZSpansCtx(&e->c, e->spans);
  __atomic_add_fetch(&g_dq.done, 1, __ATOMIC_RELEASE);
  return true;
}

/* The drawer: asleep between batches, spinning within one */

FPCQ_FAST static void *dq_main(void *arg)
{
  (void)arg;
  for (; ; )
    {
      sem_wait(&g_dq.go);
      for (; ; )
        {
          if (dq_take())
            {
              continue;
            }

          if (__atomic_load_n(&g_dq.quit, __ATOMIC_RELAXED))
            {
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

  while (h - __atomic_load_n(&g_dq.done, __ATOMIC_ACQUIRE) >= g_dq.size);
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
  sem_init(&g_dq.go, 0, 0);

  CPU_ZERO(&cpus);
  CPU_SET(0, &cpus);
  sched_setaffinity(0, sizeof(cpus), &cpus);

  pthread_getschedparam(pthread_self(), &policy, &param);
  param.sched_priority--;
  g_dq.stack = malloc(DQ_STACK);
  pthread_attr_init(&attr);
  if (g_dq.stack != NULL)
    {
      pthread_attr_setstack(&attr, g_dq.stack, DQ_STACK);
    }

  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, policy);
  pthread_attr_setschedparam(&attr, &param);
  CPU_ZERO(&cpus);
  CPU_SET(1, &cpus);
  pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
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
         "surfaces queued at most\n", (unsigned)g_dq.size);
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
  dq_push();
  return true;
}

/* The batch drawn (D_DrawSurfaces' end) */

FPCQ_FAST void fpcq_dq_wait(void)
{
  if (!g_dq.batch)
    {
      return;
    }

  __atomic_store_n(&g_dq.ending, true, __ATOMIC_RELEASE);
  while (dq_take());
  while (__atomic_load_n(&g_dq.done, __ATOMIC_ACQUIRE) != g_dq.head);
  g_dq.batch = false;
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

  fpcq_dq_wait();
  __atomic_store_n(&g_dq.quit, true, __ATOMIC_RELEASE);
  sem_post(&g_dq.go);
  pthread_join(g_dq.thread, NULL);
  sem_destroy(&g_dq.go);
  free(g_dq.stack);
  g_dq.stack = NULL;
  kmm_free(g_dq.ring);
  g_dq.ring = NULL;
}
