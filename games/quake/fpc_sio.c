/****************************************************************************
 * apps/games/quake/fpc_sio.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The renderer's inner loops on the RP2350's SIO interpolators
 * (arch/chip/interp.h): each core has two, which turn "the texel's address,
 * then step u and v" into one single-cycle load.  Pixel for pixel what the
 * C loops draw (d_scan.c, r_surf.c), which stay for everything else: the
 * simulator, -nosio, a thread that holds no interpolators.
 *
 * The interpolators are a core's, not a thread's: the game (pinned to the
 * first core) and the drawer (the second; fpc_drawq.c) each claim their
 * core's pair once pinned, and the loops here run only on a core whose
 * pair its thread holds (fpcq_sio_here()).  Each function sets up every
 * register it relies on when it starts, so that a span batch and a surface
 * build can follow each other on one core.
 *
 *   D_DrawSpans8Sio       the world's textured spans: s on one lane, t
 *                         times the cache block's width on the other three
 *                         (each a power of two of it: widths of up to
 *                         three bits set; the others stay C)
 *   D_DrawZSpansSio       the world's depths: two a word, both halves
 *                         from one POP
 *   fpcq_sio_blocks[mip]  the surface cache's blocks: the texel through
 *                         the light's row of the colour map, the light
 *                         stepped along the row
 *
 * -nosio: none of it; -siomask N: some (FPCQ_SIO_*); -siocheck: each span,
 * depth span and block drawn again by the C and compared, the counts
 * printed as the game ends (both cores' differ 0 times over demo1's
 * timedemo, at 320 x 200 and at -res 640).
 ****************************************************************************/

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

#include <errno.h>
#include <syslog.h>
#include <unistd.h>

#include "fpc_sio.h"

#ifdef FPCQ_SIO

#include <arch/chip/interp.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define I0 RP23XX_INTERP0
#define I1 RP23XX_INTERP1

/****************************************************************************
 * External Data (r_surf.c's: what R_DrawSurface sets up for a block)
 ****************************************************************************/

extern int				lightleft, sourcetstep, lightright;
extern int				lightleftstep, lightrightstep;
extern void				*prowdestbase;
extern unsigned char	*pbasesource;
extern int				surfrowbytes;
extern unsigned			*r_lightptr;
extern int				r_stepback;
extern int				r_lightwidth;
extern int				r_numvblocks;
extern unsigned char	*r_sourcemax;

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Per core: what its interpolators draw, 0 unless its thread holds them
 * (read on every span batch and surface: in the SRAM).  Four bytes, not
 * two: .sram.quake must stay a whole number of words, or the module's
 * .init_array after it starts unaligned and its end, aligned, is two
 * bytes on (gnu-elf.ld): _start calls a constructor at address 0.
 */

volatile uint8_t fpcq_sio_on[4] __attribute__((section(".sram.quake")));

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_checking;                   /* -siocheck */
static void check_alloc(void);
static void check_report(int cpu);

static int g_sio_mask = -1;               /* -siomask, -nosio (-1: not
                                           * looked at yet) */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* The calling thread, pinned to its core, takes the core's interpolators
 * (the game's thread and the drawer's, fpc_drawq.c)
 */

void fpcq_sio_claim(const char *who)
{
  int tries;
  int cpu;

  if (g_sio_mask < 0)
    {
      int i = COM_CheckParm("-siomask");

      g_checking = COM_CheckParm("-siocheck") != 0;
      if (g_checking)
        {
          check_alloc();                  /* (both cores': the game's
                                           * claim is the first) */
        }

      g_sio_mask = COM_CheckParm("-nosio") ? 0 :
                   i && i + 1 < com_argc ? atoi(com_argv[i + 1]) &
                                           FPCQ_SIO_ALL :
                   FPCQ_SIO_ALL;
    }

  if (g_sio_mask == 0)
    {
      return;
    }

  /* (A thread just pinned elsewhere moves there at its next switch) */

  for (tries = 0; (cpu = rp23xx_interp_claim()) == -EPERM && tries < 20;
       tries++)
    {
      usleep(1000);
    }

  if (cpu < 0)
    {
      syslog(LOG_INFO, "quake: %s: no interpolators (%d), plain C\n",
             who, cpu);
      return;
    }

  fpcq_sio_on[cpu] = g_sio_mask;
  syslog(LOG_INFO, "quake: %s: the interpolators of core %d (%d)\n", who,
         cpu, g_sio_mask);
}

