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
 *   - an exit (i_exit(): quitting, errors) that gives all that back;
 *   - (CONFIG_GAMES_NXDOOM_GAME) with no -iwad, an IWAD from $HOME, /home
 *     or /games/doom, its configuration and saves in .doom/ beside it; or,
 *     with none there, a screen that says where to put one.
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
#include <fcntl.h>
#include <unistd.h>

#if CONFIG_GAMES_NXDOOM_HEAPSIZE > 0
#  include <nuttx/mm/mm.h>
#endif

#ifdef CONFIG_GAMES_NXDOOM_GAME
#  include <dirent.h>
#  include <strings.h>
#  include <fpc/game.h>
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

#ifdef CONFIG_GAMES_NXDOOM_GAME
static const char * const g_iwads[] =
{
  "doom.wad", "doom2.wad", "plutonia.wad", "tnt.wad", "doom1.wad",
  "freedoom1.wad", "freedoom2.wad", "freedm.wad"
};

static char g_wad[160];                 /* The IWAD found */
static char g_config[160];              /* ... its .doom/ */
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

#ifdef CONFIG_GAMES_NXDOOM_GAME
  if (code != 0 && g_exiterror[0] != '\0')
    {
      fprintf(stderr, "nxdoom: %s\n", g_exiterror);
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

#ifdef CONFIG_GAMES_NXDOOM_GAME
/****************************************************************************
 * Name: find_wad
 *
 * Description:
 *   An IWAD in dir (the first of g_iwads there, whatever the case) into
 *   g_wad, its folder's .doom/ into g_config.
 *
 ****************************************************************************/

static bool find_wad_in(const char *dir)
{
  char found[sizeof(g_iwads) / sizeof(g_iwads[0])][16];
  struct dirent *de;
  bool any = false;
  size_t i;
  DIR *d;

  d = opendir(dir);
  if (d == NULL)
    {
      return false;
    }

  memset(found, 0, sizeof(found));
  while ((de = readdir(d)) != NULL)
    {
      for (i = 0; i < sizeof(g_iwads) / sizeof(g_iwads[0]); i++)
        {
          if (strcasecmp(de->d_name, g_iwads[i]) == 0)
            {
              strlcpy(found[i], de->d_name, sizeof(found[i]));
              any = true;
            }
        }
    }

  closedir(d);
  for (i = 0; any && i < sizeof(g_iwads) / sizeof(g_iwads[0]); i++)
    {
      if (found[i][0] != '\0')
        {
          snprintf(g_wad, sizeof(g_wad), "%s/%s", dir, found[i]);
          snprintf(g_config, sizeof(g_config), "%s/.doom/", dir);
          return true;
        }
    }

  return false;
}

static bool find_wad(void)
{
  const char *home = getenv("HOME");

  return (home != NULL && home[0] != '\0' && find_wad_in(home)) ||
         find_wad_in("/home") || find_wad_in("/games/doom");
}

/****************************************************************************
 * Name: no_wad
 *
 * Description:
 *   No IWAD anywhere: a screen that says where to put one, until Escape
 *   (or the window is closed).
 *
 ****************************************************************************/

static void no_wad(void)
{
  if (game_open(320, 200, GAME_WINDOW) < 0)
    {
      fprintf(stderr, "nxdoom: no IWAD: put one (doom1.wad, "
              "freedoom1.wad...) in /games/doom\n");
      return;
    }

  game_text_color(GAME_RGB(255, 200, 64));
  game_text_center(9, "DOOM");
  game_text_color(GAME_RGB(255, 255, 255));
  game_text_center(12, "The game needs its data: an IWAD");
  game_text_center(13, "(doom1.wad, freedoom1.wad...)");
  game_text_center(15, "Put one in /games/doom");
  game_text_center(19, "Escape: back");
  while (game_frame())
    {
      game_clear(GAME_RGB(60, 8, 4));
      game_text_draw();
    }

  game_close();
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

#ifdef CONFIG_GAMES_NXDOOM_GAME
  /* With no -iwad: one found, and its .doom/ (the arguments: -iwad FILE
   * -configdir DIR, then the rest)
   */

  {
    static FAR char *args[32];
    bool given = false;
    int k;

    for (k = 1; k < argc; k++)
      {
        given |= strcmp(argv[k], "-iwad") == 0;
      }

    if (!given)
      {
        if (!find_wad())
          {
            no_wad();
            g_running = false;
            return EXIT_FAILURE;
          }

        args[0] = argv[0];
        args[1] = "-iwad";
        args[2] = g_wad;
        args[3] = "-configdir";
        args[4] = g_config;
        for (k = 1; k < argc && k + 4 < 31; k++)
          {
            args[k + 4] = argv[k];
          }

        argc = k + 4;
        args[argc] = NULL;
        argv = args;
      }
  }
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

  /* More arguments from the file "args" in -configdir, if there is one
   * (the desktop starts the game with its own: this adds "-timedemo
   * demo1", "-mb 6" and the like): read whole, split at white space
   */

  for (int i = 1; i + 1 < argc; i++)
    {
      if (strcmp(argv[i], "-configdir") == 0)
        {
          char path[160];
          char text[256];
          FAR char **more;
          int fd;
          int n = 0;

          snprintf(path, sizeof(path), "%s%sargs", argv[i + 1],
                   argv[i + 1][strlen(argv[i + 1]) - 1] == '/' ? "" : "/");
          fd = open(path, O_RDONLY);
          if (fd >= 0)
            {
              n = read(fd, text, sizeof(text) - 1);
              close(fd);
            }

          text[n > 0 ? n : 0] = '\0';
          for (char *w = strtok(text, " \t\r\n"); w != NULL;
               w = strtok(NULL, " \t\r\n"))
            {
              /* (nxdoom_realloc: myargv is the game heap's, and this file
               * has the C library's realloc, i_heap.h's macros undone)
               */

#if CONFIG_GAMES_NXDOOM_HEAPSIZE > 0
              more = nxdoom_realloc(myargv, (myargc + 1) * sizeof(char *));
#else
              more = realloc(myargv, (myargc + 1) * sizeof(char *));
#endif
              if (more == NULL)
                {
                  break;
                }

              myargv = more;
              myargv[myargc++] = m_string_duplicate(w);
            }

          break;
        }
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
