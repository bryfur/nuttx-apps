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
size_t fpcq_surfcache_size(void);

/* The game ends: the display, the keyboard and the memory given back */

void fpcq_exit(int code) __attribute__((noreturn));
void fpcq_dq_shutdown(void);              /* fpc_drawq.c */
void fpcq_palette_parts(const unsigned char *base, int r, int g, int b,
                        int alpha);               /* view.c: the base palette and the tint */

/* A function the renderer spends its time in: run from the SRAM rather
 * than from the flash through the XIP cache the game's data in the PSRAM
 * thrashes (.time_critical, copied to the SRAM at boot)
 */

#define FPCQ_FAST __attribute__((section(".time_critical.quake")))

/* (Not in fpc_quake.c itself, which allocates the heap) */

#ifndef FPCQ_PLATFORM
#  define malloc(s)     fpcq_malloc(s)
#  define calloc(n, s)  fpcq_calloc(n, s)
#  define realloc(p, s) fpcq_realloc(p, s)
#  define free(p)       fpcq_free(p)
#endif

#endif /* __APPS_GAMES_QUAKE_FPC_QUAKE_H */
