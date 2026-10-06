/****************************************************************************
 * apps/graphics/fpcwasm/jit/jit_internal.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the WebAssembly JIT's compiler (wasm_jit.c) and its runtime
 * (wasm_jit_rt.c) share: the instance words the code reaches, the traps,
 * the helpers it calls, a compiled module.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCWASM_JIT_JIT_INTERNAL_H
#define __APPS_GRAPHICS_FPCWASM_JIT_JIT_INTERNAL_H

#include "wasm_runtime.h"
#include "wasm_jit.h"

/* The code's registers: r11 the module instance, r10 the linear memory's
 * base, r9 its limit for 4-byte accesses (EXT_LIM4: an access at a below
 * it is in bounds); all of them callee-saved, so native code keeps them.
 * ip is a scratch register.
 */

#define JR_INST  11
#define JR_MEM   10
#define JR_LIM4  9

/* WASMModuleInstance.ext_words[] (patch 0005): reached from r11 */

#define EXT_LIMIT    0     /* Native stack: a frame may not go below */
#define EXT_TRAP_SP  1     /* The innermost entry's sp, to unwind to */
#define EXT_ENV      2     /* The exec env of the call running */
#define EXT_FUNCS    3     /* jit_fent_t[]: every function's code, type */
#define EXT_AUXB     4     /* exec_env->aux_stack_boundary */
#define EXT_AUXT     5     /* exec_env->aux_stack_bottom */
#define EXT_LIM4     6     /* The memory's size - 3 (0 under 4 bytes) */

#define EXT_OFF(n)   (offsetof(WASMModuleInstance, ext_words) + 4 * (n))

/* Room left on the native stack, beyond WAMR's own guard: for a JIT
 * frame (sp may not go below boundary + JIT_STACK_MARGIN) and for a call
 * of a host function (boundary + JIT_STACK_HOST: EXT_LIMIT + the
 * difference)
 */

#define JIT_STACK_MARGIN      512
#define JIT_STACK_HOST        2048

/* Traps (jit_trap_msg[]) */

enum
{
  JT_UNREACHABLE,
  JT_OOB,
  JT_DIV0,
  JT_OVERFLOW,
  JT_CONV,
  JT_UNDEF_ELEM,
  JT_UNINIT_ELEM,
  JT_TYPE,
  JT_STACK,
  JT_AUX_OVERFLOW,
  JT_AUX_UNDERFLOW,
  JT_UNKNOWN_FUNC,
  JT_TABLE_OOB,
  JT_COUNT
};

extern const char *const jit_trap_msg[JT_COUNT];

/* A function's entry in the table the code calls through (call_indirect)
 * and the runtime enters by: its code (Thumb, bit 0 set), its type
 * (the index of the first type equal to it)
 */

typedef struct jit_fent_s
{
  uint32_t code;
  uint32_t typeid;
} jit_fent_t;

/* Entering compiled code: buf holds the arguments (4 words for r0-r3,
 * then reserve words of stack arguments, even), and gets the results (2
 * words; more than 2: after the 4, from the stack area)
 */

typedef int (*jit_enter_t)(uint32_t *buf, uint32_t reserve, uint32_t code,
                           WASMModuleInstance *inst);
typedef void (*jit_unwind_t)(WASMModuleInstance *inst);

typedef struct jit_module_s
{
  uint8_t *code;                  /* The executable memory */
  uint32_t code_size;
  bool in_sram;
  jit_fent_t *funcs;              /* Imports then functions */
  uint32_t nfuncs;
  uint32_t nimports;
  jit_enter_t enter;
  jit_unwind_t unwind;
  uint32_t fast_imports;          /* Imports called directly */
  struct wasm_jit_stats_s stats;
} jit_module_t;

/* The runtime (wasm_jit_rt.c) */

void jit_set_lim4(WASMModuleInstance *inst);
void jit_raise(WASMModuleInstance *inst, uint32_t code);
void jit_trap(WASMModuleInstance *inst, uint32_t code);  /* Never returns */
void jit_call_import(WASMModuleInstance *inst, uint32_t idx, uint32_t *argv);
uint32_t jit_mem_grow(WASMModuleInstance *inst, uint32_t delta);
void jit_mem_init(WASMModuleInstance *inst, uint32_t seg, uint32_t dst,
                  uint32_t src, uint32_t len);
void jit_data_drop(WASMModuleInstance *inst, uint32_t seg);
void jit_mem_copy(WASMModuleInstance *inst, uint32_t dst, uint32_t src,
                  uint32_t len);
void jit_mem_fill(WASMModuleInstance *inst, uint32_t dst, uint32_t val,
                  uint32_t len);
