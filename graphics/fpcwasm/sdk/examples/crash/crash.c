/****************************************************************************
 * apps/graphics/fpcwasm/sdk/examples/crash/crash.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The sandbox's test (docs/wasm.md): a game that misbehaves on purpose
 * after a few frames, with the display and the keyboard taken.  The
 * runtime must end it, give them back to the desktop and go on.
 *
 *   wasm run crash MODE [FRAME]
 *
 *   oob     reads far outside its linear memory          (a trap)
 *   ptr     gives the host a buffer outside it            (a trap)
 *   div     divides by zero                               (a trap)
 *   stack   recurses without end                          (a trap)
 *   abort   abort(): unreachable                          (a trap)
 *   loop    never returns from fpc_update                 (the watchdog)
 *   exit    exit(3) in the middle of a frame              (status 3)
 *   grow    allocates until the memory limit              (malloc fails)
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpc_wasm.h"

static const char *g_mode = "oob";
static uint32_t g_at = 10;
static volatile int g_zero;
static volatile uint32_t g_far = 0x7ffffff0u;
static void *volatile g_list;

#ifdef __clang__
#  pragma clang diagnostic ignored "-Winfinite-recursion"
#endif

static int deep(int n)
{
  volatile char pad[256];

  pad[n & 255] = (char)n;
  return deep(n + 1) + pad[(n + 1) & 255];
}

FPC_EXPORT(fpc_update) void fpc_update(void)
{
  if (fpc_game_frame() < g_at)
    {
      return;
    }

  if (strcmp(g_mode, "oob") == 0)
    {
      printf("crash: %d\n", *(volatile int *)(uintptr_t)g_far);
    }
  else if (strcmp(g_mode, "ptr") == 0)
    {
      fpc_image_put(0, 0, 0, 64, 64, (const uint16_t *)(uintptr_t)g_far,
                    64);
    }
  else if (strcmp(g_mode, "div") == 0)
    {
      printf("crash: %d\n", 100 / g_zero);
    }
  else if (strcmp(g_mode, "stack") == 0)
    {
      printf("crash: %d\n", deep(0));
    }
  else if (strcmp(g_mode, "abort") == 0)
    {
      abort();
    }
  else if (strcmp(g_mode, "loop") == 0)
    {
      volatile uint32_t n = 0;

      for (; ; )
        {
          n++;
        }
    }
  else if (strcmp(g_mode, "exit") == 0)
    {
      exit(3);
    }
  else if (strcmp(g_mode, "grow") == 0)
    {
      size_t total = 0;
      void **p;

      /* (a list of them: the compiler may not drop allocations it sees
       * no use of)
       */

      while ((p = malloc(256 * 1024)) != NULL)
        {
          memset(p, 1, 256 * 1024);
          *p = g_list;
          g_list = p;
          total += 256 * 1024;
        }

      printf("crash: malloc failed after %u KB\n", (unsigned)(total / 1024));
      fpc_game_exit();
    }
}

FPC_EXPORT(fpc_draw) void fpc_draw(void)
{
  char line[64];

  fpc_clear(FPC_RGB(120, 0, 0));
  fpc_rect(20, 20, 384, 200, FPC_RGB(40, 0, 0));
  snprintf(line, sizeof(line), "CRASH TEST: %s AT FRAME %u", g_mode,
           (unsigned)g_at);
  fpc_text_print(4, 4, line, (int32_t)strlen(line));
  snprintf(line, sizeof(line), "frame %u", (unsigned)fpc_game_frame());
  fpc_text_print(4, 6, line, (int32_t)strlen(line));
  fpc_text_draw();
}

int main(int argc, char **argv)
{
  int ret;

  if (argc > 1)
    {
      g_mode = argv[1];
    }

  if (argc > 2)
    {
      g_at = (uint32_t)atoi(argv[2]);
    }

  ret = fpc_game_run(424, 240);
  printf("crash: the game returned %d\n", ret);
  return 0;
}
