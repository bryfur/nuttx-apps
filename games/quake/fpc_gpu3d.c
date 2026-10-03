/****************************************************************************
 * apps/games/quake/fpc_gpu3d.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The world drawn by the GPU (protocol v10, docs/gpu.md "TSPAN").  The
 * software renderer still decides everything: the BSP, the edges, the
 * spans, the surface cache (built and lit here, 8-bit, as before).  What
 * changes is who textures the spans:
 *
 *   - Each surface cache block the GPU needs is uploaded into an atlas in
 *     the GPU's memory (PUT8: the GPU looks the texels up in its palette),
 *     once, and again only when it is rebuilt (its lighting changed).  The
 *     atlas is pages of the GPU's memory, shelves in them; when it is full
 *     it starts again empty (the blocks are uploaded again as they are
 *     used: what was queued before reads the old texels, the GPU keeping
 *     the order).
 *   - D_DrawSurfaces gives each textured surface's spans to the GPU as
 *     TSPAN jobs: per span the texture position every 16 pixels (the
 *     perspective divide), the GPU interpolating in between, into the
 *     canvas being drawn.  The depth is still written here (the models and
 *     particles drawn after it are tested against it).
 *   - Everything else (the sky, water, the models, particles, the status
 *     bar, the console) is drawn into the frame as before, on a view that
 *     was cleared to colour 255 (the one nothing draws: the transparent
 *     colour of Quake's pictures and skins), and the frame goes over the
 *     GPU's world as a keyed PUT8.
 *   - The palette the GPU has is the base one (with the gamma): the damage,
 *     bonus and powerup tints are a BLEND over the canvas, so the atlas's
 *     texels need not change with them.
 *
 * A frame the GPU cannot draw (the underwater warp moves the world's
 * pixels after they are drawn) is drawn here as before.
 ****************************************************************************/

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

#include <syslog.h>

#include "fpc_gpu.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PAGE_W      512                   /* Atlas pages in the GPU (the most) */
#define PAGE_H      512
#define PAGES_MAX   32
#define ATLAS_MAX   (4096 * 1024)         /* Bytes of the GPU's memory at most */
#define SHELVES     64                    /* A page's at most */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct shelf_s
{
  int16_t y;
  int16_t h;
  int16_t x;                              /* The next free column */
};

