/****************************************************************************
 * apps/games/NXDoom/src/doom/r_draw.c
 *
 * SPDX-License-Identifer: GPLv2
 *
 * Copyright(C) 1993-1996 Id Software, Inc.
 * Copyright(C) 2005-2014 Simon Howard
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * DESCRIPTION:
 *  The actual span/column drawing functions.
 *  Here find the main potential for optimization, e.g. inline assembly,
 *  different algorithms.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_GAMES_NXDOOM_DRAWQ
#  include <syslog.h>
#  include <fpc/native/core1.h>           /* The queue, the interpolators */
#endif

#if defined(CONFIG_GAMES_NXDOOM_DRAWQ) && defined(FPC_HAVE_INTERP)
#  include "m_argv.h"
#  define NXDOOM_SIO 1                    /* The drawer on the interpolators */
#endif

#include "deh_main.h"
#include "doomdef.h"

#include "i_system.h"
#include "w_wad.h"
#include "z_zone.h"

#include "r_local.h"

/* Needs access to LFB (guess what). */

#include "v_video.h"

/* State. */

#include "doomstat.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ? */

#define MAXWIDTH SCREENWIDTH              /* (Not 1120 x 832: no more here) */
#define MAXHEIGHT SCREENHEIGHT

/* status bar height at bottom of screen */

#define SBARHEIGHT 32

/* Spectre/Invisibility. */

#define FUZZTABLE 50
#define FUZZOFF (SCREENWIDTH)

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* All drawing to the view buffer is accomplished in this file.
 *
 * The other refresh files only know about coordinates, not the architecture
 * of the frame buffer.
 *
 * Conveniently, the frame buffer is a linear one, and we need only the base
 * address, and the total size == width*height*depth/8.,
 */

byte *viewimage;
int viewwidth;
int scaledviewwidth;
int viewheight;
int viewwindowx;
int viewwindowy;
NXDOOM_SRAM pixel_t *ylookup[MAXHEIGHT];
NXDOOM_SRAM int columnofs[MAXWIDTH];

/* Color tables for different players, translate a limited part to another
 * (color ramps used for  suit colors).
 */

byte translations[3][256];

NXDOOM_SRAM lighttable_t *dc_colormap;
NXDOOM_SRAM int dc_x;
NXDOOM_SRAM int dc_yl;
NXDOOM_SRAM int dc_yh;
NXDOOM_SRAM fixed_t dc_iscale;
NXDOOM_SRAM fixed_t dc_texturemid;

/* first pixel in a column (possibly virtual) */

NXDOOM_SRAM byte *dc_source;

int fuzzoffset[FUZZTABLE] =
{
  FUZZOFF,  -FUZZOFF, FUZZOFF,  -FUZZOFF, FUZZOFF,  FUZZOFF,  -FUZZOFF,
  FUZZOFF,  FUZZOFF,  -FUZZOFF, FUZZOFF,  FUZZOFF,  FUZZOFF,  -FUZZOFF,
  FUZZOFF,  FUZZOFF,  FUZZOFF,  -FUZZOFF, -FUZZOFF, -FUZZOFF, -FUZZOFF,
  FUZZOFF,  -FUZZOFF, -FUZZOFF, FUZZOFF,  FUZZOFF,  FUZZOFF,  FUZZOFF,
  -FUZZOFF, FUZZOFF,  -FUZZOFF, FUZZOFF,  FUZZOFF,  -FUZZOFF, -FUZZOFF,
  FUZZOFF,  FUZZOFF,  -FUZZOFF, -FUZZOFF, -FUZZOFF, -FUZZOFF, FUZZOFF,
  FUZZOFF,  FUZZOFF,  FUZZOFF,  -FUZZOFF, FUZZOFF,  FUZZOFF,  -FUZZOFF,
  FUZZOFF,
};

int fuzzpos = 0;

NXDOOM_SRAM byte *dc_translation;
byte *translationtables;

NXDOOM_SRAM int ds_y;
NXDOOM_SRAM int ds_x1;
NXDOOM_SRAM int ds_x2;

NXDOOM_SRAM lighttable_t *ds_colormap;

NXDOOM_SRAM fixed_t ds_xfrac;
NXDOOM_SRAM fixed_t ds_yfrac;
NXDOOM_SRAM fixed_t ds_xstep;
NXDOOM_SRAM fixed_t ds_ystep;

/* start of a 64*64 tile image */

NXDOOM_SRAM byte *ds_source;

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Backing buffer containing the bezel drawn around the screen and
 * surrounding background.
 */

static pixel_t *background_buffer = NULL;

#ifdef CONFIG_GAMES_NXDOOM_DRAWQ
/****************************************************************************
 * The draw queue (CONFIG_GAMES_NXDOOM_DRAWQ)
 *
 * Drawing the columns and spans takes the renderer about half of its time
 * and decides nothing: in high detail, colfunc and spanfunc only note what
 * to draw in a ring in the SRAM, and a thread on the other core draws it,
 * in the order it was noted (the overdraw, the sprites over the walls and
 * the fuzz reading what is under it all come out as before), while the game
 * walks the BSP, clips and sorts.  r_render_player_view() starts a frame's
 * drawing and waits for the last of it, before the status bar, the menus
 * and the frame's trip to the screen.  Whatever is drawn outside a frame is
 * drawn at once; a block of the zone is purged only once the queue is empty
 * (z_purge_hook: a queued column may be reading it).  The queue is the FPC
 * native tier's (fpc/native/core1.h, Quake's too): only the drawer takes
 * entries, in order.
 ****************************************************************************/

