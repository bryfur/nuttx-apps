/****************************************************************************
 * apps/games/quake/fpc_quake.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Quake (quakegeneric: WinQuake's software renderer) on the FPC board: its
 * platform (QG_*), full screen on the GPU as Doom's is (i_fpcscreen.c):
 * the display and the keyboard taken from the desktop, 320 x 200 shown at
 * 4:3 in the desktop's own mode (or -res W: W x 3W/4), each frame PUT8s of
 * its 8-bit pixels (the GPU looks them up in the palette), two canvases
 * taken in turn.
 *
 * A loadable module: the loader gives it its code and variables for the
 * time it runs.  What the game allocates (fpc_quake.h) comes from a heap
 * of its own and the hunk, which go back whole when it ends (a flat build
 * frees nothing a task allocated itself).
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <malloc.h>

#include <nuttx/kmalloc.h>
#include <nuttx/mm/mm.h>
#include <nuttx/input/kbd_codec.h>
#include <nuttx/nx/nxkeys.h>

#include "fpc_gpu.h"
#include "fpcgl.h"

/* fpc_glq.c's: -gl, the 3D GPU draws the view (fpcgl) */

extern int fpcq_gl;
void fpcq_gl_init(void);
size_t fpcq_gl_bytes(void);
void fpcq_gl_report(void);

/* fpc_gpu3d.c's (its qboolean is the game's: an int here) */

void fpcq_g3_init(void);
void fpcq_g3_report(void);
int  fpcq_g3_drawing(void);
void fpcq_g3_end(void);
const unsigned short *fpcq_base_lut(void);
const unsigned short *fpcq_gpu_lut(void);
void fpcq_dq_wait(void);                  /* fpc_drawq.c */

#define FPCQ_PLATFORM
#include "fpc_quake.h"
#include "quakegeneric.h"
#include "quakekeys.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FS_W          320
#define FS_H          200
#define FS_H43        240                 /* Its rows at 4:3 */
#define FS_MODE       FPC_GPU_MODE_1280x800   /* (-gpu3d, -gl) */
#define FS_SCALE      4
#define RES_MIN       320                 /* -res W */
#define RES_MAX       1024
#define SC_320        (600 * 1024)        /* The surface cache at 320 x 200
                                           * (in GAMES_QUAKE_HEAPSIZE) */
#define SC_LEAST      SC_320
#define HEAP_SPARE    (128 * 1024)        /* PSRAM left to the system (-res) */

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
static size_t g_surfcache;                /* fpcq_surfcache_size() */
static size_t g_hunksize;                 /* -res's (else 0) */

static struct
{
  bool     on;
  int      w;                             /* The software renderer's frame */
  int      h;
  bool     res;                           /* -res (else 320 x 200) */
  int      back;                          /* The canvas drawn next */
  int      x[2];
  int      y[2];
  bool     aspect;                        /* 4:3 in the desktop's mode */
  int      cw;                            /* A canvas */
  int      ch;
  int      ox;                            /* The frame in it */
  int      oy;
  uint16_t lut[256];                      /* The palette, RGB565 */
  uint16_t base[256];                     /* ... without the tints (the GPU's world) */
  bool baseok;
  bool basesent;                          /* The GPU has base (PUT8 with NULL) */
  uint16_t tint;                          /* The tints as one BLEND (RGB565, alpha) */
  uint8_t tintalpha;
  uint32_t frames;
  uint32_t hold;                          /* -holdframe */
  bool     hash;                          /* -framehash */
  uint32_t fnv;                           /* ... its hash so far */
  uint32_t nhashed;
  uint32_t hashes[128];                   /* ... each frame's */
  uint64_t send_us;
  uint8_t  mods;                          /* NX_KEY_* modifiers held */
  int      keyq[KEYQ];                    /* key | down << 16 */
  int      nkeyq;
} g_fs;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* In the 64-bit simulator the hunk and the heap are static arrays: QuakeC's
 * strings are 32-bit offsets from its string table (in the hunk), also to
 * strings in the game's static data (sv.name, the models' names, PF_ftos's
 * buffer), which the host's heap is more than 2 GB away from
 */

