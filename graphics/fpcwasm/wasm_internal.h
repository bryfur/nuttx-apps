/****************************************************************************
 * apps/graphics/fpcwasm/wasm_internal.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The WebAssembly app runtime's own (wasm_main.c, wasm_fpc.c,
 * wasm_image.c): the app that runs, the host API's registration.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCWASM_WASM_INTERNAL_H
#define __APPS_GRAPHICS_FPCWASM_WASM_INTERNAL_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#include "wasm_export.h"

#define WASM_API_VERSION   1              /* sdk/fpc_wasm.h's FPC_API_VERSION */

/* The app that runs (one a task: the runtime is a program of its own) */

struct wasm_app_s
{
  char name[32];                          /* For the log and the mixer */
  char dir[PATH_MAX];                     /* Its directory ("" none) */
  wasm_module_inst_t inst;
  wasm_exec_env_t env;
  const char *script;                     /* -s: fpcgame's script */
  volatile bool in_game;                  /* fpc_game_run() is running */
  volatile uint32_t beat;                 /* A callback returned (the watchdog) */
  volatile bool in_callback;              /* In fpc_update/fpc_draw ... */
  volatile bool killed;                   /* The watchdog terminated it */
  bool trapped;                           /* A callback trapped */
  int game_w;                             /* For an app with no _start */
  int game_h;
};

extern struct wasm_app_s g_wapp;

/* wasm_fpc.c: the "env" module's fpc_* functions */

bool wasm_fpc_register(void);
void wasm_fpc_game_end(void);             /* Free what the game held */
int  wasm_fpc_run_game(int w, int h);     /* fpc_game_run(), from the host */

/* wasm_image.c: an image file's bytes into a new fpcgame image */

int  wasm_image_decode(const uint8_t *data, size_t len);

#endif /* __APPS_GRAPHICS_FPCWASM_WASM_INTERNAL_H */
