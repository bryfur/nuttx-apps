/****************************************************************************
 * apps/games/quake/fpc_quake.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Included first in every file of the game (the Makefile's GAMEFLAGS):
 * what it allocates comes from its own heap (fpc_quake.c), which goes back
 * whole when the game ends; a flat build frees nothing of a task's own.
 ****************************************************************************/

#ifndef __APPS_GAMES_QUAKE_FPC_QUAKE_H
#define __APPS_GAMES_QUAKE_FPC_QUAKE_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <fpc_place.h>

void *fpcq_malloc(size_t size);
void *fpcq_calloc(size_t n, size_t size);
void *fpcq_realloc(void *ptr, size_t size);
void fpcq_free(void *ptr);

/* Zeroed memory for one of the renderer's hot arrays: from the SRAM while
 * there is room (CONFIG_GAMES_QUAKE_SRAM), else from the game's heap; it
 * all goes back when the game ends
 */

void *fpcq_fast_alloc(size_t size, const char *what);

/* The memory the game asks for at start (quakegeneric.c): the hunk's
 * size and where the game's data is
 */

size_t fpcq_hunk_size(void);
void *fpcq_hunk(void);                  /* (Allocated before the game starts) */
const char *fpcq_basedir(void);

/* What DOS Quake's minimum memory (MINIMUM_MEMORY) had in it for the
 * video buffers at 320 x 200 (vid_vga.c: the depth buffer and the surface
 * cache from the hunk), which come from the game's heap here: the hunk
 * may be that much smaller (-res makes it so to make room for its own)
 */

#define FPCQ_HUNK_VIDEO  (320 * 200 * 2 + 600 * 1024)

/* The software renderer's frame (vid_null.c): 320 x 200, or -res W's
 * W x 3W/4; and the surface cache the heap was sized for (0: the game's
 * own reckoning, D_SurfaceCacheForRes)
 */

void fpcq_vid_size(int *w, int *h);
float fpcq_vid_aspect(void);
size_t fpcq_surfcache_size(void);

/* The game ends: the display, the keyboard and the memory given back */

void fpcq_exit(int code) __attribute__((noreturn));
void fpcq_dq_shutdown(void);              /* fpc_drawq.c */
void fpcq_palette_parts(const unsigned char *base, int r, int g, int b,
                        int alpha);               /* view.c: the base palette and the tint */

/* A function the renderer spends its time in: run from the SRAM rather
 * than from the PSRAM through the XIP cache the game's data thrashes (the
 * module loader puts it in the SRAM while the game runs, if there is room:
 * fpc_place.h).  The name is the port's: quakegeneric's files use it too.
 */

#define FPCQ_FAST FPC_SRAM_CODE("quake")

/* Where a frame's time goes (us, summed over a timedemo; fpc_drawq.c prints
 * it at the end): the game's core's parts, the world's spans by the core
 * that drew them, the surfaces' columns of blocks by the core that built
 * them; -xipcount: the XIP cache's misses a frame too
 */

enum
{
  FPCQ_T_VIEW,                            /* R_RenderView, all of it */
  FPCQ_T_WORLD,                           /* R_RenderWorld: the BSP, the edges */
  FPCQ_T_BENT,                            /* R_DrawBEntitiesOnList */
  FPCQ_T_SCAN,                            /* R_ScanEdges, D_DrawSurfaces in it */
  FPCQ_T_DSURF,                           /* D_DrawSurfaces */
  FPCQ_T_ENT,                             /* R_DrawEntitiesOnList, the view model */
  FPCQ_T_PART,                            /* R_DrawParticles */
  FPCQ_T_SEND,                            /* QG_DrawFrame */
  FPCQ_T_FRAME,                           /* A frame, all of it */
  FPCQ_T_SPANS0,                          /* The world's spans on the game's core */
  FPCQ_T_SPANS1,                          /* ... on the drawer's */
  FPCQ_T_FULL,                            /* The game waiting for room in the drawer's queue */
  FPCQ_T_COLS0,                           /* Surface columns: the game's */
  FPCQ_T_COLS1,                           /* ... the drawer's */
  FPCQ_T_N
};

extern unsigned int fpcq_tsum[FPCQ_T_N];
extern unsigned int fpcq_xmisses;         /* -xipcount: XIP cache misses (both cores') */
extern unsigned int fpcq_xipcount;

static inline unsigned int fpcq_us(void)
{
#ifdef __ARM_ARCH
  return *(volatile unsigned int *)0x400b0028;   /* RP2350 TIMER0 TIMERAWL */
#else
  return 0;
#endif
}

#define FPCQ_TIMED(i, stmt) \
  do { unsigned int t0_ = fpcq_us(); stmt; fpcq_tsum[i] += fpcq_us() - t0_; } \
  while (0)

void fpcq_times_report(unsigned int frames);

/* (Not in fpc_quake.c itself, which allocates the heap) */

#ifndef FPCQ_PLATFORM
#  define malloc(s)     fpcq_malloc(s)
#  define calloc(n, s)  fpcq_calloc(n, s)
#  define realloc(p, s) fpcq_realloc(p, s)
#  define free(p)       fpcq_free(p)
#endif

#endif /* __APPS_GAMES_QUAKE_FPC_QUAKE_H */