#define DQ_MOST     512                   /* Entries, a power of two */
#define DQ_LEAST    64

enum
{
  DQ_COLUMN,
  DQ_TRANS,
  DQ_FUZZ,
  DQ_SPAN
};

struct dq_s                               /* 32 bytes */
{
  pixel_t *dest;
  const byte *source;
  const lighttable_t *colormap;
  const byte *translation;
  uint32_t frac;                          /* A span's: packed x and y */
  uint32_t step;
  int count;                              /* Pixels - 1 */
  int kind;
};

static struct
{
  struct fpc_core1_s *q;                  /* The queue (its block: the SRAM) */
  bool frame;                             /* Between begin and end */
  bool tried;
  int fuzzpos;                            /* The drawer's */
  bool sio;                               /* The drawer has its core's
                                           * interpolators */
  bool siocheck;                          /* -siocheck */
  uint32_t checked;                       /* ... entries compared */
  uint32_t checkbad;                      /* ... that differed */
} g_dq NXDOOM_SRAM;                       /* (Read by both cores) */

#ifdef NXDOOM_SIO
/* The drawer's columns and spans on its core's SIO interpolators
 * (arch/chip/interp.h), pixel for pixel dq_draw()'s: the texel's address
 * and the step in one single-cycle load (POP2: BASE2, the texture, plus
 * the lanes' shifted and masked accumulators, and each accumulator
 * stepped).  The drawer is pinned to its core and claims the pair
 * (dq_main()); each entry sets up all it uses.
 */

#define I0 RP23XX_INTERP0

static void dq_draw(const struct dq_s *e);

/* A column: lane 0 frac (ADD_RAW, BASE0 the step) giving
 * (frac >> FRACBITS) & 127, bits 16-22 whatever frac's sign, as the C's
 * shift and mask take them
 */

NXDOOM_FAST static void dq_sio_column(const struct dq_s *e)
{
  pixel_t *dest = e->dest;
  int count = e->count;
  const lighttable_t *colormap = e->colormap;

  I0->ctrl[0] = INTERP_SHIFT(FRACBITS) | INTERP_MASK(0, 6) | INTERP_ADD_RAW;
  I0->ctrl[1] = 0;
  I0->accum[0] = e->frac;
  I0->accum[1] = 0;
  I0->base[0] = e->step;
  I0->base[1] = 0;
  I0->base[2] = (uint32_t)e->source;

  while (count >= 3)
    {
      dest[0] = colormap[*(const byte *)I0->pop[2]];
      dest[SCREENWIDTH] = colormap[*(const byte *)I0->pop[2]];
      dest[2 * SCREENWIDTH] = colormap[*(const byte *)I0->pop[2]];
      dest[3 * SCREENWIDTH] = colormap[*(const byte *)I0->pop[2]];
      dest += 4 * SCREENWIDTH;
      count -= 4;
    }

  while (count-- >= 0)
    {
      *dest = colormap[*(const byte *)I0->pop[2]];
      dest += SCREENWIDTH;
    }
}

/* A span: frac packs x (6.10, the top half) and y (6.10, the bottom);
 * lane 0 gives frac >> 26 (x's whole part, bits 26-31) and steps it,
 * lane 1 reads the same accumulator (CROSS_INPUT) for (frac >> 4) & 0x0fc0
 * (y's, bits 10-15, as row * 64); the two fields do not overlap, so their
 * sum is the C's |
 */

/* -siocheck: an entry drawn on the interpolators drawn again by the C
 * (dq_draw()) and the two compared; the counts logged as the drawer ends
 */

static void dq_sio_check(const struct dq_s *e)
{
  pixel_t got[SCREENWIDTH];
  int step = e->kind == DQ_COLUMN ? SCREENWIDTH : 1;
  int i;

  if (e->count >= SCREENWIDTH)
    {
      return;                             /* (not drawn: dq_draw() would) */
    }

  for (i = 0; i <= e->count; i++)
    {
      got[i] = e->dest[i * step];
    }

  dq_draw(e);
  g_dq.checked++;
  for (i = 0; i <= e->count; i++)
    {
      if (got[i] != e->dest[i * step])
        {
          g_dq.checkbad++;
          break;
        }
    }
}

NXDOOM_FAST static void dq_sio_span(const struct dq_s *e)
{
  pixel_t *dest = e->dest;
  int count = e->count;
  const lighttable_t *colormap = e->colormap;

  I0->ctrl[0] = INTERP_SHIFT(26) | INTERP_MASK(0, 5) | INTERP_ADD_RAW;
  I0->ctrl[1] = INTERP_SHIFT(4) | INTERP_MASK(6, 11) | INTERP_CROSS_INPUT;
  I0->accum[0] = e->frac;
  I0->accum[1] = 0;
  I0->base[0] = e->step;
  I0->base[1] = 0;
  I0->base[2] = (uint32_t)e->source;

  while (count >= 3)
    {
      dest[0] = colormap[*(const byte *)I0->pop[2]];
      dest[1] = colormap[*(const byte *)I0->pop[2]];
      dest[2] = colormap[*(const byte *)I0->pop[2]];
      dest[3] = colormap[*(const byte *)I0->pop[2]];
      dest += 4;
      count -= 4;
    }

  while (count-- >= 0)
    {
      *dest++ = colormap[*(const byte *)I0->pop[2]];
    }
}
#endif

