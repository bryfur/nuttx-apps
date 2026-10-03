/****************************************************************************
 * apps/games/quake/fpc_quake.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Quake (quakegeneric: WinQuake's software renderer) on the FPC board: its
 * platform (QG_*), full screen on the GPU as Doom's is (i_fpcscreen.c):
 * the display and the keyboard taken from the desktop, 320 x 200 at 4 x 4
 * in 1280 x 800, each frame one PUT looked up in the palette on its way
 * into the link, two canvases taken in turn.
 *
 * A loadable module: the loader gives it its code and variables for the
 * time it runs.  What the game allocates (fpc_quake.h) comes from a heap
 * of its own and the hunk, which go back whole when it ends (a flat build
 * frees nothing a task allocated itself).
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <malloc.h>

#include <nuttx/kmalloc.h>
#include <nuttx/mm/mm.h>
#include <nuttx/input/kbd_codec.h>
#include <nuttx/nx/nxkeys.h>

#include "fpc_gpu.h"

#define FPCQ_PLATFORM
#include "fpc_quake.h"
#include "quakegeneric.h"
#include "quakekeys.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FS_W          320
#define FS_H          200
#define FS_MODE       FPC_GPU_MODE_1280x800
#define FS_SCALE      4

#define SRAM_MAX      8                   /* Arrays in the SRAM at most */
#define SRAM_RESERVE  (32 * 1024)         /* Left to the system */
#define KEYQ          16                  /* Key events waiting */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct mm_heap_s *g_heap;          /* The game's own */
static void *g_heapmem;
static size_t g_heapsize;
static void *g_hunk;                      /* fpcq_hunk(), apart from it */

static void *g_sram[SRAM_MAX];            /* fpcq_fast_alloc()'s, kmm */
static int g_nsram;
static size_t g_sram_bytes;

static int g_argc;
static char **g_argv;

static struct
{
  bool     on;
  int      back;                          /* The canvas drawn next */
  int      x[2];
  int      y[2];
  uint16_t lut[256];                      /* The palette, RGB565 */
  uint32_t frames;
  uint64_t send_us;
  uint8_t  mods;                          /* NX_KEY_* modifiers held */
  int      keyq[KEYQ];                    /* key | down << 16 */
  int      nkeyq;
} g_fs;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void heap_destroy(void)
{
  int i;

  for (i = 0; i < g_nsram; i++)
    {
      kmm_free(g_sram[i]);
    }

  g_nsram = 0;
  g_sram_bytes = 0;
  if (g_heap != NULL)
    {
      struct mallinfo info = mm_mallinfo(g_heap);

      printf("quake: heap of %zu KB, %d KB used at most\n",
             g_heapsize / 1024, info.usmblks / 1024);
      mm_uninitialize(g_heap);
      g_heap = NULL;
      free(g_heapmem);
      g_heapmem = NULL;
    }

  free(g_hunk);
  g_hunk = NULL;
}

/* The game's memory: the hunk, and a heap of CONFIG_GAMES_QUAKE_HEAPSIZE
 * for the rest, two blocks of the user heap (one of 6 MB and more would
 * often not be had in one piece)
 */

static int heap_create(void)
{
  size_t hunk = fpcq_hunk_size();

  g_hunk = malloc(hunk);
  g_heapsize = (size_t)CONFIG_GAMES_QUAKE_HEAPSIZE * 1024;
  g_heapmem = g_hunk != NULL ? malloc(g_heapsize) : NULL;
  if (g_heapmem == NULL)
    {
      struct mallinfo info = mallinfo();

      fprintf(stderr, "quake: not enough memory: %zu and %zu KB wanted, %d "
              "KB in one piece (-mem N: a smaller hunk, in MB)\n",
              hunk / 1024, g_heapsize / 1024, info.mxordblk / 1024);
      free(g_hunk);
      g_hunk = NULL;
      return -1;
    }

  g_heap = mm_initialize("quake", g_heapmem, g_heapsize);
  if (g_heap == NULL)
    {
      free(g_heapmem);
      g_heapmem = NULL;
      return -1;
    }

  return 0;
}

/* A key of the GPU's game keyboard to Quake's (quakekeys.h): lowercase
 * ASCII for those that type, K_* for the rest; 0 for none
 */