#if defined(CONFIG_ARCH_SIM) && UINTPTR_MAX > 0xffffffffu
#  define SIM_HUNK_MAX  (32 * 1024 * 1024)
static uint8_t g_sim_hunk[SIM_HUNK_MAX] aligned_data(16);
static uint8_t g_sim_heap[CONFIG_GAMES_QUAKE_HEAPSIZE * 1024] aligned_data(16);
#  define hunk_alloc(n) ((n) <= SIM_HUNK_MAX ? (void *)g_sim_hunk : NULL)
#  define heap_alloc(n) ((void *)g_sim_heap)
#  define hunk_free(p)
#  define heap_free(p)
#else
#  define hunk_alloc(n) malloc(n)
#  define heap_alloc(n) malloc(n)
#  define hunk_free(p)  free(p)
#  define heap_free(p)  free(p)
#endif

/* The software renderer's frame: 320 x 200, shown at 4:3 (its rows
 * stretched, 200 as 240); or -res W: W x 3W/4 (W from RES_MIN to RES_MAX,
 * a multiple of 8, at most what the desktop's mode shows whole), square
 * pixels.  -gpu3d and -gl draw in 320 x 200.
 */

static void res_choose(void)
{
  int w = 0;
  int mw;
  int mh;
  int k;

  g_fs.w = FS_W;
  g_fs.h = FS_H;
  for (k = 1; k < g_argc; k++)
    {
      if (k + 1 < g_argc && strcmp(g_argv[k], "-res") == 0)
        {
          w = atoi(g_argv[k + 1]);
        }
    }

  for (k = 1; k < g_argc && w > 0; k++)
    {
      if (strcmp(g_argv[k], "-gl") == 0 || strcmp(g_argv[k], "-gpu3d") == 0)
        {
          printf("quake: -res: not with %s (320 x 200)\n", g_argv[k]);
          w = 0;
        }
    }

  if (w <= 0)
    {
      return;
    }

  if (fpc_gpu_modesize(fpc_gpu_mode(), &mw, &mh) == 0)
    {
      w = w > mw ? mw : w;
      w = w > mh * 4 / 3 ? mh * 4 / 3 : w;
    }

  w = w < RES_MIN ? RES_MIN : w > RES_MAX ? RES_MAX : w;
  g_fs.w = w & ~7;
  g_fs.h = (g_fs.w * 3 / 4) & ~1;
  g_fs.res = true;
}

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
      heap_free(g_heapmem);
      g_heapmem = NULL;
    }

  hunk_free(g_hunk);
  g_hunk = NULL;
}

/* -res: the game's memory for a larger frame.  The frame and the depth
 * buffer, from the heap where the SRAM has no room for them (as
 * fpcq_fast_alloc() will find), and the surface cache the game would have
 * for the size (D_SurfaceCacheForRes; -surfcachesize N in KB), in the
 * PSRAM with the hunk, HEAP_SPARE left over: what does not fit comes off
 * the hunk (without -mem), by at most FPCQ_HUNK_VIDEO (what the hunk holds
 * besides the level is the models' cache), then off the surface cache,
 * down to its size at 320 x 200
 */

static void res_memory(size_t *hunk)
{
  size_t px = (size_t)g_fs.w * g_fs.h;
  size_t avail = (size_t)mallinfo().mxordblk;
  size_t inheap = px * 3;
  size_t fixed;
  size_t least;
  size_t sc;
  bool mem = false;
  int k;

#ifdef CONFIG_GAMES_QUAKE_SRAM
  size_t sram = (size_t)kmm_mallinfo().mxordblk;

  if (sram >= px + SRAM_RESERVE)          /* The frame */
    {
      inheap -= px;
      sram -= px;
    }

  if (sram >= 2 * px + SRAM_RESERVE)      /* The depth buffer */
    {
      inheap -= 2 * px;
    }
#endif

  sc = SC_320 + (px - FS_W * FS_H) * 3;
  for (k = 1; k + 1 < g_argc; k++)
    {
      if (strcmp(g_argv[k], "-surfcachesize") == 0)
        {
          sc = (size_t)atoi(g_argv[k + 1]) * 1024;
        }

      mem |= strcmp(g_argv[k], "-mem") == 0;
    }

  fixed = g_heapsize - SC_320 + inheap;
  avail = avail > HEAP_SPARE + fixed ? avail - HEAP_SPARE - fixed : 0;
  if (!mem && *hunk + sc > avail)
    {
      least = *hunk - FPCQ_HUNK_VIDEO;
      *hunk = avail > least + sc ? avail - sc : least;
    }

  if (*hunk + sc > avail)
    {
      sc = avail > *hunk + SC_LEAST ? avail - *hunk : SC_LEAST;
    }

  g_surfcache = sc;
  g_heapsize = fixed + sc;
  printf("quake: %d x %d: a hunk of %zu KB, a heap of %zu KB with a "
         "surface cache of %zu KB\n", g_fs.w, g_fs.h, *hunk / 1024,
         g_heapsize / 1024, sc / 1024);
}