NXDOOM_FAST static void dq_draw(const struct dq_s *e)
{
  pixel_t *dest = e->dest;
  int count = e->count;
  uint32_t frac = e->frac;
  uint32_t step = e->step;
  const byte *source = e->source;
  const lighttable_t *colormap = e->colormap;

  switch (e->kind)
    {
      case DQ_COLUMN:
        do
          {
            *dest = colormap[source[(frac >> FRACBITS) & 127]];
            dest += SCREENWIDTH;
            frac += step;
          }
        while (count--);
        break;

      case DQ_TRANS:
        do
          {
            *dest = colormap[e->translation[source[frac >> FRACBITS]]];
            dest += SCREENWIDTH;
            frac += step;
          }
        while (count--);
        break;

      case DQ_FUZZ:
        {
          int pos = g_dq.fuzzpos;

          do
            {
              *dest = colormaps[6 * 256 + dest[fuzzoffset[pos]]];
              if (++pos == FUZZTABLE)
                {
                  pos = 0;
                }

              dest += SCREENWIDTH;
            }
          while (count--);
          g_dq.fuzzpos = pos;
        }
        break;

      case DQ_SPAN:
        do
          {
            *dest++ = colormap[source[((frac >> 4) & 0x0fc0) |
                                      (frac >> 26)]];
            frac += step;
          }
        while (count--);
        break;
    }
}

/* The drawer's interpolators given back as it ends */

static void dq_sio_release(void)
{
#ifdef NXDOOM_SIO
  if (g_dq.sio)
    {
      g_dq.sio = false;
      fpc_interp_release();
      if (g_dq.siocheck)
        {
          syslog(LOG_INFO, "nxdoom: -siocheck: %u columns and spans "
                 "compared, %u differ\n", (unsigned)g_dq.checked,
                 (unsigned)g_dq.checkbad);
        }
    }
#endif
}

/* The drawer's start: pinned to its core, its interpolators, unless -nosio */

static void dq_thread_start(void *arg)
{
  (void)arg;
#ifdef NXDOOM_SIO
  if (m_check_parm("-nosio") <= 0)
    {
      int cpu = fpc_interp_claim();

      g_dq.sio = cpu >= 0;
      g_dq.siocheck = g_dq.sio && m_check_parm("-siocheck") > 0;
      syslog(LOG_INFO, "nxdoom: the drawer %s\n", cpu >= 0 ?
             "on its core's interpolators" : "in plain C (no interpolators)");
    }
#endif
}

static void dq_thread_end(void *arg)
{
  (void)arg;
  dq_sio_release();
}

/* An entry drawn by the drawer, in the order they were noted */

NXDOOM_FAST static void dq_run(void *arg, void *entry, bool game)
{
  const struct dq_s *e = entry;

  (void)arg;
  (void)game;
#ifdef NXDOOM_SIO
  if (g_dq.sio && e->kind == DQ_COLUMN)
    {
      dq_sio_column(e);
      if (g_dq.siocheck)
        {
          dq_sio_check(e);
        }
    }
  else if (g_dq.sio && e->kind == DQ_SPAN)
    {
      dq_sio_span(e);
      if (g_dq.siocheck)
        {
          dq_sio_check(e);
        }
    }
  else
#endif
    {
      dq_draw(e);
    }
}

/* The next free entry (waiting for the drawer while the ring is full) */

NXDOOM_FAST static struct dq_s *dq_slot(void)
{
  return fpc_core1_slot(g_dq.q);
}

/* An entry filled: to the drawer within a frame, else drawn here */

NXDOOM_FAST static void dq_push(struct dq_s *e)
{
  if (g_dq.frame)
    {
      fpc_core1_push(g_dq.q);
    }
  else
    {
      dq_draw(e);
    }
}

/* Everything noted drawn (the zone's purge hook) */

static void dq_wait(void)
{
  fpc_core1_drain(g_dq.q);
}

static void dq_shutdown(void)
{
  if (g_dq.q == NULL)
    {
      return;
    }

#ifdef CONFIG_GAMES_NXDOOM_ZONE
  z_purge_hook = NULL;
#endif
  if (colfunc == r_drawq_column)
    {
      colfunc = basecolfunc = r_draw_column;
      fuzzcolfunc = r_draw_fuzz_column;
      transcolfunc = r_draw_translated_column;
      spanfunc = r_draw_span;
    }

  if (g_dq.frame)
    {
      fpc_core1_wait(g_dq.q, false);
    }

  fpc_core1_stop(g_dq.q);                 /* (the drawer's interpolators) */
  g_dq.q = NULL;
  g_dq.frame = false;
}

/* The queue and the drawer, the first frame (the game on one core, the
 * drawer on the other, a step below the game so that the rest of the
 * system still gets that core when it wants it).  The game leaves the
 * SRAM all but full: the ring gets what there is.
 */

