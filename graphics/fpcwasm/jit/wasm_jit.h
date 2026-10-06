/****************************************************************************
 * apps/graphics/fpcwasm/jit/wasm_jit.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A WebAssembly to Thumb-2 (Cortex-M33) single-pass compiler for WAMR
 * (docs/wasm.md, "The JIT").  A module loaded as bytecode and
 * instantiated (WAMR's fast interpreter) has every function compiled at
 * once; from then on WAMR's calls into the module run the compiled code
 * (patch 0005's hook in wasm_interp_call_wasm), with the interpreter's
 * semantics: the same traps, the same exceptions, the terminate request
 * (the watchdog, Ctrl-C) seen at every loop.  A module using what the
 * compiler does not support (SIMD, threads) is left to the interpreter.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCWASM_JIT_WASM_JIT_H
#define __APPS_GRAPHICS_FPCWASM_JIT_WASM_JIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wasm_export.h"

/* wasm_jit_attach()'s flags */

#define WASM_JIT_SRAM     1      /* The code in the internal SRAM if it fits */
#define WASM_JIT_VERBOSE  2      /* What it did, on stderr */
#define WASM_JIT_MAP      4      /* Where each function's code is, on stderr */
#define WASM_JIT_PSRAM    8      /* The code in the PSRAM (neither flag: the
                                  * SRAM when wasm_jit_sram_fits()) */

/* Code (JIT or AOT) goes to the internal SRAM by default when it is at
 * most WASM_SRAM_CODE_MAX bytes and the SRAM keeps a free block of
 * WASM_SRAM_FLOOR bytes beside it
 */

#define WASM_SRAM_CODE_MAX  (160 * 1024)
#define WASM_SRAM_FLOOR     (96 * 1024)

struct wasm_jit_stats_s
{
  uint32_t functions;            /* Compiled */
  uint32_t wasm_bytes;           /* Their bytecode */
  uint32_t code_bytes;           /* The Thumb code (with the imports' stubs) */
  uint32_t compile_us;           /* Time taken */
  uint32_t fast_imports;         /* Host functions called directly */
  uint32_t slow_imports;         /* ... through WAMR's generic call */
  uint32_t peak_bytes;           /* The compiler's own memory, at most */
  bool in_sram;                  /* Where the code is */
};

#ifdef __cplusplus
extern "C"
{
#endif

/* Whether this build has the compiler (a Thumb-2 target) */

bool wasm_jit_supported(void);

/* Compile the instance's module and run its functions compiled from now
 * on.  False (and why, in err) if it cannot: the instance stays
 * interpreted.
 */

bool wasm_jit_attach(wasm_module_inst_t inst, unsigned flags, char *err,
                     size_t errlen);

/* Before the instance is deinstantiated: back to the interpreter, the
 * code freed
 */

void wasm_jit_detach(wasm_module_inst_t inst);

/* After wasm_runtime_terminate() (another thread: a watchdog): the
 * compiled code's loops go to the exception at their next iteration
 * (their heads are patched; nothing looks at the exception otherwise).
 * The instance cannot run its compiled code normally afterwards.
 */

void wasm_jit_interrupt(wasm_module_inst_t inst);

/* Whether size bytes of code go to the SRAM by default (false where there
 * is no such choice: the simulator, the test harness)
 */

bool wasm_jit_sram_fits(uint32_t size);

/* What the compiler did for an attached instance */

bool wasm_jit_stats(wasm_module_inst_t inst, struct wasm_jit_stats_s *st);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_GRAPHICS_FPCWASM_JIT_WASM_JIT_H */