void fpcq_sio_release(void)
{
  int cpu = fpcq_sio_cpu();

  if (fpcq_sio_on[cpu])
    {
      fpcq_sio_on[cpu] = 0;
      rp23xx_interp_release();
      if (g_checking)
        {
          check_report(cpu);
        }
    }
}

/****************************************************************************
 * D_DrawSpans8Sio
 *
 * D_DrawSpans8Ctx (d_scan.c) with its pixel loop
 *
 *   *pdest++ = *(pbase + (s >> 16) + (t >> 16) * cachewidth);
 *   s += sstep; t += tstep;
 *
 * as POPs: INTERP0 lane 0 is s (s >> 16), the other three lanes each a copy
 * of t giving (t >> 16) << b for a bit b of cachewidth, all four stepped by
 * their POPs (ADD_RAW, BASE the step); INTERP0's BASE2 is pbase, so the
 * texel is at POP2 of INTERP0 plus POP2 of INTERP1.  Exact: s and t are
 * never negative nor 2^31 or more within a span (clamped to 0 .. bbextent,
 * the far ends to 8 .. bbextent, the steps between them), so each lane's
 * rotate and mask is the C's shift, and the sum is the C's address.
 ****************************************************************************/

#define SPAN_SETUP(sbits)                                     \
  do                                                          \
    {                                                         \
      I0->accum[0] = s; I0->base[0] = sstep;                  \
      I0->accum[1] = t; I0->base[1] = tstep;                  \
      if ((sbits) >= 2)                                       \
        {                                                     \
          I1->accum[0] = t; I1->base[0] = tstep;              \
        }                                                     \
      if ((sbits) >= 3)                                       \
        {                                                     \
          I1->accum[1] = t; I1->base[1] = tstep;              \
        }                                                     \
    }                                                         \
  while (0)

