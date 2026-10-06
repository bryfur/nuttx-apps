/****************************************************************************
 * apps/graphics/fpcdesk/desk_wasm.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebAssembly apps in the launcher (desk_wasm.c, docs/wasm.md)
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_WASM_H
#define __APPS_GRAPHICS_FPCDESK_DESK_WASM_H

#include "desk.h"

#define DESK_WASM_MAX 16                 /* Apps listed at most */

#ifdef CONFIG_GRAPHICS_FPCWASM
void desk_wasm_scan(void);               /* The apps installed now */
int  desk_wasm_count(void);
const char *desk_wasm_name(int i);
void desk_wasm_icon(struct desk_ops_s *o, int i, int x, int y, int s,
                    uint8_t alpha);
bool desk_wasm_launch(int i);            /* wasm run its directory */
#else
#  define desk_wasm_scan()          do { } while (0)
#  define desk_wasm_count()         0
#  define desk_wasm_name(i)         ""
#  define desk_wasm_icon(o, i, x, y, s, a) do { } while (0)
#  define desk_wasm_launch(i)       false
#endif

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_WASM_H */