struct page_s
{
  int x;                                  /* In the GPU's space */
  int y;
  int w;
  int h;
  int nshelf;
  int top;                                /* The next shelf's y */
  struct shelf_s shelf[SHELVES];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct
{
  qboolean on;                            /* The GPU draws the world */
  qboolean frame;                         /* ... this frame */
  int npage;
  struct page_s page[PAGES_MAX];
  unsigned gen;                           /* The atlas's generation */
  uint32_t uploads, upbytes, flushes, spans, frames, fallbacks;
} g_g3;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* A w x h rectangle in the atlas (x even: PUT8), false if there is no room
 * (the caller starts it again)
 */

static qboolean atlas_alloc(int w, int h, int *x, int *y)
{
  int p;
  int i;

  w = (w + 1) & ~1;
  for (p = 0; p < g_g3.npage; p++)
    {
      struct page_s *pg = &g_g3.page[p];

      for (i = 0; i < pg->nshelf; i++)    /* A shelf it fits, not too tall */
        {
          struct shelf_s *s = &pg->shelf[i];

          if (s->h >= h && s->h <= h + h / 2 + 2 && s->x + w <= pg->w)
            {
              *x = pg->x + s->x;
              *y = pg->y + s->y;
              s->x += w;
              return true;
            }
        }
    }

  for (p = 0; p < g_g3.npage; p++)        /* A new shelf */
    {
      struct page_s *pg = &g_g3.page[p];

      if (pg->nshelf < SHELVES && pg->top + h <= pg->h)
        {
          struct shelf_s *s = &pg->shelf[pg->nshelf++];

          s->y = pg->top;
          s->h = h;
          s->x = w;
          pg->top += h;
          *x = pg->x;
          *y = pg->y + s->y;
          return true;
        }
    }

  return false;
}

static void atlas_flush(void)
{
  int p;

  for (p = 0; p < g_g3.npage; p++)
    {
      g_g3.page[p].nshelf = 0;
      g_g3.page[p].top = 0;
    }

  g_g3.gen++;
  g_g3.flushes++;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* The atlas, at the start (QG_Init): as many pages as the GPU has room for */

void fpcq_g3_init(void)
{
  int p;

  /* (-gpu3d for now: until the GPU reads texels faster than one SDRAM read
   * each, the CPU's two cores draw the world sooner)
   */

  if (!COM_CheckParm("-gpu3d") || fpc_gpu_version() < 10)
    {
      return;
    }

  /* The biggest pages that fit first, then smaller ones in the gaps the
   * desktop's windows leave
   */

  static const int16_t sz[][2] =
  {
    { 512, 512 }, { 512, 256 }, { 256, 256 }, { 256, 128 }
  };

  int bytes = 0;
  int k;

  p = 0;
  for (k = 0; k < 4 && p < PAGES_MAX && bytes < ATLAS_MAX; k++)
    {
      while (p < PAGES_MAX && bytes < ATLAS_MAX &&
             fpc_gpu_game_alloc(sz[k][0], sz[k][1], &g_g3.page[p].x,
                                &g_g3.page[p].y) >= 0)
        {
          g_g3.page[p].w = sz[k][0];
          g_g3.page[p].h = sz[k][1];
          bytes += sz[k][0] * sz[k][1] * 2;
          p++;
        }
    }

  g_g3.npage = p;
  g_g3.gen = 1;
  g_g3.on = p > 0;
  syslog(LOG_INFO, "quake: %s (%d atlas pages, %d KB of the GPU's memory)\n",
         g_g3.on ? "the GPU draws the world" : "no room for the GPU's atlas",
         p, bytes / 1024);
}

void fpcq_g3_report(void)
{
  if (g_g3.on)
    {
      printf("quake: GPU world in %u of %u frames: %u spans, %u uploads "
             "(%u KB), %u atlas flushes\n", (unsigned)g_g3.frames,
             (unsigned)(g_g3.frames + g_g3.fallbacks), (unsigned)g_g3.spans,
             (unsigned)g_g3.uploads, (unsigned)(g_g3.upbytes / 1024),
             (unsigned)g_g3.flushes);
    }
}

/* A frame starts (R_SetupFrame): the GPU's, unless the view is warped;
 * its view cleared to the key for what is drawn here
 */

void fpcq_g3_frame(qboolean warp)
{
  int y;

  g_g3.frame = g_g3.on && !warp;
  if (!g_g3.frame)
    {
      g_g3.fallbacks += g_g3.on;
      return;
    }

  g_g3.frames++;
  for (y = 0; y < r_refdef.vrect.height; y++)
    {
      memset(vid.buffer + (r_refdef.vrect.y + y) * vid.rowbytes +
             r_refdef.vrect.x, 255, r_refdef.vrect.width);
    }
}

qboolean fpcq_g3_drawing(void)
{
  return g_g3.frame;
}

/* The frame has gone (QG_DrawFrame) */

void fpcq_g3_end(void)
{
  g_g3.frame = false;
}

/* A surface cache block about to be textured from: uploaded if it is not
 * in the atlas as it is; false if it cannot be (drawn here then)
 */

FPCQ_FAST qboolean fpcq_g3_surface(surfcache_t *c)
{
  const uint16_t *lut;
  int x;
  int y;

  if (c->gen == g_g3.gen && !c->gdirty)
    {
      return true;
    }

  if (c->width > PAGE_W || c->height > PAGE_H || (c->width & 1))
    {
      return false;
    }

  if (c->gen != g_g3.gen)
    {
      if (!atlas_alloc(c->width, c->height, &x, &y))
        {
          atlas_flush();
          if (!atlas_alloc(c->width, c->height, &x, &y))
            {
              return false;
            }
        }

      c->gx = x;
      c->gy = y;
      c->gen = g_g3.gen;
    }

  lut = fpcq_gpu_lut();                   /* (NULL: the GPU has it) */
  fpc_gpu_put8(c->gx, c->gy, c->width, c->height, c->data, c->width, lut);
  c->gdirty = 0;
  g_g3.uploads++;
  g_g3.upbytes += c->width * c->height;
  return true;
}

/* A textured surface's spans as TSPAN jobs into the canvas being drawn,
 * from the gradients D_CalcGradients set (as D_DrawSpans8 reads them)
 */

FPCQ_FAST void fpcq_g3_spans(espan_t *pspan, surfcache_t *c)
{
  dspanctx_t g;

  D_SpanCtx(&g);
  fpcq_g3_spans_ctx(pspan, c, &g);
}

/* ... from gradients captured when the surface was queued (the other
 * core: fpc_drawq.c)
 */

FPCQ_FAST void fpcq_g3_spans_ctx(espan_t *pspan, surfcache_t *c,
                                 const dspanctx_t *g)
{
  int cx;
  int cy;
  const float d_sdivzstepu = g->sdivzstepu, d_tdivzstepu = g->tdivzstepu;
  const float d_zistepu = g->zistepu;
  const float d_sdivzstepv = g->sdivzstepv, d_tdivzstepv = g->tdivzstepv;
  const float d_zistepv = g->zistepv;
  const float d_sdivzorigin = g->sdivzorigin, d_tdivzorigin = g->tdivzorigin;
  const float d_ziorigin = g->ziorigin;
  const fixed16_t sadjust = g->sadjust, tadjust = g->tadjust;
  const fixed16_t bbextents = g->bbextents, bbextentt = g->bbextentt;
  float sdivz16 = d_sdivzstepu * 16;
  float tdivz16 = d_tdivzstepu * 16;
  float zi16 = d_zistepu * 16;

  fpcq_canvas(&cx, &cy);
  fpc_gpu_jobs_lock();                    /* (the jobs written into the batch) */
  do
    {
      int count = pspan->count;
      int np = (count + 15) / 16 + 1;
      uint32_t *j;
      float du = (float)pspan->u;
      float dv = (float)pspan->v;
      float sdivz = d_sdivzorigin + dv * d_sdivzstepv + du * d_sdivzstepu;
      float tdivz = d_tdivzorigin + dv * d_tdivzstepv + du * d_tdivzstepu;
      float zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
      int k;

      j = fpc_gpu_jobs_room(5 + np);
      j[0] = FPC_GPU_JOB_TSPAN;
      j[1] = (uint32_t)(cx + pspan->u) | (uint32_t)(cy + pspan->v) << 16;
      j[2] = (uint32_t)count | 1u << 16;
      j[3] = (uint32_t)c->gx | (uint32_t)c->gy << 16;
      j[4] = 255;
      for (k = 0; k < np; k++)
        {
          float z = (float)0x10000 / zi;   /* 16.16, as D_DrawSpans8 */
          int s = (int)(sdivz * z) + sadjust;
          int t = (int)(tdivz * z) + tadjust;

          s = s < 0 ? 0 : s > bbextents ? bbextents : s;
          t = t < 0 ? 0 : t > bbextentt ? bbextentt : t;
          j[5 + k] = (uint32_t)(s >> 12) | (uint32_t)(t >> 12) << 16;
          sdivz += sdivz16;
          tdivz += tdivz16;
          zi += zi16;
        }

      g_g3.spans++;
    }
  while ((pspan = pspan->pnext) != NULL);

  fpc_gpu_jobs_unlock();
}

/* A surface the atlas can take (its block at miplevel): the biggest
 * page's size, an even width (PUT8)
 */

FPCQ_FAST qboolean fpcq_g3_fits(msurface_t *pface, int miplevel)
{
  int w = pface->extents[0] >> miplevel;
  int h = pface->extents[1] >> miplevel;

  return (w & 1) == 0 && w <= g_g3.page[0].w && h <= g_g3.page[0].h;
}