static inline __attribute__((always_inline))
void span_body(const dspanctx_t *c, espan_t *pspan, const int nbits)
{
	int				count, spancount;
	unsigned char	*pdest;
	fixed16_t		s, t, snext, tnext, sstep, tstep;
	float			sdivz, tdivz, zi, z, du, dv, spancountminus1;
	float			sdivz8stepu, tdivz8stepu, zi8stepu;
	pixel_t * const	d_viewbuffer = c->viewbuffer;
	const int		screenwidth = c->screenwidth;
	const float		d_sdivzstepu = c->sdivzstepu, d_tdivzstepu = c->tdivzstepu;
	const float		d_zistepu = c->zistepu;
	const float		d_sdivzstepv = c->sdivzstepv, d_tdivzstepv = c->tdivzstepv;
	const float		d_zistepv = c->zistepv;
	const float		d_sdivzorigin = c->sdivzorigin, d_tdivzorigin = c->tdivzorigin;
	const float		d_ziorigin = c->ziorigin;
	const fixed16_t	sadjust = c->sadjust, tadjust = c->tadjust;
	const fixed16_t	bbextents = c->bbextents, bbextentt = c->bbextentt;

	sstep = 0;	// keep compiler happy
	tstep = 0;	// ditto

	sdivz8stepu = d_sdivzstepu * 8;
	tdivz8stepu = d_tdivzstepu * 8;
	zi8stepu = d_zistepu * 8;

	do
	{
		pdest = (unsigned char *)((byte *)d_viewbuffer +
				(screenwidth * pspan->v) + pspan->u);

		count = pspan->count;

	// calculate the initial s/z, t/z, 1/z, s, and t and clamp
		du = (float)pspan->u;
		dv = (float)pspan->v;

		sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
		tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

		s = (int)(sdivz * z) + sadjust;
		if (s > bbextents)
			s = bbextents;
		else if (s < 0)
			s = 0;

		t = (int)(tdivz * z) + tadjust;
		if (t > bbextentt)
			t = bbextentt;
		else if (t < 0)
			t = 0;

		do
		{
		// calculate s and t at the far end of the span
			if (count >= 8)
				spancount = 8;
			else
				spancount = count;

			count -= spancount;

			if (count)
			{
				sdivz += sdivz8stepu;
				tdivz += tdivz8stepu;
				zi += zi8stepu;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;

				sstep = (snext - s) >> 3;
				tstep = (tnext - t) >> 3;
			}
			else
			{
				spancountminus1 = (float)(spancount - 1);
				sdivz += d_sdivzstepu * spancountminus1;
				tdivz += d_tdivzstepu * spancountminus1;
				zi += d_zistepu * spancountminus1;
				z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
				snext = (int)(sdivz * z) + sadjust;
				if (snext > bbextents)
					snext = bbextents;
				else if (snext < 8)
					snext = 8;

				tnext = (int)(tdivz * z) + tadjust;
				if (tnext > bbextentt)
					tnext = bbextentt;
				else if (tnext < 8)
					tnext = 8;

				if (spancount > 1)
				{
					sstep = (snext - s) / (spancount - 1);
					tstep = (tnext - t) / (spancount - 1);
				}
			}

			SPAN_SETUP(nbits);
			// (8 at most: unrolled, entered at the count's place)
			if (nbits == 1)
			{
				switch (spancount)
				{
					case 8: *pdest++ = *(const unsigned char *)I0->pop[2];
					case 7: *pdest++ = *(const unsigned char *)I0->pop[2];
					case 6: *pdest++ = *(const unsigned char *)I0->pop[2];
					case 5: *pdest++ = *(const unsigned char *)I0->pop[2];
					case 4: *pdest++ = *(const unsigned char *)I0->pop[2];
					case 3: *pdest++ = *(const unsigned char *)I0->pop[2];
					case 2: *pdest++ = *(const unsigned char *)I0->pop[2];
					default: *pdest++ = *(const unsigned char *)I0->pop[2];
				}
			}
			else
			{
#define PIX2 do { uint32_t a = I0->pop[2]; \
                  *pdest++ = *(const unsigned char *)(a + I1->pop[2]); } while (0)
				switch (spancount)
				{
					case 8: PIX2;
					case 7: PIX2;
					case 6: PIX2;
					case 5: PIX2;
					case 4: PIX2;
					case 3: PIX2;
					case 2: PIX2;
					default: PIX2;
				}
#undef PIX2
			}

			s = snext;
			t = tnext;

		} while (count > 0);

	} while ((pspan = pspan->pnext) != NULL);
}

/* The world's spans of a surface (D_DrawSpans8Ctx's work) on this core's
 * interpolators: false if its width has more than three bits set (the C
 * draws it)
 */

FPCQ_FAST static qboolean spans_sio(const dspanctx_t *c, espan_t *pspan)
{
  uint32_t w = (uint32_t)c->cachewidth;
  uint32_t ctrl = INTERP_FIELD(16, 0, 15) | INTERP_ADD_RAW;
  int b[3];
  int n = 0;

  while (w != 0 && n < 3)
    {
      b[n++] = __builtin_ctz(w);
      w &= w - 1;
    }

  if (w != 0 || n == 0)
    {
      return false;
    }

  /* INTERP0: s and t << b[0], from the cache block; INTERP1: t << b[1]
   * and t << b[2], or nothing (a lane of ctrl 0 on accumulator and base 0
   * stays 0)
   */

  I0->ctrl[0] = ctrl;
  I0->ctrl[1] = INTERP_FIELD(16, b[0], 15) | INTERP_ADD_RAW;
  I0->base[2] = (uint32_t)c->cacheblock;
  if (n >= 2)
    {
      I1->ctrl[0] = INTERP_FIELD(16, b[1], 15) | INTERP_ADD_RAW;
      I1->ctrl[1] = n >= 3 ? INTERP_FIELD(16, b[2], 15) | INTERP_ADD_RAW
                           : 0;
      I1->accum[1] = 0;
      I1->base[1] = 0;
      I1->base[2] = 0;
    }

  switch (n)
    {
      case 1:
        span_body(c, pspan, 1);
        break;

      case 2:
        span_body(c, pspan, 2);
        break;

      default:
        span_body(c, pspan, 3);
        break;
    }

  return true;
}

