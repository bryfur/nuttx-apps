/****************************************************************************
 * apps/games/NXDoom/src/i_main.c
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
 *  Main program, simply calls d_doom_main high level loop.
 *
 *  A builtin program runs again and again in the same memory, so this
 *  file (outside the game's state block, CONFIG_GAMES_NXDOOM_RESTART) also
 *  looks after what a process would get from the system:
 *
 *   - its variables as they were built, at every start (the game's data
 *     and bss, one block each: nxdoom.ld);
 *   - a heap of its own (CONFIG_GAMES_NXDOOM_HEAPSIZE), given back whole
 *     at the end;
 *   - an exit (i_exit(): quitting, errors) that gives all that back, and
 *     tells the desktop how the game ended (CONFIG_GAMES_NXDOOM_DESKTOP).
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "config.h"

#include <assert.h>
#include <malloc.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_GAMES_NXDOOM_HEAPSIZE > 0
#  include <nuttx/mm/mm.h>
#endif

#ifdef CONFIG_GAMES_NXDOOM_DESKTOP
#  include <graphics/deskgame.h>
#endif

#include "doomtype.h"
#include "i_system.h"
#include "i_video.h"
#include "m_argv.h"
#include "m_misc.h"
#include "z_zone.h"

/* This file is the heap: the real allocator below */

#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* With less free than CONFIG_GAMES_NXDOOM_HEAPSIZE, the game takes all but
 * HEAP_RESERVE of it (for the rest of the system), if that is HEAP_MIN
 */

#define HEAP_RESERVE   (1024 * 1024)
#define HEAP_MIN       (3 * 1024 * 1024)

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: d_doom_main()
 *
 * Description:
 *  Not a globally visible function, just included for source reference,
 *  calls all startup code, parses command line options.
 *
 ****************************************************************************/