/* The game's memory: the hunk, and a heap of CONFIG_GAMES_QUAKE_HEAPSIZE
 * for the rest, two blocks of the user heap (one of 6 MB and more would
 * often not be had in one piece)
 */

static int heap_create(void)
{
  size_t hunk = fpcq_hunk_size();
  int k;

  g_heapsize = (size_t)CONFIG_GAMES_QUAKE_HEAPSIZE * 1024;
  for (k = 1; k < g_argc; k++)
    {
      if (strcmp(g_argv[k], "-gl") == 0)
        {
          g_heapsize += fpcq_gl_bytes();  /* (fpc_glq.c's state) */
        }
    }

  if (g_fs.res)
    {
      res_memory(&hunk);
      g_hunksize = hunk;
    }

  g_hunk = hunk_alloc(hunk);
  g_heapmem = g_hunk != NULL ? heap_alloc(g_heapsize) : NULL;
  if (g_heapmem == NULL)
    {
      struct mallinfo info = mallinfo();

      fprintf(stderr, "quake: not enough memory: %zu and %zu KB wanted, %d "
              "KB in one piece (-mem N: a smaller hunk, in MB)\n",
              hunk / 1024, g_heapsize / 1024, info.mxordblk / 1024);
      hunk_free(g_hunk);
      g_hunk = NULL;
      return -1;
    }

  g_heap = mm_initialize("quake", g_heapmem, g_heapsize);
  if (g_heap == NULL)
    {
      heap_free(g_heapmem);
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
  if (fpcq_gl)
    {
      fpcq_gl_report();
      printf("quake: %u frames\n", (unsigned)g_fs.frames);
      fpc_gpu_game_keys(false);
      fglShutdown();
      return;
    }

  fpcq_g3_report();
  printf("quake: %u frames sent, %u us each on average\n",
         (unsigned)g_fs.frames,
         (unsigned)(g_fs.frames ? g_fs.send_us / g_fs.frames : 0));
  if (g_fs.hash)
    {
      uint32_t i;

      printf("quake: frame hash %08x (%u frames)\n", (unsigned)g_fs.fnv,
             (unsigned)g_fs.nhashed);
      for (i = 0; i < g_fs.nhashed && i < 128; i++)
        {
          printf("%08x%s", (unsigned)g_fs.hashes[i],
                 i % 8 == 7 ? "\n" : " ");
        }

      printf("\n");
    }
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

  if (g_hunksize != 0)
    {
      return g_hunksize;
    }

  for (i = 1; i + 1 < g_argc; i++)
    {
      if (strcmp(g_argv[i], "-mem") == 0 && atoi(g_argv[i + 1]) > 0)
        {
          return (size_t)atoi(g_argv[i + 1]) * 1024 * 1024;
        }
    }

  return (size_t)CONFIG_GAMES_QUAKE_HUNKSIZE * 1024;
}

void fpcq_vid_size(int *w, int *h)
{
  *w = g_fs.w;
  *h = g_fs.h;
}

/* The 3D view's pixel aspect (vid.aspect): the software renderer's frame
 * is shown at 4:3 (QG_Init()), so its pixels are h / w * 4 / 3 as tall as
 * wide (0.833 for 320 x 200, as on DOS Quake's 4:3 screens; 1 for -res's
 * square pixels); -gpu3d and -gl show 320 x 200 at 16:10, square
 */

float fpcq_vid_aspect(void)
{
  int k;

  for (k = 1; k < g_argc; k++)
    {
      if (strcmp(g_argv[k], "-gpu3d") == 0 || strcmp(g_argv[k], "-gl") == 0)
        {
          return 1.0f;
        }
    }

  return (float)g_fs.h / (float)g_fs.w * 4.0f / 3.0f;
}

size_t fpcq_surfcache_size(void)
{
  return g_surfcache;
}

const char *fpcq_basedir(void)
{
  return CONFIG_GAMES_QUAKE_BASEDIR;
}

void fpcq_exit(int code)
{
  fpcq_dq_shutdown();                     /* (It runs the game's code) */
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
  extern void Host_Frame(float time);
  extern void D_DrawSpans8(void *pspans);
  int w;
  int h;
  int s;
  int k;

  /* Where the loader put the code (a profile's symbols: the module's
   * relocated by these)
   */

  syslog(LOG_INFO, "quake: Host_Frame at %p, D_DrawSpans8 at %p\n",
         (void *)Host_Frame, (void *)D_DrawSpans8);

  for (k = 1; k < g_argc; k++)
    {
      if (strcmp(g_argv[k], "-gl") == 0)
        {
          fpcq_gl = 1;
        }
    }

  if (fpcq_gl)
    {
      /* The 3D GPU (fpcgl): the view drawn there, the 2D parts over it */

      int ret = fglInit(FS_W, FS_H, FS_MODE);

      if (ret < 0)
        {
          fprintf(stderr, "quake: -gl: no 3D GPU (%d)\n", ret);
          fpcq_exit(1);
        }

      fpcq_gl_init();
      fpc_gpu_game_keys(true);
      g_fs.on = true;
      return;
    }

  if (fpc_gpu_version() < 8 || fpc_gpu_game_display(true) < 0)
    {
      fprintf(stderr, "quake: the GPU's display is not to be had (a game "
              "has it, or the GPU is older than protocol v8)\n");
      fpcq_exit(1);
    }

  /* The software renderer's frame at 4:3, as on the CRTs Quake was drawn
   * for, in the desktop's own display mode: 200 rows as 240 (every fifth
   * row twice), scaled the most whole times that fit (3 x 3, 960 x 720,
   * in 1280 x 800, 1280 x 720 and 1024 x 768; 2 x 2 in 800 x 600), in the
   * middle with black around.  -res W's frame is 4:3 itself (480 x 360:
   * 2 x 2 in 1280 x 720 or 800; 640 x 480: 1 x 1).  -gpu3d draws its world
   * in 320 x 200 on the GPU: 1280 x 800 at 4 x 4 as before
   */

  g_fs.aspect = true;
  for (k = 1; k < g_argc; k++)
    {
      if (strcmp(g_argv[k], "-gpu3d") == 0)
        {
          g_fs.aspect = false;
        }
    }

  s = FS_SCALE;
  g_fs.cw = g_fs.w;
  g_fs.ch = g_fs.h;
  g_fs.ox = 0;
  g_fs.oy = 0;
  if (g_fs.aspect && fpc_gpu_modesize(fpc_gpu_mode(), &w, &h) == 0)
    {
      int h43 = g_fs.res ? g_fs.h : FS_H43;

      s = w / g_fs.w < h / h43 ? w / g_fs.w : h / h43;
      s = s < 1 ? 1 : s > 4 ? 4 : s;
      g_fs.cw = (w + s - 1) / s;
      g_fs.ch = (h + s - 1) / s;
      g_fs.ox = ((g_fs.cw - g_fs.w) / 2) & ~1;   /* (PUT8: x even) */
      g_fs.oy = (g_fs.ch - h43) / 2;
    }
  else
    {
      g_fs.aspect = false;
    }

  for (k = 0; k < 2; k++)
    {
      int r = fpc_gpu_game_alloc(g_fs.cw, g_fs.ch, &g_fs.x[k], &g_fs.y[k]);

      if (r < 0 && k == 1 && g_fs.res)
        {
          /* -res at 1 x 1: no room for a second whole screen, the one
           * drawn as it is shown (a frame may tear)
           */

          g_fs.x[1] = g_fs.x[0];
          g_fs.y[1] = g_fs.y[0];
          syslog(LOG_INFO, "quake: one canvas (no room for two)\n");
          break;
        }

      if (r < 0)
        {
          fpc_gpu_game_free_all();
          fpc_gpu_game_display(false);
          fprintf(stderr, "quake: no room in the GPU's memory\n");
          fpcq_exit(1);
        }

      fpc_gpu_fill(g_fs.x[k], g_fs.y[k], g_fs.cw, g_fs.ch, 0);
    }

  syslog(LOG_INFO, "quake: canvases at (%d, %d) (%d, %d), %d x %d at "
         "%d x %d%s, the frame (%d x %d) at (+%d, +%d)\n", g_fs.x[0],
         g_fs.y[0], g_fs.x[1], g_fs.y[1], g_fs.cw, g_fs.ch, s, s,
         g_fs.aspect ? " (4:3)" : "", g_fs.w, g_fs.h, g_fs.ox,
         g_fs.oy);                        /* (grab.py) */
  for (k = 1; k + 1 < g_argc; k++)
    {
      if (strcmp(g_argv[k], "-holdframe") == 0)
        {
          g_fs.hold = atoi(g_argv[k + 1]);
        }
    }

  for (k = 1; k < g_argc; k++)
    {
      if (strcmp(g_argv[k], "-framehash") == 0)
        {
          g_fs.hash = true;
          g_fs.fnv = 2166136261u;
        }
    }

  fpcq_g3_init();                         /* (the atlas: after the canvases) */
  fpc_gpu_game_keys(true);
  if (!g_fs.aspect)
    {
      fpc_gpu_game_mode(FS_MODE);
    }

  fpc_gpu_set_scale(s, s);
  fpc_gpu_game_flip(g_fs.x[0], g_fs.y[0]);
  fpc_gpu_flush();
  g_fs.back = 1;
  g_fs.on = true;
}

void QG_Quit(void)
{
  fs_close();
}

/* The GPU drew the world: what was drawn here goes over it, the rows
 * with something in them (bands of them, each as wide as what is in it),
 * keyed (255: nothing drawn), in the base palette
 */

static void overlay_send(int cx, int cy, const uint8_t *pixels)
{
  int y0 = -1;
  int x0 = FS_W;
  int x1 = 0;
  int y;

  for (y = 0; y <= FS_H; y++)
    {
      int l = FS_W;
      int r = -1;

      if (y < FS_H)
        {
          const uint32_t *w = (const uint32_t *)(pixels + y * FS_W);
          int i;

          for (i = 0; i < FS_W / 4; i++)      /* (a word of four at a time) */
            {
              if (w[i] != 0xffffffffu)
                {
                  if (l == FS_W)
                    {
                      l = i * 4;
                    }

                  r = i * 4 + 3;
                }
            }
        }

      if (r >= 0)                             /* Something: in the band */
        {
          if (y0 < 0)
            {
              y0 = y;
            }

          x0 = l < x0 ? l : x0;
          x1 = r > x1 ? r : x1;
        }
      else if (y0 >= 0)                       /* The band ends */
        {
          fpc_gpu_put8_key(cx + x0, cy + y0, x1 - x0 + 1,
                           y - y0, pixels + y0 * FS_W + x0, FS_W,
                           fpcq_gpu_lut(), 255);
          y0 = -1;
          x0 = FS_W;
          x1 = 0;
        }
    }
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
  if (fpcq_gl)
    {
      int x;
      int y;

      /* The view is drawn (fpc_glq.c): the 2D parts over it, keyed */

      fglBackBuffer(&x, &y);
      overlay_send(x, y, pixels);
      fglSwapBuffers();
      g_fs.frames++;
      return;
    }

  if (fpcq_g3_drawing())
    {
      fpcq_dq_wait();                     /* (the world's spans first) */

      /* The GPU drew the world: the frame over it (the key where nothing
       * was drawn here), in the base palette, then the tints as a BLEND
       */

      overlay_send(g_fs.x[k], g_fs.y[k], pixels);
      if (g_fs.tintalpha > 0)
        {
          struct fpc_gpu_blend_s b;

          memset(&b, 0, sizeof(b));
          b.dx = g_fs.x[k];
          b.dy = g_fs.y[k];
          b.w = FS_W;
          b.h = FS_H;
          b.colour = g_fs.tint;
          b.alpha = g_fs.tintalpha;
          b.mode = FPC_BLEND_SRC_COLOUR;
          fpc_gpu_blend(&b);
        }

      fpcq_g3_end();
    }
  else
    {
      if (g_fs.aspect && !g_fs.res)
        {
          int g;

          /* 200 rows as 240: each group of five rows as six, its middle
           * one twice (rows 0-2 and 2-4 of the group, a PUT each)
           */

          for (g = 0; g < FS_H / 5; g++)
            {
              const uint8_t *src = pixels + g * 5 * FS_W;
              int y = g_fs.y[k] + g_fs.oy + g * 6;

              fpc_gpu_put8(g_fs.x[k] + g_fs.ox, y, FS_W, 3, src, FS_W,
                           g_fs.lut);
              fpc_gpu_put8(g_fs.x[k] + g_fs.ox, y + 3, FS_W, 3,
                           src + 2 * FS_W, FS_W, g_fs.lut);
            }
        }
      else
        {
          /* (-res: 4:3 itself) */

          fpc_gpu_put8(g_fs.x[k] + g_fs.ox, g_fs.y[k] + g_fs.oy, g_fs.w,
                       g_fs.h, pixels, g_fs.w, g_fs.lut);
        }

      g_fs.basesent = false;              /* (the GPU has the tinted one) */
    }

  fpc_gpu_game_flip(g_fs.x[k], g_fs.y[k]);
  fpc_gpu_flush();
  g_fs.send_us += fpc_gpu_game_us() - t0;
  g_fs.frames++;
  g_fs.back = 1 - k;

  /* -framehash: every tenth frame's hash (FNV-1a of its pixels), logged at
   * the end, each (the first 128) and all of them with the palettes folded
   * into one.  With +con_notifytime 0 +host_framerate 0.05 most of a
   * timedemo's frames are the same from run to run, but not all: some
   * particles are not (-siocheck, fpc_sio.c, is the exact check of the
   * interpolators' loops)
   */

  if (g_fs.hash && g_fs.frames % 10 == 0)
    {
      const uint8_t *p = pixels;
      uint32_t f = 2166136261u;
      uint32_t h = g_fs.fnv;
      int i;

      for (i = 0; i < g_fs.w * g_fs.h; i++)
        {
          f = (f ^ p[i]) * 16777619u;
        }

      for (i = 0; i < 4; i++)
        {
          h = (h ^ ((f >> (8 * i)) & 0xff)) * 16777619u;
        }

      for (i = 0; i < 256; i++)
        {
          h = (h ^ g_fs.lut[i]) * 16777619u;
        }

      if (g_fs.nhashed < 128)
        {
          g_fs.hashes[g_fs.nhashed] = f;
        }

      g_fs.fnv = h;
      g_fs.nhashed++;
    }

  /* -holdframe N: frame N's hash (FNV-1a of its 8-bit pixels: the same
   * picture, the same hash), and the frame kept on the screen a while
   * (for grab.py: a renderer's change compared)
   */

  if (g_fs.frames == g_fs.hold)
    {
      const uint8_t *p = pixels;
      uint32_t h = 2166136261u;
      int i;

      for (i = 0; i < g_fs.w * g_fs.h; i++)
        {
          h = (h ^ p[i]) * 16777619u;
        }

      syslog(LOG_INFO, "quake: frame %" PRIu32 " hash %08" PRIx32
             " (canvas (%d, %d)), held 40 s\n", g_fs.frames, h,
             g_fs.x[k] + g_fs.ox, g_fs.y[k] + g_fs.oy);
      sleep(40);
    }
}

/* The GPU's world (fpc_gpu3d.c): the canvas being drawn, the palette
 * without the tints (the atlas's texels, and the frame over them), the
 * tints (view.c V_UpdatePalette)
 */

void fpcq_canvas(int *x, int *y)
{
  *x = g_fs.x[g_fs.back];
  *y = g_fs.y[g_fs.back];
}

const unsigned short *fpcq_base_lut(void)
{
  return g_fs.baseok ? g_fs.base : g_fs.lut;
}

/* For a PUT8 of the GPU's world: the base palette the first time after it
 * changed (or another was sent), else NULL (the GPU's, no compare)
 */

const unsigned short *fpcq_gpu_lut(void)
{
  if (g_fs.basesent && g_fs.baseok)
    {
      return NULL;
    }

  g_fs.basesent = g_fs.baseok;
  return fpcq_base_lut();
}

void fpcq_palette_parts(const unsigned char *base, int r, int g, int b,
                        int alpha)
{
  int i;

  for (i = 0; i < 256; i++)
    {
      const uint8_t *c = base + 3 * i;

      g_fs.base[i] = (uint16_t)((c[0] >> 3) << 11 | (c[1] >> 2) << 5 |
                                (c[2] >> 3));
    }

  g_fs.baseok = true;
  g_fs.basesent = false;
  g_fs.tint = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
  g_fs.tintalpha = alpha < 0 ? 0 : alpha > 255 ? 255 : alpha;
}

/* The tints as one blend (fpc_glq.c: over the view) */

void fpcq_tint(uint16_t *colour, int *alpha)
{
  *colour = g_fs.tint;
  *alpha = g_fs.tintalpha;
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

  /* The desktop's windows give back the memory their copies take in the
   * CPU's (the compositor's swap) before the game takes most of it
   */

  fpc_gpu_game_prepare();
  res_choose();
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
#ifdef CONFIG_ARCH_SIM
      usleep(1000);                       /* (the simulator's ticks come only
                                           * when nothing runs) */
#endif
    }

  return EXIT_SUCCESS;
}