/****************************************************************************
 * D_DrawZSpansSio
 *
 * D_DrawZSpansCtx (d_scan.c) with its two-pixel loop
 *
 *   ltemp = izi >> 16; izi += izistep;
 *   ltemp |= izi & 0xFFFF0000; izi += izistep;
 *   *(int *)pdest = ltemp; pdest += 2;
 *
 * as one POP2 a word: lane 0 izi (izi >> 16), lane 1 izi + izistep (its
 * top half), each stepped by 2 izistep.  Exact where izi is never negative
 * nor 2^31 or more along the span (then the C's shift leaves the top half
 * zero, and the two halves' | is their sum): checked at its two ends, the
 * C's loop where it is not.
 ****************************************************************************/

FPCQ_FAST static void zspans_sio(const dspanctx_t *c, espan_t *pspan)
{
	int				count, doublecount, izistep;
	int				izi;
	short			*pdest;
	unsigned		ltemp;
	float			zi;
	float			du, dv;
	int64_t			end;
	short * const	d_pzbuffer = c->pzbuffer;
	const int		d_zwidth = c->zwidth;
	const float		d_zistepu = c->zistepu, d_zistepv = c->zistepv;
	const float		d_ziorigin = c->ziorigin;

	izistep = (int)(d_zistepu * 0x8000 * 0x10000);

	I0->ctrl[0] = INTERP_FIELD(16, 0, 16) | INTERP_ADD_RAW;
	I0->ctrl[1] = INTERP_MASK(16, 31) | INTERP_ADD_RAW;
	I0->base[0] = 2 * (uint32_t)izistep;
	I0->base[1] = 2 * (uint32_t)izistep;
	I0->base[2] = 0;

	do
	{
		pdest = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;

		count = pspan->count;

	// calculate the initial 1/z
		du = (float)pspan->u;
		dv = (float)pspan->v;

		zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
		izi = (int)(zi * 0x8000 * 0x10000);
		end = (int64_t)izi + (int64_t)count * izistep;

		if ((intptr_t)pdest & 0x02)
		{
			*pdest++ = (short)(izi >> 16);
			izi += izistep;
			count--;
		}

		if ((doublecount = count >> 1) > 0)
		{
			if (izi >= 0 && end >= 0 && end <= INT32_MAX)
			{
				I0->accum[0] = izi;
				I0->accum[1] = izi + izistep;
				do
				{
					*(int *)pdest = I0->pop[2];
					pdest += 2;
				} while (--doublecount > 0);
				izi = I0->accum[0];
			}
			else
			{
				do
				{
					ltemp = izi >> 16;
					izi += izistep;
					ltemp |= izi & 0xFFFF0000;
					izi += izistep;
					*(int *)pdest = ltemp;
					pdest += 2;
				} while (--doublecount > 0);
			}
		}

		if (count & 1)
			*pdest = (short)(izi >> 16);

	} while ((pspan = pspan->pnext) != NULL);
}

/****************************************************************************
 * The surface cache's blocks: R_DrawSurfaceBlock8_mip0..3 (r_surf.c) with
 * their pixel loop
 *
 *   prowdest[b] = vid.colormap[(light & 0xFF00) + psource[b]];
 *   light += lightstep;
 *
 * as INTERP0's POP2 (BASE2 the colour map, lane 0 light & 0xFF00, lane 1
 * nothing) plus the texel, lane 0 stepping the light (ADD_RAW).  The light
 * wraps as the C's int does (the steps along a block's left and right
 * edges are unsigned, as in the C: they can be huge), and the mask is the
 * C's & whatever the sign: exact.  The globals the C leaves behind are
 * left the same.
 ****************************************************************************/

