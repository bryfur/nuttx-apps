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
const char *fpcq_basedir(void);

/* The game ends: the display, the keyboard and the memory given back */

void fpcq_exit(int code) __attribute__((noreturn));

/* (Not in fpc_quake.c itself, which allocates the heap) */

#ifndef FPCQ_PLATFORM
#  define malloc(s)     fpcq_malloc(s)
#  define calloc(n, s)  fpcq_calloc(n, s)
#  define realloc(p, s) fpcq_realloc(p, s)
#  define free(p)       fpcq_free(p)
#endif

#endif /* __APPS_GAMES_QUAKE_FPC_QUAKE_H */