static int quake_key(uint8_t code)
{
  if (NX_KEY_ISSPECIAL(code))
    {
      switch (NX_KEY_KEYCODE(code))
        {
          case KEYCODE_UP:       return K_UPARROW;
          case KEYCODE_DOWN:     return K_DOWNARROW;
          case KEYCODE_LEFT:     return K_LEFTARROW;
          case KEYCODE_RIGHT:    return K_RIGHTARROW;
          case KEYCODE_INSERT:   return K_INS;
          case KEYCODE_FWDDEL:   return K_DEL;
          case KEYCODE_PAGEUP:   return K_PGUP;
          case KEYCODE_PAGEDOWN: return K_PGDN;
          case KEYCODE_HOME:     return K_HOME;
          case KEYCODE_END:      return K_END;
          case KEYCODE_PAUSE:    return K_PAUSE;
          default:
            break;
        }

      if (NX_KEY_KEYCODE(code) >= KEYCODE_F1 &&
          NX_KEY_KEYCODE(code) < KEYCODE_F1 + 12)
        {
          return K_F1 + NX_KEY_KEYCODE(code) - KEYCODE_F1;
        }

      return 0;                           /* (Modifiers: from the flags) */
    }

  switch (code)
    {
      case NX_KEY_ENTER:     return K_ENTER;
      case NX_KEY_TAB:       return K_TAB;
      case NX_KEY_ESCAPE:    return K_ESCAPE;
      case NX_KEY_BACKSPACE: return K_BACKSPACE;
      default:
        return code >= 0x20 && code < 0x7f ? code : 0;
    }
}

static void key_push(int key, int down)
{
  if (key != 0 && g_fs.nkeyq < KEYQ)
    {
      g_fs.keyq[g_fs.nkeyq++] = key | down << 16;
    }
}

/* Shift, Ctrl and Alt as keys of their own, from the modifiers each event
 * carries (whether or not the keyboard reports the modifier keys)
 */

static void mods_changed(uint8_t mods)
{
  static const struct
  {
    uint8_t flag;
    int key;
  } m[] =
  {
    { NX_KEY_SHIFT, K_SHIFT }, { NX_KEY_CTRL, K_CTRL }, { NX_KEY_ALT, K_ALT }
  };

  int i;

  for (i = 0; i < 3; i++)
    {
      if ((mods ^ g_fs.mods) & m[i].flag)
        {
          key_push(m[i].key, (mods & m[i].flag) != 0);
        }
    }

  g_fs.mods = mods;
}

static void fs_close(void)
{
  if (!g_fs.on)
    {
      return;
    }

  g_fs.on = false;
  printf("quake: %u frames sent, %u us each on average\n",
         (unsigned)g_fs.frames,
         (unsigned)(g_fs.frames ? g_fs.send_us / g_fs.frames : 0));
  fpc_gpu_game_free_all();
  fpc_gpu_game_keys(false);
  fpc_gpu_game_display(false);
  fpc_gpu_flush();
}

/****************************************************************************
 * Public Functions: the game's heap (fpc_quake.h)
 ****************************************************************************/

void *fpcq_malloc(size_t size)
{
  return mm_malloc(g_heap, size);
}

void *fpcq_calloc(size_t n, size_t size)
{
  if (size != 0 && n > SIZE_MAX / size)
    {
      return NULL;
    }

  return mm_zalloc(g_heap, n * size);
}

void *fpcq_realloc(void *ptr, size_t size)
{
  return mm_realloc(g_heap, ptr, size);
}

void fpcq_free(void *ptr)
{
  if (ptr != NULL)
    {
      mm_free(g_heap, ptr);
    }
}

void *fpcq_fast_alloc(size_t size, const char *what)
{
  void *p;

#ifdef CONFIG_GAMES_QUAKE_SRAM
  if (g_nsram < SRAM_MAX && kmm_mallinfo().mxordblk >= size + SRAM_RESERVE)
    {
      p = kmm_zalloc(size);
      if (p != NULL)
        {
          g_sram[g_nsram++] = p;
          g_sram_bytes += size;
          printf("quake: %s in the SRAM (%zu bytes)\n", what, size);
          return p;
        }
    }
#endif

  p = fpcq_calloc(1, size);
  if (p == NULL)
    {
      fprintf(stderr, "quake: no memory for %s (%zu bytes)\n", what, size);
      fpcq_exit(1);
    }

  return p;
}

void *fpcq_hunk(void)
{
  return g_hunk;
}