void jit_table_init(WASMModuleInstance *inst, uint32_t segtbl, uint32_t dst,
                    uint32_t src, uint32_t len);
void jit_elem_drop(WASMModuleInstance *inst, uint32_t seg);
void jit_table_copy(WASMModuleInstance *inst, uint32_t tbls, uint32_t dst,
                    uint32_t src, uint32_t len);
uint32_t jit_table_grow(WASMModuleInstance *inst, uint32_t tbl,
                        uint32_t init, uint32_t n);
void jit_table_fill(WASMModuleInstance *inst, uint32_t tbl, uint32_t i,
                    uint32_t val, uint32_t n);
uint32_t jit_table_get(WASMModuleInstance *inst, uint32_t tbl, uint32_t i);
void jit_table_set(WASMModuleInstance *inst, uint32_t tbl, uint32_t i,
                   uint32_t val);

/* Arithmetic (bit patterns in, bit patterns out: no float registers) */

uint32_t jit_i32_popcnt(uint32_t a);
uint64_t jit_i64_div_s(uint64_t a, uint64_t b);
uint64_t jit_i64_div_u(uint64_t a, uint64_t b);
uint64_t jit_i64_rem_s(uint64_t a, uint64_t b);
uint64_t jit_i64_rem_u(uint64_t a, uint64_t b);
uint64_t jit_i64_shl(uint64_t a, uint32_t n);
uint64_t jit_i64_shr_s(uint64_t a, uint32_t n);
uint64_t jit_i64_shr_u(uint64_t a, uint32_t n);
uint64_t jit_i64_rotl(uint64_t a, uint32_t n);
uint64_t jit_i64_rotr(uint64_t a, uint32_t n);
uint64_t jit_i64_ctz(uint64_t a);
uint64_t jit_i64_popcnt(uint64_t a);

uint64_t jit_f64_add(uint64_t a, uint64_t b);
uint64_t jit_f64_sub(uint64_t a, uint64_t b);
uint64_t jit_f64_mul(uint64_t a, uint64_t b);
uint64_t jit_f64_div(uint64_t a, uint64_t b);
uint64_t jit_f64_min(uint64_t a, uint64_t b);
uint64_t jit_f64_max(uint64_t a, uint64_t b);
uint64_t jit_f64_sqrt(uint64_t a);
uint64_t jit_f64_ceil(uint64_t a);
uint64_t jit_f64_floor(uint64_t a);
uint64_t jit_f64_trunc(uint64_t a);
uint64_t jit_f64_nearest(uint64_t a);
uint32_t jit_f64_eq(uint64_t a, uint64_t b);
uint32_t jit_f64_ne(uint64_t a, uint64_t b);
uint32_t jit_f64_lt(uint64_t a, uint64_t b);
uint32_t jit_f64_gt(uint64_t a, uint64_t b);
uint32_t jit_f64_le(uint64_t a, uint64_t b);
uint32_t jit_f64_ge(uint64_t a, uint64_t b);

uint32_t jit_i32_trunc_f64_s(WASMModuleInstance *inst, uint64_t a);
uint32_t jit_i32_trunc_f64_u(WASMModuleInstance *inst, uint64_t a);
uint64_t jit_i64_trunc_f32_s(WASMModuleInstance *inst, uint32_t a);
uint64_t jit_i64_trunc_f32_u(WASMModuleInstance *inst, uint32_t a);
uint64_t jit_i64_trunc_f64_s(WASMModuleInstance *inst, uint64_t a);
uint64_t jit_i64_trunc_f64_u(WASMModuleInstance *inst, uint64_t a);
uint32_t jit_i32_trunc_sat_f64_s(uint64_t a);
uint32_t jit_i32_trunc_sat_f64_u(uint64_t a);
uint64_t jit_i64_trunc_sat_f32_s(uint32_t a);
uint64_t jit_i64_trunc_sat_f32_u(uint32_t a);
uint64_t jit_i64_trunc_sat_f64_s(uint64_t a);
uint64_t jit_i64_trunc_sat_f64_u(uint64_t a);
uint32_t jit_f32_convert_i64_s(uint64_t a);
uint32_t jit_f32_convert_i64_u(uint64_t a);
uint32_t jit_f32_demote_f64(uint64_t a);
uint64_t jit_f64_convert_i32_s(uint32_t a);
uint64_t jit_f64_convert_i32_u(uint32_t a);
uint64_t jit_f64_convert_i64_s(uint64_t a);
uint64_t jit_f64_convert_i64_u(uint64_t a);
uint64_t jit_f64_promote_f32(uint32_t a);

#endif /* __APPS_GRAPHICS_FPCWASM_JIT_JIT_INTERNAL_H */