static bool dq_start(void)
{
  struct fpc_core1_config_s cfg =
  {
    .name  = "doom draw",
    .entry = sizeof(struct dq_s),
    .most  = DQ_MOST,
    .least = DQ_LEAST,
    .run   = dq_run,
    .start = dq_thread_start,
    .end   = dq_thread_end,
  };

  g_dq.tried = true;
  g_dq.fuzzpos = 0;
  g_dq.sio = false;
  g_dq.q = fpc_core1_start(&cfg);
  if (g_dq.q == NULL)
    {
      syslog(LOG_WARNING, "nxdoom: no draw queue (SRAM, thread)\n");
      return false;
    }

#ifdef CONFIG_GAMES_NXDOOM_ZONE
  z_purge_hook = dq_wait;
#endif
  i_at_exit(dq_shutdown, true);           /* (before the screen goes) */
  syslog(LOG_INFO, "nxdoom: drawing on the other core, %u entries queued "
         "at most\n", (unsigned)(g_dq.q->mask + 1));
  return true;
}

/* colfunc, fuzzcolfunc, transcolfunc and spanfunc in high detail */

NXDOOM_FAST void r_drawq_column(void)
{
  struct dq_s *e;
  int count = dc_yh - dc_yl;

  if (count < 0)
    {
      return;
    }

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
      i_error("r_draw_column: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }
#endif

  e = dq_slot();
  e->kind = DQ_COLUMN;
  e->dest = ylookup[dc_yl] + columnofs[dc_x];
  e->count = count;
  e->step = dc_iscale;
  e->frac = dc_texturemid + (dc_yl - centery) * dc_iscale;
  e->source = dc_source;
  e->colormap = dc_colormap;
  dq_push(e);
}

NXDOOM_FAST void r_drawq_translated_column(void)
{
  struct dq_s *e;
  int count = dc_yh - dc_yl;

  if (count < 0)
    {
      return;
    }

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
      i_error("r_draw_column: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }
#endif

  e = dq_slot();
  e->kind = DQ_TRANS;
  e->dest = ylookup[dc_yl] + columnofs[dc_x];
  e->count = count;
  e->step = dc_iscale;
  e->frac = dc_texturemid + (dc_yl - centery) * dc_iscale;
  e->source = dc_source;
  e->colormap = dc_colormap;
  e->translation = dc_translation;
  dq_push(e);
}

NXDOOM_FAST void r_drawq_fuzz_column(void)
{
  struct dq_s *e;
  int count;

  if (!dc_yl)
    {
      dc_yl = 1;
    }

  if (dc_yh == viewheight - 1)
    {
      dc_yh = viewheight - 2;
    }

  count = dc_yh - dc_yl;
  if (count < 0)
    {
      return;
    }

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
      i_error("r_draw_fuzz_column: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }
#endif

  e = dq_slot();
  e->kind = DQ_FUZZ;
  e->dest = ylookup[dc_yl] + columnofs[dc_x];
  e->count = count;
  dq_push(e);
}

NXDOOM_FAST void r_drawq_span(void)
{
  struct dq_s *e;

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if (ds_x2 < ds_x1 || ds_x1 < 0 || ds_x2 >= SCREENWIDTH ||
      ds_y < 0 || ds_y >= viewheight)
    {
      i_error("r_draw_span: %i to %i at %i", ds_x1, ds_x2, ds_y);
    }
#endif

  e = dq_slot();
  e->kind = DQ_SPAN;
  e->dest = ylookup[ds_y] + columnofs[ds_x1];
  e->count = ds_x2 - ds_x1;
  e->frac = ((ds_xfrac << 10) & 0xffff0000) | ((ds_yfrac >> 6) & 0x0000ffff);
  e->step = ((ds_xstep << 10) & 0xffff0000) | ((ds_ystep >> 6) & 0x0000ffff);
  e->source = ds_source;
  e->colormap = ds_colormap;
  dq_push(e);
}

/* A frame's drawing: begin it (false: no queue, draw as before) */

bool r_drawq_begin(void)
{
  if (detailshift || (g_dq.q == NULL && (g_dq.tried || !dq_start())))
    {
      return false;                       /* (Low detail: as before) */
    }

  colfunc = basecolfunc = r_drawq_column;
  fuzzcolfunc = r_drawq_fuzz_column;
  transcolfunc = r_drawq_translated_column;
  spanfunc = r_drawq_span;
  g_dq.frame = true;
  fpc_core1_begin(g_dq.q);
  return true;
}

/* ...and wait for its last pixel */

void r_drawq_end(void)
{
  if (!g_dq.frame)
    {
      return;
    }

  g_dq.frame = false;
  fpc_core1_wait(g_dq.q, false);
}
#endif /* CONFIG_GAMES_NXDOOM_DRAWQ */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Source is the top of the column to scale.
 *
 * A column is a vertical slice/span from a wall texture that, given the DOOM
 * style restrictions on the view orientation, will always have constant z
 * depth.
 *
 * Thus a special case loop for very fast rendering can be used. It has also
 * been used with Wolfenstein 3D.
 */

NXDOOM_FAST void r_draw_column(void)
{
  int count;
  pixel_t *dest;
  fixed_t frac;
  fixed_t fracstep;

  count = dc_yh - dc_yl;

  /* Zero length, column does not exceed a pixel. */

  if (count < 0)
    {
      return;
    }

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
      i_error("r_draw_column: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }
#endif

  /* Framebuffer destination address.
   * Use ylookup LUT to avoid multiply with ScreenWidth.
   * Use columnofs LUT for subwindows?
   */

  dest = ylookup[dc_yl] + columnofs[dc_x];

  /* Determine scaling, which is the only mapping to be done. */

  fracstep = dc_iscale;
  frac = dc_texturemid + (dc_yl - centery) * fracstep;

  /* Inner loop that does the actual texture mapping,
   *  e.g. a DDA-lile scaling.
   * This is as fast as it gets.
   */

  do
    {
      /* Re-map color indices from wall texture column
       *  using a lighting/special effects LUT.
       */

      *dest = dc_colormap[dc_source[(frac >> FRACBITS) & 127]];

      dest += SCREENWIDTH;
      frac += fracstep;
    }
  while (count--);
}

/* UNUSED. Loop unrolled. */

#if 0
NXDOOM_FAST void r_draw_column(void)
{
  int count;
  byte *source;
  byte *dest;
  byte *colormap;

  unsigned frac;
  unsigned fracstep;
  unsigned fracstep2;
  unsigned fracstep3;
  unsigned fracstep4;

  count = dc_yh - dc_yl + 1;

  source = dc_source;
  colormap = dc_colormap;
  dest = ylookup[dc_yl] + columnofs[dc_x];

  fracstep = dc_iscale << 9;
  frac = (dc_texturemid + (dc_yl - centery) * dc_iscale) << 9;

  fracstep2 = fracstep + fracstep;
  fracstep3 = fracstep2 + fracstep;
  fracstep4 = fracstep3 + fracstep;

  while (count >= 8)
    {
      dest[0] = colormap[source[frac >> 25]];
      dest[SCREENWIDTH] = colormap[source[(frac + fracstep) >> 25]];
      dest[SCREENWIDTH * 2] = colormap[source[(frac + fracstep2) >> 25]];
      dest[SCREENWIDTH * 3] = colormap[source[(frac + fracstep3) >> 25]];

      frac += fracstep4;

      dest[SCREENWIDTH * 4] = colormap[source[frac >> 25]];
      dest[SCREENWIDTH * 5] = colormap[source[(frac + fracstep) >> 25]];
      dest[SCREENWIDTH * 6] = colormap[source[(frac + fracstep2) >> 25]];
      dest[SCREENWIDTH * 7] = colormap[source[(frac + fracstep3) >> 25]];

      frac += fracstep4;
      dest += SCREENWIDTH * 8;
      count -= 8;
    }

  while (count > 0)
    {
      *dest = colormap[source[frac >> 25]];
      dest += SCREENWIDTH;
      frac += fracstep;
      count--;
    }
}
#endif

NXDOOM_FAST void r_draw_column_low(void)
{
  int count;
  pixel_t *dest;
  pixel_t *dest2;
  fixed_t frac;
  fixed_t fracstep;
  int x;

  count = dc_yh - dc_yl;

  /* Zero length. */

  if (count < 0)
    {
      return;
    }

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
      i_error("r_draw_column: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }
#endif

  /* Blocky mode, need to multiply by 2. */

  x = dc_x << 1;

  dest = ylookup[dc_yl] + columnofs[x];
  dest2 = ylookup[dc_yl] + columnofs[x + 1];

  fracstep = dc_iscale;
  frac = dc_texturemid + (dc_yl - centery) * fracstep;

  do
    {
      /* Hack. Does not work correctly. */

      *dest2 = *dest = dc_colormap[dc_source[(frac >> FRACBITS) & 127]];
      dest += SCREENWIDTH;
      dest2 += SCREENWIDTH;
      frac += fracstep;
    }
  while (count--);
}

/* Framebuffer postprocessing.
 *
 * Creates a fuzzy image by copying pixels from adjacent ones to left and
 * right.
 *
 * Used with an all black colormap, this could create the SHADOW effect, i.e.
 * spectres and invisible players.
 */

NXDOOM_FAST void r_draw_fuzz_column(void)
{
  int count;
  pixel_t *dest;

  /* Adjust borders. Low... */

  if (!dc_yl)
    {
      dc_yl = 1;
    }

  /* .. and high. */

  if (dc_yh == viewheight - 1)
    {
      dc_yh = viewheight - 2;
    }

  count = dc_yh - dc_yl;

  /* Zero length. */

  if (count < 0)
    {
      return;
    }

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
      i_error("r_draw_fuzz_column: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }
#endif

  dest = ylookup[dc_yl] + columnofs[dc_x];

  /* Looks like an attempt at dithering,
   *  using the colormap #6 (of 0-31, a bit
   *  brighter than average).
   */

  do
    {
      /* Lookup framebuffer, and retrieve a pixel that is either one column
       * left or right of the current one. Add index from colormap to index.
       */

      *dest = colormaps[6 * 256 + dest[fuzzoffset[fuzzpos]]];

      /* Clamp table lookup index. */

      if (++fuzzpos == FUZZTABLE)
        {
          fuzzpos = 0;
        }

      dest += SCREENWIDTH;
    }
  while (count--);
}

/* low detail mode version */

void r_draw_fuzz_column_low(void)
{
  int count;
  pixel_t *dest;
  pixel_t *dest2;
  int x;

  /* Adjust borders. Low... */

  if (!dc_yl)
    {
      dc_yl = 1;
    }

  /* .. and high. */

  if (dc_yh == viewheight - 1)
    {
      dc_yh = viewheight - 2;
    }

  count = dc_yh - dc_yl;

  /* Zero length. */

  if (count < 0)
    {
      return;
    }

  /* low detail mode, need to multiply by 2 */

  x = dc_x << 1;

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if ((unsigned)x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
      i_error("r_draw_fuzz_column: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }
#endif

  dest = ylookup[dc_yl] + columnofs[x];
  dest2 = ylookup[dc_yl] + columnofs[x + 1];

  /* Looks like an attempt at dithering, using the colormap #6
   * (of 0-31, a bit brighter than average).
   */

  do
    {
      /* Lookup framebuffer, and retrieve a pixel that is either one column
       * left or right of the current one. Add index from colormap to index.
       */

      *dest = colormaps[6 * 256 + dest[fuzzoffset[fuzzpos]]];
      *dest2 = colormaps[6 * 256 + dest2[fuzzoffset[fuzzpos]]];

      /* Clamp table lookup index. */

      if (++fuzzpos == FUZZTABLE)
        {
          fuzzpos = 0;
        }

      dest += SCREENWIDTH;
      dest2 += SCREENWIDTH;
    }
  while (count--);
}

/* r_draw_translated_column
 *
 * Used to draw player sprites with the green colorramp mapped to others.
 *
 * Could be used with different translation tables, e.g. the lighter colored
 * version of the BaronOfHell, the HellKnight, uses identical sprites, kinda
 * brightened up.
 */

NXDOOM_FAST void r_draw_translated_column(void)
{
  int count;
  pixel_t *dest;
  fixed_t frac;
  fixed_t fracstep;

  count = dc_yh - dc_yl;
  if (count < 0)
    {
      return;
    }

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if ((unsigned)dc_x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
      i_error("r_draw_column: %i to %i at %i", dc_yl, dc_yh, dc_x);
    }

#endif

  dest = ylookup[dc_yl] + columnofs[dc_x];

  /* Looks familiar. */

  fracstep = dc_iscale;
  frac = dc_texturemid + (dc_yl - centery) * fracstep;

  /* Here we do an additional index re-mapping. */

  do
    {
      /* Translation tables are used to map certain colorramps to other ones,
       * used with PLAY sprites.
       *
       * Thus the "green" ramp of the player 0 sprite is mapped to gray, red,
       * black/indigo.
       */

      *dest = dc_colormap[dc_translation[dc_source[frac >> FRACBITS]]];
      dest += SCREENWIDTH;

      frac += fracstep;
    }
  while (count--);
}

void r_draw_translated_column_low(void)
{
  int count;
  pixel_t *dest;
  pixel_t *dest2;
  fixed_t frac;
  fixed_t fracstep;
  int x;

  count = dc_yh - dc_yl;
  if (count < 0)
    {
      return;
    }

  /* low detail, need to scale by 2 */

  x = dc_x << 1;

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if ((unsigned)x >= SCREENWIDTH || dc_yl < 0 || dc_yh >= SCREENHEIGHT)
    {
      i_error("r_draw_column: %i to %i at %i", dc_yl, dc_yh, x);
    }

#endif

  dest = ylookup[dc_yl] + columnofs[x];
  dest2 = ylookup[dc_yl] + columnofs[x + 1];

  /* Looks familiar. */

  fracstep = dc_iscale;
  frac = dc_texturemid + (dc_yl - centery) * fracstep;

  /* Here we do an additional index re-mapping. */

  do
    {
      /* Translation tables are used to map certain colorramps to other ones,
       * used with PLAY sprites.
       *
       * Thus the "green" ramp of the player 0 sprite is mapped to gray, red,
       * black/indigo.
       */

      *dest = dc_colormap[dc_translation[dc_source[frac >> FRACBITS]]];
      *dest2 = dc_colormap[dc_translation[dc_source[frac >> FRACBITS]]];
      dest += SCREENWIDTH;
      dest2 += SCREENWIDTH;

      frac += fracstep;
    }
  while (count--);
}

/* r_init_translation_table
 *
 * Creates the translation tables to map the green color ramp to gray, brown,
 * red.
 *
 * Assumes a given structure of the PLAYPAL. Could be read from a lump
 * instead.
 */

void r_init_translation_table(void)
{
  int i;

  translationtables = z_malloc(256 * 3, PU_STATIC, 0);

  /* translate just the 16 green colors */

  for (i = 0; i < 256; i++)
    {
      if (i >= 0x70 && i <= 0x7f)
        {
          /* map green ramp to gray, brown, red */

          translationtables[i] = 0x60 + (i & 0xf);
          translationtables[i + 256] = 0x40 + (i & 0xf);
          translationtables[i + 512] = 0x20 + (i & 0xf);
        }
      else
        {
          /* Keep all other colors as is. */

          translationtables[i] = translationtables[i + 256] =
              translationtables[i + 512] = i;
        }
    }
}

/* r_draw_span
 *
 * With DOOM style restrictions on view orientation, the floors and ceilings
 * consist of horizontal slices or spans with constant z depth.
 *
 * However, rotation around the world z axis is possible, thus this mapping,
 * while simpler and faster than perspective correct texture mapping, has to
 * traverse the texture at an angle in all but a few cases.
 *
 * In consequence, flats are not stored by column (like walls), and the inner
 * loop has to step in texture space u and v.
 *
 * Draws the actual span.
 */

NXDOOM_FAST void r_draw_span(void)
{
  unsigned int position;
  unsigned int step;
  pixel_t *dest;
  int count;
  int spot;
  unsigned int xtemp;
  unsigned int ytemp;

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if (ds_x2 < ds_x1 || ds_x1 < 0 || ds_x2 >= SCREENWIDTH ||
      ds_y < 0 || ds_y >= viewheight)
    {
      i_error("r_draw_span: %i to %i at %i", ds_x1, ds_x2, ds_y);
    }
#endif

  /* Pack position and step variables into a single 32-bit integer,
   * with x in the top 16 bits and y in the bottom 16 bits.  For
   * each 16-bit part, the top 6 bits are the integer part and the
   * bottom 10 bits are the fractional part of the pixel position.
   */

  position = ((ds_xfrac << 10) & 0xffff0000) |
      ((ds_yfrac >> 6) & 0x0000ffff);
  step = ((ds_xstep << 10) & 0xffff0000) | ((ds_ystep >> 6) & 0x0000ffff);

  dest = ylookup[ds_y] + columnofs[ds_x1];

  /* We do not check for zero spans here? */

  count = ds_x2 - ds_x1;

  do
    {
      /* Calculate current texture index in u,v. */

      ytemp = (position >> 4) & 0x0fc0;
      xtemp = (position >> 26);
      spot = xtemp | ytemp;

      /* Lookup pixel from flat texture tile, re-index using light/colormap.
       */

      *dest++ = ds_colormap[ds_source[spot]];

      position += step;
    }
  while (count--);
}

/* UNUSED. Loop unrolled by 4. */

#if 0
NXDOOM_FAST void r_draw_span(void)
{
  unsigned position;
  unsigned step;

  byte *source;
  byte *colormap;
  pixel_t *dest;

  unsigned count;
  usingned spot;
  unsigned value;
  unsigned temp;
  unsigned xtemp;
  unsigned ytemp;

  position = ((ds_xfrac << 10) & 0xffff0000) | ((ds_yfrac >> 6) & 0xffff);
  step = ((ds_xstep << 10) & 0xffff0000) | ((ds_ystep >> 6) & 0xffff);

  source = ds_source;
  colormap = ds_colormap;
  dest = ylookup[ds_y] + columnofs[ds_x1];
  count = ds_x2 - ds_x1 + 1;

  while (count >= 4)
    {
      ytemp = position >> 4;
      ytemp = ytemp & 4032;
      xtemp = position >> 26;
      spot = xtemp | ytemp;
      position += step;
      dest[0] = colormap[source[spot]];

      ytemp = position >> 4;
      ytemp = ytemp & 4032;
      xtemp = position >> 26;
      spot = xtemp | ytemp;
      position += step;
      dest[1] = colormap[source[spot]];

      ytemp = position >> 4;
      ytemp = ytemp & 4032;
      xtemp = position >> 26;
      spot = xtemp | ytemp;
      position += step;
      dest[2] = colormap[source[spot]];

      ytemp = position >> 4;
      ytemp = ytemp & 4032;
      xtemp = position >> 26;
      spot = xtemp | ytemp;
      position += step;
      dest[3] = colormap[source[spot]];

      count -= 4;
      dest += 4;
    }
  while (count > 0)
    {
      ytemp = position >> 4;
      ytemp = ytemp & 4032;
      xtemp = position >> 26;
      spot = xtemp | ytemp;
      position += step;
      *dest++ = colormap[source[spot]];
      count--;
    }
}
#endif

/* Again.. */

void r_draw_span_low(void)
{
  unsigned int position;
  unsigned int step;
  unsigned int xtemp;
  unsigned int ytemp;
  pixel_t *dest;
  int count;
  int spot;

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if (ds_x2 < ds_x1 || ds_x1 < 0 || ds_x2 >= SCREENWIDTH ||
      ds_y < 0 || ds_y >= viewheight)
    {
      i_error("r_draw_span: %i to %i at %i", ds_x1, ds_x2, ds_y);
    }
#endif

  position = ((ds_xfrac << 10) & 0xffff0000) |
      ((ds_yfrac >> 6) & 0x0000ffff);
  step = ((ds_xstep << 10) & 0xffff0000) | ((ds_ystep >> 6) & 0x0000ffff);

  count = (ds_x2 - ds_x1);

  /* Blocky mode, need to multiply by 2. */

  ds_x1 <<= 1;
  ds_x2 <<= 1;

  dest = ylookup[ds_y] + columnofs[ds_x1];

  do
    {
      /* Calculate current texture index in u,v. */

      ytemp = (position >> 4) & 0x0fc0;
      xtemp = (position >> 26);
      spot = xtemp | ytemp;

      /* Lowres/blocky mode does it twice, while scale is adjusted
       * appropriately.
       */

      *dest++ = ds_colormap[ds_source[spot]];
      *dest++ = ds_colormap[ds_source[spot]];

      position += step;
    }
  while (count--);
}

/* r_init_buffer
 *
 * Creates lookup tables that avoid multiplies and other hazzles for getting
 * the framebuffer address of a pixel to draw.
 */

void r_init_buffer(int width, int height)
{
  int i;

  /* Handle resize,
   *  e.g. smaller view windows
   *  with border and/or status bar.
   */

  viewwindowx = (SCREENWIDTH - width) >> 1;

  /* Column offset. For windows. */

  for (i = 0; i < width; i++)
    {
      columnofs[i] = viewwindowx + i;
    }

  /* Samw with base row offset. */

  if (width == SCREENWIDTH)
    {
      viewwindowy = 0;
    }
  else
    {
      viewwindowy = (SCREENHEIGHT - SBARHEIGHT - height) >> 1;
    }

  /* Preclaculate all row offsets. */

  for (i = 0; i < height; i++)
    {
      ylookup[i] = i_video_buffer + (i + viewwindowy) * SCREENWIDTH;
    }
}

/* r_fill_back_screen
 *
 * Fills the back screen with a pattern for variable screen sizes
 * Also draws a beveled edge.
 */

void r_fill_back_screen(void)
{
  byte *src;
  pixel_t *dest;
  int x;
  int y;
  patch_t *patch;

  /* DOOM border patch. */

  const char *name1 = ("FLOOR7_2");

  /* DOOM II border patch. */

  const char *name2 = ("GRNROCK");

  const char *name;

  /* If we are running full screen, there is no need to do any of this,
   * and the background buffer can be freed if it was previously in use.
   */

  if (scaledviewwidth == SCREENWIDTH)
    {
      if (background_buffer != NULL)
        {
          z_free(background_buffer);
          background_buffer = NULL;
        }

      return;
    }

  /* Allocate the background buffer if necessary */

  if (background_buffer == NULL)
    {
      background_buffer = z_malloc(
              SCREENWIDTH * (SCREENHEIGHT - SBARHEIGHT) *
              sizeof(*background_buffer), PU_STATIC, NULL);
    }

  if (gamemode == commercial)
    {
      name = name2;
    }
  else
    {
      name = name1;
    }

  src = w_cache_lump_name(name, PU_CACHE);
  dest = background_buffer;

  for (y = 0; y < SCREENHEIGHT - SBARHEIGHT; y++)
    {
      for (x = 0; x < SCREENWIDTH / 64; x++)
        {
          memcpy(dest, src + ((y & 63) << 6), 64);
          dest += 64;
        }

      if (SCREENWIDTH & 63)
        {
          memcpy(dest, src + ((y & 63) << 6), SCREENWIDTH & 63);
          dest += (SCREENWIDTH & 63);
        }
    }

  /* Draw screen and bezel; this is done to a separate screen buffer. */

  v_use_buffer(background_buffer);

  patch = w_cache_lump_name(("brdr_t"), PU_CACHE);

  for (x = 0; x < scaledviewwidth; x += 8)
    {
      v_draw_patch(viewwindowx + x, viewwindowy - 8, patch);
    }

  patch = w_cache_lump_name(("brdr_b"), PU_CACHE);

  for (x = 0; x < scaledviewwidth; x += 8)
    {
      v_draw_patch(viewwindowx + x, viewwindowy + viewheight, patch);
    }

  patch = w_cache_lump_name(("brdr_l"), PU_CACHE);

  for (y = 0; y < viewheight; y += 8)
    {
      v_draw_patch(viewwindowx - 8, viewwindowy + y, patch);
    }

  patch = w_cache_lump_name(("brdr_r"), PU_CACHE);

  for (y = 0; y < viewheight; y += 8)
    {
      v_draw_patch(viewwindowx + scaledviewwidth, viewwindowy + y, patch);
    }

  /* Draw beveled edge. */

  v_draw_patch(viewwindowx - 8, viewwindowy - 8,
               w_cache_lump_name(("brdr_tl"), PU_CACHE));

  v_draw_patch(viewwindowx + scaledviewwidth, viewwindowy - 8,
               w_cache_lump_name(("brdr_tr"), PU_CACHE));

  v_draw_patch(viewwindowx - 8, viewwindowy + viewheight,
               w_cache_lump_name(("brdr_bl"), PU_CACHE));

  v_draw_patch(viewwindowx + scaledviewwidth, viewwindowy + viewheight,
               w_cache_lump_name(("brdr_br"), PU_CACHE));

  v_restore_buffer();
}

/* Copy a screen buffer. */

void r_video_erase(unsigned ofs, int count)
{
  /* LFB copy.
   * This might not be a good idea if memcpy is not optional, e.g. byte by
   * byte on a 32bit CPU, as GNU GCC/Linux libc did at one point.
   */

  if (background_buffer != NULL)
    {
      memcpy(i_video_buffer + ofs, background_buffer + ofs,
             count * sizeof(*i_video_buffer));
    }
}

/* r_draw_view_border
 * Draws the border around the view for different size windows?
 */

void r_draw_view_border(void)
{
  int top;
  int side;
  int ofs;
  int i;

  if (scaledviewwidth == SCREENWIDTH)
    {
      return;
    }

  top = ((SCREENHEIGHT - SBARHEIGHT) - viewheight) / 2;
  side = (SCREENWIDTH - scaledviewwidth) / 2;

  /* copy top and one line of left side */

  r_video_erase(0, top * SCREENWIDTH + side);

  /* copy one line of right side and bottom */

  ofs = (viewheight + top) * SCREENWIDTH - side;
  r_video_erase(ofs, top * SCREENWIDTH + side);

  /* copy sides using wraparound */

  ofs = top * SCREENWIDTH + SCREENWIDTH - side;
  side <<= 1;

  for (i = 1; i < viewheight; i++)
    {
      r_video_erase(ofs, side);
      ofs += SCREENWIDTH;
    }

  /* ? */

  v_mark_rect(0, 0, SCREENWIDTH, SCREENHEIGHT - SBARHEIGHT);
}
