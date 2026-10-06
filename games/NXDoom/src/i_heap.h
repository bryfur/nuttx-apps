/****************************************************************************
 * apps/games/NXDoom/src/i_heap.h
 *
 * SPDX-License-Identifier: GPLv2
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
 *   Included in every source file (-include): sends the game's
 *   allocations to its own heap (CONFIG_GAMES_NXDOOM_HEAPSIZE, i_main.c).
 *
 ****************************************************************************/

#ifndef __I_HEAP__
#define __I_HEAP__

#include <nuttx/config.h>

#if defined(CONFIG_GAMES_NXDOOM_HEAPSIZE) && CONFIG_GAMES_NXDOOM_HEAPSIZE > 0

/* Declare the real ones first, then redirect the calls */

#include <stdlib.h>
#include <string.h>

void *nxdoom_malloc(size_t size);
void *nxdoom_calloc(size_t n, size_t size);
void *nxdoom_realloc(void *ptr, size_t size);
void  nxdoom_free(void *ptr);
char *nxdoom_strdup(const char *s);

#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup

#define malloc(s)     nxdoom_malloc(s)
#define calloc(n, s)  nxdoom_calloc(n, s)
#define realloc(p, s) nxdoom_realloc(p, s)
#define free(p)       nxdoom_free(p)
#define strdup(s)     nxdoom_strdup(s)

#endif

#endif /* __I_HEAP__ */