void d_doom_main(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Kept from one run to the next (not in the game's state block) */

static volatile bool g_running;         /* One game at a time */

#ifdef CONFIG_GAMES_NXDOOM_RESTART
extern char __nxdoom_data_start[];
extern char __nxdoom_data_end[];
extern char __nxdoom_bss_start[];
extern char __nxdoom_bss_end[];

static FAR void *g_data0;               /* The data before the first run */
#endif

#if CONFIG_GAMES_NXDOOM_HEAPSIZE > 0
static FAR struct mm_heap_s *g_heap;
static FAR void *g_heapmem;
static size_t g_heapsize;
static unsigned long g_purged;          /* Cache let go of for room */
#endif

static char g_exiterror[160];

#ifdef CONFIG_GAMES_NXDOOM_DESKTOP
static FAR struct deskgame_s *g_deskgame;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_GAMES_NXDOOM_RESTART
/****************************************************************************
 * Name: state_restore
 *
 * Description:
 *   The game's variables as they were built: the data saved before the
 *   first run put back, the bss cleared.
 *
 ****************************************************************************/

static int state_restore(void)
{
  size_t len = __nxdoom_data_end - __nxdoom_data_start;

  if (g_data0 == NULL)
    {
      g_data0 = malloc(len);
      if (g_data0 == NULL)
        {
          return -1;
        }

      memcpy(g_data0, __nxdoom_data_start, len);
    }
  else
    {
      memcpy(__nxdoom_data_start, g_data0, len);
    }

  memset(__nxdoom_bss_start, 0, __nxdoom_bss_end - __nxdoom_bss_start);
  return 0;
}
#endif

#if CONFIG_GAMES_NXDOOM_HEAPSIZE > 0
/****************************************************************************
 * Name: heap_create
 *
 * Description:
 *   The game's heap: CONFIG_GAMES_NXDOOM_HEAPSIZE KB, or "-mb N" MB.
 *
 ****************************************************************************/

static int heap_create(int argc, FAR char *argv[])
{
  size_t size = (size_t)CONFIG_GAMES_NXDOOM_HEAPSIZE * 1024;
  int i;

  for (i = 1; i + 1 < argc; i++)
    {
      if (strcmp(argv[i], "-mb") == 0 && atoi(argv[i + 1]) > 0)
        {
          size = (size_t)atoi(argv[i + 1]) * 1024 * 1024;
        }
    }

  /* A heap left by a game that was killed */

  if (g_heap != NULL)
    {
      mm_uninitialize(g_heap);
      free(g_heapmem);
      g_heap = NULL;
    }

  g_heapmem = malloc(size);
  if (g_heapmem == NULL)
    {
      /* Less, if there is enough: all but HEAP_RESERVE of what is free */

      struct mallinfo info = mallinfo();
      size_t avail = info.mxordblk > HEAP_RESERVE ?
                     info.mxordblk - HEAP_RESERVE : 0;

      if (avail >= HEAP_MIN)
        {
          size      = avail;
          g_heapmem = malloc(size);
        }
    }

  if (g_heapmem == NULL)
    {
      snprintf(g_exiterror, sizeof(g_exiterror),
               "Not enough memory: the game needs %zu KB, and at least "
               "%u", size / 1024, HEAP_MIN / 1024);
      return -1;
    }

  g_heapsize = size;
  g_purged   = 0;
  g_heap     = mm_initialize("doom", g_heapmem, size);
  if (g_heap == NULL)
    {
      free(g_heapmem);
      g_heapmem = NULL;
      strlcpy(g_exiterror, "No heap for the game", sizeof(g_exiterror));
      return -1;
    }

  return 0;
}

static void heap_destroy(void)
{
  if (g_heap != NULL)
    {
      struct mm_heap_s *heap = g_heap;
      struct mallinfo info = mm_mallinfo(heap);

      printf("nxdoom: heap of %zu KB: %d KB used at most, %lu KB of cache "
             "let go of for room\n", g_heapsize / 1024, info.usmblks / 1024,
             g_purged / 1024);
      g_heap = NULL;
      mm_uninitialize(heap);
      free(g_heapmem);
      g_heapmem = NULL;
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#if CONFIG_GAMES_NXDOOM_HEAPSIZE > 0
/****************************************************************************
 * Name: nxdoom_malloc and friends
 *
 * Description:
 *   The game's allocations (i_heap.h), in its heap once there is one.
 *   Whatever was allocated elsewhere goes back there.
 *
 ****************************************************************************/

void *nxdoom_malloc(size_t size)
{
  void *p;

  if (g_heap == NULL)
    {
      return malloc(size);
    }

  /* Full: the zone's cache makes room (the heap fills up with it) */

  while ((p = mm_malloc(g_heap, size)) == NULL &&
         z_purge_cache(size + 64, &g_purged))
    {
    }

  return p;
}

void *nxdoom_calloc(size_t n, size_t size)
{
  void *p = nxdoom_malloc(n * size);

  if (p != NULL)
    {
      memset(p, 0, n * size);
    }

  return p;
}

void nxdoom_free(void *ptr)
{
  if (ptr == NULL)
    {
      return;
    }

  if (g_heap != NULL && mm_heapmember(g_heap, ptr))
    {
      mm_free(g_heap, ptr);
    }
  else
    {
      free(ptr);
    }
}

void *nxdoom_realloc(void *ptr, size_t size)
{
  if (ptr == NULL)
    {
      return nxdoom_malloc(size);
    }

  if (g_heap != NULL && mm_heapmember(g_heap, ptr))
    {
      void *p;

      while ((p = mm_realloc(g_heap, ptr, size)) == NULL && size > 0 &&
             z_purge_cache(size + 64, &g_purged))
        {
        }

      return p;
    }

  return realloc(ptr, size);
}

char *nxdoom_strdup(const char *s)
{
  size_t len = strlen(s) + 1;
  char *p = nxdoom_malloc(len);

  if (p != NULL)
    {
      memcpy(p, s, len);
    }

  return p;
}
#endif

/****************************************************************************
 * Name: nxdoom_end
 *
 * Description:
 *   The game has ended: give back its heap, tell the desktop.
 *
 ****************************************************************************/

static void nxdoom_end(int code)
{
#if CONFIG_GAMES_NXDOOM_HEAPSIZE > 0
  heap_destroy();
#endif

#ifdef CONFIG_GAMES_NXDOOM_DESKTOP
  if (g_deskgame != NULL)
    {
      deskgame_detach(g_deskgame, code == 0 ? NULL :
                      g_exiterror[0] != '\0' ? g_exiterror :
                      "The game ended with an error");
      g_deskgame = NULL;
    }
#endif

  g_running = false;
}

/****************************************************************************
 * Name: i_exit
 *
 * Description:
 *   The game ends (the task: the game runs on its own).
 *
 ****************************************************************************/

void i_exit(int code)
{
  nxdoom_end(code);
  exit(code);
}

/****************************************************************************
 * Name: i_exit_error
 *
 * Description:
 *   Why the game is ending (i_error()), for whoever started it.
 *
 ****************************************************************************/

void i_exit_error(const char *msg)
{
  size_t n;
  char *p;

  strlcpy(g_exiterror, msg, sizeof(g_exiterror));

  /* One line, for a window */

  for (p = g_exiterror; *p != '\0'; p++)
    {
      if ((unsigned char)*p < ' ')
        {
          *p = ' ';
        }
    }

  for (n = strlen(g_exiterror); n > 0 && g_exiterror[n - 1] == ' '; n--)
    {
      g_exiterror[n - 1] = '\0';
    }
}

#ifdef CONFIG_GAMES_NXDOOM_DESKTOP
/****************************************************************************
 * Name: i_deskgame
 *
 * Description:
 *   The desktop's session, if the desktop started the game.
 *
 ****************************************************************************/

FAR struct deskgame_s *i_deskgame(void)
{
  return g_deskgame;
}
#endif

int main(int argc, char **argv)
{
  bool busy;

  sched_lock();
  busy      = g_running;
  g_running = true;
  sched_unlock();

  if (busy)
    {
      fprintf(stderr, "nxdoom: already running\n");
      return EXIT_FAILURE;
    }

  g_exiterror[0] = '\0';

#ifdef CONFIG_GAMES_NXDOOM_DESKTOP
  g_deskgame = deskgame_attach(argc, argv, SCREENWIDTH, SCREENHEIGHT);
  if (g_deskgame == NULL)
    {
      fprintf(stderr, "nxdoom: it plays in a window of the desktop: "
                      "open Doom there\n");
      g_running = false;
      return EXIT_FAILURE;
    }

  deskgame_status(g_deskgame, "Loading the game's data");
#endif

#ifdef CONFIG_GAMES_NXDOOM_RESTART
  if (state_restore() < 0)
    {
      strlcpy(g_exiterror, "Not enough memory", sizeof(g_exiterror));
      nxdoom_end(EXIT_FAILURE);
      return EXIT_FAILURE;
    }
#endif

#if CONFIG_GAMES_NXDOOM_HEAPSIZE > 0
  if (heap_create(argc, argv) < 0)
    {
      nxdoom_end(EXIT_FAILURE);
      return EXIT_FAILURE;
    }
#endif

  i_install_quit_signal();

  /* save arguments */

  myargc = argc;
#if CONFIG_GAMES_NXDOOM_HEAPSIZE > 0
  myargv = nxdoom_malloc(argc * sizeof(char *));
#else
  myargv = malloc(argc * sizeof(char *));
#endif
  assert(myargv != NULL);

  for (int i = 0; i < argc; i++)
    {
      myargv[i] = m_string_duplicate(argv[i]);
    }

  /* Print the program version and exit. */

  if (m_parm_exists("-version") || m_parm_exists("--version"))
    {
      puts(PACKAGE_STRING);
      i_exit(0);
    }

  m_find_response_file();
  m_set_exe_dir();

  /* start doom (it ends with i_exit()) */

  d_doom_main();

  i_exit(0);
  return 0;
}