size_t fpcq_hunk_size(void)
{
  int i;

  for (i = 1; i + 1 < g_argc; i++)
    {
      if (strcmp(g_argv[i], "-mem") == 0 && atoi(g_argv[i + 1]) > 0)
        {
          return (size_t)atoi(g_argv[i + 1]) * 1024 * 1024;
        }
    }

  return (size_t)CONFIG_GAMES_QUAKE_HUNKSIZE * 1024;
}

const char *fpcq_basedir(void)
{
  return CONFIG_GAMES_QUAKE_BASEDIR;
}

void fpcq_exit(int code)
{
  fs_close();
  heap_destroy();
  fflush(stdout);
  exit(code);
}

/****************************************************************************
 * Public Functions: the platform (quakegeneric.h)
 ****************************************************************************/

void QG_Init(void)
{
  int k;

  if (fpc_gpu_version() < 8 || fpc_gpu_game_display(true) < 0)
    {
      fprintf(stderr, "quake: the GPU's display is not to be had (a game "
              "has it, or the GPU is older than protocol v8)\n");
      fpcq_exit(1);
    }

  for (k = 0; k < 2; k++)
    {
      if (fpc_gpu_game_alloc(FS_W, FS_H, &g_fs.x[k], &g_fs.y[k]) < 0)
        {
          fpc_gpu_game_free_all();
          fpc_gpu_game_display(false);
          fprintf(stderr, "quake: no room in the GPU's memory\n");
          fpcq_exit(1);
        }

      fpc_gpu_fill(g_fs.x[k], g_fs.y[k], FS_W, FS_H, 0);
    }

  fpc_gpu_game_keys(true);
  fpc_gpu_game_mode(FS_MODE);
  fpc_gpu_set_scale(FS_SCALE, FS_SCALE);
  fpc_gpu_game_flip(g_fs.x[0], g_fs.y[0]);
  fpc_gpu_flush();
  g_fs.back = 1;
  g_fs.on = true;
}

void QG_Quit(void)
{
  fs_close();
}

void QG_DrawFrame(void *pixels)
{
  int k = g_fs.back;
  uint32_t t0;

  if (!g_fs.on)
    {
      return;
    }

  t0 = fpc_gpu_game_us();
  fpc_gpu_put8(g_fs.x[k], g_fs.y[k], FS_W, FS_H, pixels, FS_W, g_fs.lut);
  fpc_gpu_game_flip(g_fs.x[k], g_fs.y[k]);
  fpc_gpu_flush();
  g_fs.send_us += fpc_gpu_game_us() - t0;
  g_fs.frames++;
  g_fs.back = 1 - k;
}

void QG_SetPalette(unsigned char palette[768])
{
  int i;

  for (i = 0; i < 256; i++)
    {
      const uint8_t *c = palette + 3 * i;

      g_fs.lut[i] = (uint16_t)((c[0] >> 3) << 11 | (c[1] >> 2) << 5 |
                               (c[2] >> 3));
    }
}

int QG_GetKey(int *down, int *key)
{
  uint8_t flags;
  uint8_t code;
  uint8_t ch;

  while (g_fs.nkeyq == 0 && g_fs.on &&
         fpc_gpu_game_key(&flags, &code, &ch) > 0)
    {
      mods_changed(flags & NX_KEY_MODS);
      key_push(quake_key(code), (flags & NX_KEY_RELEASE) == 0);
    }

  if (g_fs.nkeyq == 0)
    {
      return 0;
    }

  *key  = g_fs.keyq[0] & 0xffff;
  *down = g_fs.keyq[0] >> 16;
  memmove(g_fs.keyq, g_fs.keyq + 1, --g_fs.nkeyq * sizeof(g_fs.keyq[0]));
  return 1;
}

void QG_GetMouseMove(int *x, int *y)
{
  *x = 0;
  *y = 0;
}

void QG_GetJoyAxes(float *axes)
{
  memset(axes, 0, QUAKEGENERIC_JOY_MAX_AXES * sizeof(float));
}

/****************************************************************************
 * main
 ****************************************************************************/

double Sys_FloatTime(void);

int main(int argc, char *argv[])
{
  double then;

  g_argc = argc;
  g_argv = argv;
  if (heap_create() < 0)
    {
      return EXIT_FAILURE;
    }

  QG_Create(argc, argv);                  /* (Host_Init: QG_Init) */
  then = Sys_FloatTime();
  for (; ; )
    {
      double now = Sys_FloatTime();

      QG_Tick(now - then);
      then = now;
    }

  return EXIT_SUCCESS;
}
