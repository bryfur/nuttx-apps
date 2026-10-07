/****************************************************************************
 * apps/games/quake/fpc_sio.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The renderer's loops on the SIO interpolators (fpc_sio.c): on the
 * RP2350 only (FPCQ_SIO); elsewhere these are nothing and the C draws.
 ****************************************************************************/

#ifndef __APPS_GAMES_QUAKE_FPC_SIO_H
#define __APPS_GAMES_QUAKE_FPC_SIO_H

#include <nuttx/config.h>

#include <stdint.h>

#ifdef CONFIG_ARCH_CHIP_RP23XX
#  define FPCQ_SIO 1
#endif

/* What is drawn on them (-siomask N: a sum of these; -nosio: none) */

#define FPCQ_SIO_SPANS   1                /* D_DrawSpans8 */
#define FPCQ_SIO_BLOCKS  2                /* R_DrawSurfaceBlock8 */
#define FPCQ_SIO_ZSPANS  4                /* D_DrawZSpans */
#define FPCQ_SIO_ALL     7

#ifdef FPCQ_SIO

extern volatile uint8_t fpcq_sio_on[4];

/* The core this runs on (SIO CPUID, a single-cycle read) */

static inline int fpcq_sio_cpu(void)
{
  return *(volatile uint32_t *)0xd0000000;
}

/* What this core's interpolators draw: nothing (0) unless its thread
 * holds them (a pinned thread that claimed them: the game's or the
 * drawer's)
 */

static inline int fpcq_sio_here(void)
{
  return fpcq_sio_on[fpcq_sio_cpu()];
}

void fpcq_sio_claim(const char *who);
void fpcq_sio_release(void);

#ifdef SCANBUFFERPAD                      /* (d_local.h's dspanctx_t) */
qboolean D_DrawSpans8Sio(const dspanctx_t *c, espan_t *pspan);
void D_DrawZSpansSio(const dspanctx_t *c, espan_t *pspan);
#endif
extern void (*const fpcq_sio_blocks[4])(void);

#else

#  define fpcq_sio_here()     0
#  define fpcq_sio_claim(who) ((void)(who))
#  define fpcq_sio_release()  ((void)0)
#  define D_DrawSpans8Sio(c, pspan) ((void)(c), (void)(pspan), 0)
#  define D_DrawZSpansSio(c, pspan) ((void)(c), (void)(pspan))

#endif

#endif /* __APPS_GAMES_QUAKE_FPC_SIO_H */