static inline __attribute__((always_inline)) void block_body(const int shift)
{
	const int		size = 1 << shift;
	int				v, i, b, lightstep, lighttemp;
	unsigned char	*psource, *prowdest;
	int				ll, lr, lls, lrs;
	unsigned		*lp = r_lightptr;
	const int		lw = r_lightwidth;
	const int		tstep = sourcetstep;
	const int		rowbytes = surfrowbytes;

	I0->ctrl[0] = INTERP_MASK(8, 15) | INTERP_ADD_RAW;
	I0->ctrl[1] = 0;
	I0->accum[1] = 0;
	I0->base[1] = 0;
	I0->base[2] = (uint32_t)vid.colormap;

	psource = pbasesource;
	prowdest = prowdestbase;
	ll = lightleft;
	lr = lightright;
	lls = lightleftstep;
	lrs = lightrightstep;

	for (v=0 ; v<r_numvblocks ; v++)
	{
		ll = lp[0];
		lr = lp[1];
		lp += lw;
		lls = (lp[0] - ll) >> shift;
		lrs = (lp[1] - lr) >> shift;

		for (i=0 ; i<size ; i++)
		{
			lighttemp = ll - lr;
			lightstep = lighttemp >> shift;

			I0->accum[0] = lr;
			I0->base[0] = lightstep;
#pragma GCC unroll 16
			for (b=size-1; b>=0; b--)
			{
				prowdest[b] = *((const unsigned char *)I0->pop[2] +
				                psource[b]);
			}

			psource += tstep;
			lr += lrs;
			ll += lls;
			prowdest += rowbytes;
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
	}

	r_lightptr = lp;
	lightleft = ll;
	lightright = lr;
	lightleftstep = lls;
	lightrightstep = lrs;
}

FPCQ_FAST static void block_sio0(void)
{
  block_body(4);
}

FPCQ_FAST static void block_sio1(void)
{
  block_body(3);
}

FPCQ_FAST static void block_sio2(void)
{
  block_body(2);
}

FPCQ_FAST static void block_sio3(void)
{
  block_body(1);
}

/****************************************************************************
 * -siocheck: everything drawn on the interpolators drawn again by the C,
 * and the two compared (the pixels, the depths, a block's texels and the
 * globals it leaves), counted per core and reported as the game ends.
 * The C's is what stays.  Slow; a check of the loops above over a whole
 * timedemo whatever else differs from run to run.
 ****************************************************************************/

#define CHECK_BLOCK  (16 * 1024)          /* A column of blocks at most */

struct check_s
{
  uint8_t *pix;                           /* A span's pixels */
  short *z;                               /* ... depths */
  uint8_t *blk;                           /* CHECK_BLOCK */
  uint32_t spans, zspans, blocks;         /* Compared */
  uint32_t spansbad, zspansbad, blocksbad;
  uint32_t skipped;
};

static struct check_s g_check[2];

extern void R_DrawSurfaceBlock8_mip0(void);
extern void R_DrawSurfaceBlock8_mip1(void);
extern void R_DrawSurfaceBlock8_mip2(void);
extern void R_DrawSurfaceBlock8_mip3(void);

/* (From the system's heap: the game's is all taken) */

#undef malloc
#undef free

static void check_alloc(void)
{
  int w;
  int h;
  int k;

  fpcq_vid_size(&w, &h);
  for (k = 0; k < 2; k++)
    {
      g_check[k].pix = malloc(w);
      g_check[k].z = malloc(w * sizeof(short));
      g_check[k].blk = malloc(CHECK_BLOCK);
      if (!g_check[k].pix || !g_check[k].z || !g_check[k].blk)
        {
          printf("quake: -siocheck: no memory\n");
          g_checking = false;
          return;
        }
    }
}

static void check_report(int cpu)
{
  struct check_s *k = &g_check[cpu];

  printf("quake: -siocheck core %d: %u spans (%u differ), %u depth spans "
         "(%u differ), %u blocks (%u differ), %u not compared\n", cpu,
         (unsigned)k->spans, (unsigned)k->spansbad, (unsigned)k->zspans,
         (unsigned)k->zspansbad, (unsigned)k->blocks,
         (unsigned)k->blocksbad, (unsigned)k->skipped);
  free(k->pix);                           /* (nothing frees them else) */
  free(k->z);
  free(k->blk);
  k->pix = NULL;
  k->z = NULL;
  k->blk = NULL;
}

/* A surface's spans one at a time (each is drawn on its own: nothing
 * carries from one to the next), each on the interpolators, kept, drawn
 * again by the C and compared
 */

FPCQ_FAST qboolean D_DrawSpans8Sio(const dspanctx_t *c, espan_t *pspan)
{
  struct check_s *k;
  espan_t *s;
  espan_t *next;
  int cpu;

  if (!g_checking)
    {
      return spans_sio(c, pspan);
    }

  cpu = fpcq_sio_cpu();
  k = &g_check[cpu];
  for (s = pspan; s != NULL; s = next)
    {
      size_t o = (size_t)c->screenwidth * s->v + s->u;

      next = s->pnext;
      s->pnext = NULL;
      if (!spans_sio(c, s))
        {
          s->pnext = next;
          return false;                   /* (the same for every span) */
        }

      memcpy(k->pix, c->viewbuffer + o, s->count);
      fpcq_sio_on[cpu] &= ~FPCQ_SIO_SPANS;  /* (the C's, this once) */
      D_DrawSpans8Ctx(c, s);
      fpcq_sio_on[cpu] |= FPCQ_SIO_SPANS;
      s->pnext = next;
      k->spans++;
      k->spansbad += memcmp(k->pix, c->viewbuffer + o, s->count) != 0;
    }

  return true;
}

FPCQ_FAST void D_DrawZSpansSio(const dspanctx_t *c, espan_t *pspan)
{
  struct check_s *k;
  espan_t *s;
  espan_t *next;
  int cpu;

  if (!g_checking)
    {
      zspans_sio(c, pspan);
      return;
    }

  cpu = fpcq_sio_cpu();
  k = &g_check[cpu];
  for (s = pspan; s != NULL; s = next)
    {
      size_t o = (size_t)c->zwidth * s->v + s->u;

      next = s->pnext;
      s->pnext = NULL;
      zspans_sio(c, s);
      memcpy(k->z, c->pzbuffer + o, s->count * sizeof(short));
      fpcq_sio_on[cpu] &= ~FPCQ_SIO_ZSPANS;
      D_DrawZSpansCtx(c, s);
      fpcq_sio_on[cpu] |= FPCQ_SIO_ZSPANS;
      s->pnext = next;
      k->zspans++;
      k->zspansbad += memcmp(k->z, c->pzbuffer + o,
                             s->count * sizeof(short)) != 0;
    }
}

static void block_check(int mip, void (*sio)(void), void (*cfn)(void))
{
  struct check_s *k = &g_check[fpcq_sio_cpu()];
  const int size = 16 >> mip;
  const int rows = r_numvblocks * size;
  unsigned *lp = r_lightptr;
  int ll = lightleft;
  int lr = lightright;
  int lls = lightleftstep;
  int lrs = lightrightstep;
  unsigned *lp2;
  int ll2, lr2, lls2, lrs2;
  qboolean bad = false;
  int y;

  sio();
  if (!g_checking)
    {
      return;
    }

  if (rows * size > CHECK_BLOCK)
    {
      k->skipped++;
      return;
    }

  for (y = 0; y < rows; y++)
    {
      memcpy(k->blk + y * size,
             (uint8_t *)prowdestbase + y * surfrowbytes, size);
    }

  lp2 = r_lightptr;
  ll2 = lightleft;
  lr2 = lightright;
  lls2 = lightleftstep;
  lrs2 = lightrightstep;
  r_lightptr = lp;
  lightleft = ll;
  lightright = lr;
  lightleftstep = lls;
  lightrightstep = lrs;
  cfn();
  for (y = 0; y < rows; y++)
    {
      bad |= memcmp(k->blk + y * size,
                    (uint8_t *)prowdestbase + y * surfrowbytes, size) != 0;
    }

  bad |= lp2 != r_lightptr || ll2 != lightleft || lr2 != lightright ||
         lls2 != lightleftstep || lrs2 != lightrightstep;
  k->blocks++;
  k->blocksbad += bad;
}

FPCQ_FAST static void block_mip0(void)
{
  if (g_checking)
    block_check(0, block_sio0, R_DrawSurfaceBlock8_mip0);
  else
    block_sio0();
}

FPCQ_FAST static void block_mip1(void)
{
  if (g_checking)
    block_check(1, block_sio1, R_DrawSurfaceBlock8_mip1);
  else
    block_sio1();
}

FPCQ_FAST static void block_mip2(void)
{
  if (g_checking)
    block_check(2, block_sio2, R_DrawSurfaceBlock8_mip2);
  else
    block_sio2();
}

FPCQ_FAST static void block_mip3(void)
{
  if (g_checking)
    block_check(3, block_sio3, R_DrawSurfaceBlock8_mip3);
  else
    block_sio3();
}

void (*const fpcq_sio_blocks[4])(void) =
{
  block_mip0, block_mip1, block_mip2, block_mip3
};

#endif /* FPCQ_SIO */
