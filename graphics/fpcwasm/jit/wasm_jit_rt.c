/****************************************************************************
 * apps/graphics/fpcwasm/jit/wasm_jit_rt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the JIT's code calls (wasm_jit.c): traps, the generic call of a
 * host function, memory and table instructions, and the arithmetic done
 * in C (i64 division and variable shifts, everything f64: the Cortex-M33
 * has no double precision FPU, so these are the firmware's soft-float).
 * Values travel as bit patterns in core registers.  The semantics are the
 * interpreter's (wasm_interp_fast.c), message for message.
 ****************************************************************************/

#include "jit_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

const char *const jit_trap_msg[JT_COUNT] =
{
  "unreachable",
  "out of bounds memory access",
  "integer divide by zero",
  "integer overflow",
  "invalid conversion to integer",
  "undefined element",
  "uninitialized element",
  "indirect call type mismatch",
  "native stack overflow",
  "wasm auxiliary stack overflow",
  "wasm auxiliary stack underflow",
  "unknown function",
  "out of bounds table access",
};

/****************************************************************************
 * Traps
 ****************************************************************************/

void jit_raise(WASMModuleInstance *inst, uint32_t code)
{
  wasm_set_exception(inst, jit_trap_msg[code]);
}

/* Back to the entry (the exception set) */

static void unwind(WASMModuleInstance *inst)
{
  ((jit_module_t *)inst->e->ext_data)->unwind(inst);
  __builtin_unreachable();
}

void jit_trap(WASMModuleInstance *inst, uint32_t code)
{
  jit_raise(inst, code);
  unwind(inst);
}

/****************************************************************************
 * Host functions (the ones not called directly)
 ****************************************************************************/

void jit_call_import(WASMModuleInstance *inst, uint32_t idx, uint32_t *argv)
{
  WASMExecEnv *env = (WASMExecEnv *)(uintptr_t)inst->ext_words[EXT_ENV];
  WASMFunctionInstance *f = inst->e->functions + idx;
  WASMFunctionImport *imp = f->u.func_import;
  CApiFuncImport *capi = NULL;
  void *fp = NULL;
  uint32_t ret[2];
  bool ok;

  if (inst->ext_words[EXT_LIMIT] != 0 && (uintptr_t)ret <
      inst->ext_words[EXT_LIMIT] + JIT_STACK_HOST - JIT_STACK_MARGIN)
    {
      jit_trap(inst, JT_STACK);
    }

  if (!imp->call_conv_wasm_c_api)
    {
      fp = inst->import_func_ptrs[idx];
    }
  else if (inst->c_api_func_imports != NULL)
    {
      capi = inst->c_api_func_imports + idx;
      fp = capi->func_ptr_linked;
    }

  if (fp == NULL)
    {
      char buf[128];

      snprintf(buf, sizeof(buf),
               "failed to call unlinked import function (%s, %s)",
               imp->module_name, imp->field_name);
      wasm_set_exception(inst, buf);
      unwind(inst);
    }

  ret[0] = ret[1] = 0;
  if (imp->call_conv_wasm_c_api)
    {
      ok = wasm_runtime_invoke_c_api_native((WASMModuleInstanceCommon *)inst,
                                            fp, imp->func_type,
                                            f->param_cell_num, argv,
                                            capi->with_env_arg,
                                            capi->env_arg);
      ret[0] = argv[0];
      ret[1] = argv[1];
    }
  else if (!imp->call_conv_raw)
    {
      ok = wasm_runtime_invoke_native(env, fp, imp->func_type,
                                      imp->signature, imp->attachment, argv,
                                      f->param_cell_num, ret);
    }
  else
    {
      ok = wasm_runtime_invoke_native_raw(env, fp, imp->func_type,
                                          imp->signature, imp->attachment,
                                          argv, f->param_cell_num, ret);
    }

  if (!ok || wasm_copy_exception(inst, NULL))
    {
      unwind(inst);
    }

  argv[0] = ret[0];
  argv[1] = ret[1];
  jit_set_lim4(inst);           /* (the host may have grown the memory) */
}

/****************************************************************************
 * Memory
 ****************************************************************************/

/* The limit the code checks 4-byte accesses against (r9) */

void jit_set_lim4(WASMModuleInstance *inst)
{
  uint64_t size = inst->memory_count > 0 ?
                  inst->memories[0]->memory_data_size : 0;

  if (size > 0xffffffffu)
    {
      size = 0xffffffffu;
    }

  jit_module_t *jm = inst->e->ext_data;
  uint32_t g = jm != NULL && jm->guard != 0 ? jm->guard : 3;

  inst->ext_words[EXT_LIM4] = size > g ? (uint32_t)size - g : 0;
}

static uint8_t *mem_check(WASMModuleInstance *inst, uint32_t addr,
                          uint32_t len)
{
  WASMMemoryInstance *m = inst->memories[0];

  if ((uint64_t)addr + len > m->memory_data_size)
    {
      jit_trap(inst, JT_OOB);
    }

  return m->memory_data + addr;
}

uint32_t jit_mem_grow(WASMModuleInstance *inst, uint32_t delta)
{
  uint32_t prev = inst->memories[0]->cur_page_count;

  if (!wasm_enlarge_memory(inst, delta))
    {
      return 0xffffffffu;
    }

  jit_set_lim4(inst);
  return prev;
}

void jit_mem_init(WASMModuleInstance *inst, uint32_t seg, uint32_t dst,
                  uint32_t src, uint32_t len)
{
  uint8_t *d = mem_check(inst, dst, len);
  uint64_t seg_len = 0;
  uint8_t *data = NULL;

  if (!bh_bitmap_get_bit(inst->e->common.data_dropped, seg))
    {
      seg_len = inst->module->data_segments[seg]->data_length;
      data = inst->module->data_segments[seg]->data;
    }

  if ((uint64_t)src + len > seg_len)
    {
      jit_trap(inst, JT_OOB);
    }

  if (len > 0)
    {
      memcpy(d, data + src, len);
    }
}

void jit_data_drop(WASMModuleInstance *inst, uint32_t seg)
{
  bh_bitmap_set_bit(inst->e->common.data_dropped, seg);
}

void jit_mem_copy(WASMModuleInstance *inst, uint32_t dst, uint32_t src,
                  uint32_t len)
{
  uint8_t *s = mem_check(inst, src, len);
  uint8_t *d = mem_check(inst, dst, len);

  if (len > 0 && d != s)
    {
      memmove(d, s, len);
    }
}

void jit_mem_fill(WASMModuleInstance *inst, uint32_t dst, uint32_t val,
                  uint32_t len)
{
  uint8_t *d = mem_check(inst, dst, len);

  memset(d, (int)(uint8_t)val, len);
}

/****************************************************************************
 * Tables
 ****************************************************************************/

void jit_table_init(WASMModuleInstance *inst, uint32_t segtbl, uint32_t dst,
                    uint32_t src, uint32_t len)
{
  uint32_t seg = segtbl >> 16;
  WASMTableInstance *t = wasm_get_table_inst(inst, segtbl & 0xffff);
  InitializerExpression *init = NULL;
  uint32_t seg_len = 0;
  uint32_t i;

  if (!bh_bitmap_get_bit(inst->e->common.elem_dropped, seg))
    {
      init = inst->module->table_segments[seg].init_values;
      seg_len = inst->module->table_segments[seg].value_count;
    }

  if (offset_len_out_of_bounds(src, len, seg_len) ||
      offset_len_out_of_bounds(dst, len, t->cur_size))
    {
      jit_trap(inst, JT_TABLE_OOB);
    }

  for (i = 0; i < len; i++)
    {
      t->elems[dst + i] = (table_elem_type_t)init[src + i].u.unary.v.ref_index;
    }
}

void jit_elem_drop(WASMModuleInstance *inst, uint32_t seg)
{
  bh_bitmap_set_bit(inst->e->common.elem_dropped, seg);
}

void jit_table_copy(WASMModuleInstance *inst, uint32_t tbls, uint32_t dst,
                    uint32_t src, uint32_t len)
{
  WASMTableInstance *d = wasm_get_table_inst(inst, tbls >> 16);
  WASMTableInstance *s = wasm_get_table_inst(inst, tbls & 0xffff);

  if (offset_len_out_of_bounds(dst, len, d->cur_size) ||
      offset_len_out_of_bounds(src, len, s->cur_size))
    {
      jit_trap(inst, JT_TABLE_OOB);
    }

  memmove(d->elems + dst, s->elems + src, len * sizeof(table_elem_type_t));
}

uint32_t jit_table_grow(WASMModuleInstance *inst, uint32_t tbl,
                        uint32_t init, uint32_t n)
{
  uint32_t orig = wasm_get_table_inst(inst, tbl)->cur_size;

  if (!wasm_enlarge_table(inst, tbl, n, (table_elem_type_t)init))
    {
      return 0xffffffffu;
    }

  return orig;
}

void jit_table_fill(WASMModuleInstance *inst, uint32_t tbl, uint32_t i,
                    uint32_t val, uint32_t n)
{
  WASMTableInstance *t = wasm_get_table_inst(inst, tbl);

  if (offset_len_out_of_bounds(i, n, t->cur_size))
    {
      jit_trap(inst, JT_TABLE_OOB);
    }

  for (; n != 0; i++, n--)
    {
      t->elems[i] = (table_elem_type_t)val;
    }
}

uint32_t jit_table_get(WASMModuleInstance *inst, uint32_t tbl, uint32_t i)
{
  WASMTableInstance *t = wasm_get_table_inst(inst, tbl);

  if (i >= t->cur_size)
    {
      jit_trap(inst, JT_TABLE_OOB);
    }

  return (uint32_t)t->elems[i];
}

void jit_table_set(WASMModuleInstance *inst, uint32_t tbl, uint32_t i,
                   uint32_t val)
{
  WASMTableInstance *t = wasm_get_table_inst(inst, tbl);

  if (i >= t->cur_size)
    {
      jit_trap(inst, JT_TABLE_OOB);
    }

  t->elems[i] = (table_elem_type_t)val;
}

/****************************************************************************
 * Integers
 ****************************************************************************/

uint32_t jit_i32_popcnt(uint32_t a)
{
  return (uint32_t)__builtin_popcount(a);
}

uint64_t jit_i64_div_s(uint64_t a, uint64_t b)
{
  return (uint64_t)((int64_t)a / (int64_t)b);
}

uint64_t jit_i64_div_u(uint64_t a, uint64_t b)
{
  return a / b;
}

uint64_t jit_i64_rem_s(uint64_t a, uint64_t b)
{
  if ((int64_t)b == -1)
    {
      return 0;
    }

  return (uint64_t)((int64_t)a % (int64_t)b);
}

uint64_t jit_i64_rem_u(uint64_t a, uint64_t b)
{
  return a % b;
}

uint64_t jit_i64_shl(uint64_t a, uint32_t n)
{
  return a << (n & 63);
}

uint64_t jit_i64_shr_s(uint64_t a, uint32_t n)
{
  return (uint64_t)((int64_t)a >> (n & 63));
}

uint64_t jit_i64_shr_u(uint64_t a, uint32_t n)
{
  return a >> (n & 63);
}

uint64_t jit_i64_rotl(uint64_t a, uint32_t n)
{
  n &= 63;
  return n ? (a << n) | (a >> (64 - n)) : a;
}

uint64_t jit_i64_rotr(uint64_t a, uint32_t n)
{
  n &= 63;
  return n ? (a >> n) | (a << (64 - n)) : a;
}

uint64_t jit_i64_ctz(uint64_t a)
{
  return a ? (uint64_t)__builtin_ctzll(a) : 64;
}

uint64_t jit_i64_popcnt(uint64_t a)
{
  return (uint64_t)__builtin_popcountll(a);
}

/****************************************************************************
 * Floating point (bit patterns)
 ****************************************************************************/

static inline double d_(uint64_t x)
{
  double d;

  memcpy(&d, &x, 8);
  return d;
}

static inline uint64_t u_(double d)
{
  uint64_t x;

  memcpy(&x, &d, 8);
  return x;
}

static inline float f_(uint32_t x)
{
  float f;

  memcpy(&f, &x, 4);
  return f;
}

static inline uint32_t w_(float f)
{
  uint32_t x;

  memcpy(&x, &f, 4);
  return x;
}





uint64_t jit_f64_min(uint64_t a, uint64_t b)
{
  double x = d_(a);
  double y = d_(b);

  if (isnan(x) || isnan(y))
    {
      return u_(x + y);
    }

  if (x == 0 && y == 0)
    {
      return signbit(x) ? a : b;
    }

  return x < y ? a : b;
}

uint64_t jit_f64_max(uint64_t a, uint64_t b)
{
  double x = d_(a);
  double y = d_(b);

  if (isnan(x) || isnan(y))
    {
      return u_(x + y);
    }

  if (x == 0 && y == 0)
    {
      return signbit(x) ? b : a;
    }

  return x > y ? a : b;
}

uint64_t jit_f64_sqrt(uint64_t a)
{
  return u_(sqrt(d_(a)));
}

uint64_t jit_f64_ceil(uint64_t a)
{
  return u_(ceil(d_(a)));
}

uint64_t jit_f64_floor(uint64_t a)
{
  return u_(floor(d_(a)));
}

uint64_t jit_f64_trunc(uint64_t a)
{
  return u_(trunc(d_(a)));
}

uint64_t jit_f64_nearest(uint64_t a)
{
  return u_(rint(d_(a)));
}


uint32_t jit_f64_ne(uint64_t a, uint64_t b)
{
  return d_(a) != d_(b);
}





/* Truncations that trap */

uint32_t jit_i32_trunc_f64_s(WASMModuleInstance *inst, uint64_t a)
{
  double x = d_(a);

  if (isnan(x))
    {
      jit_trap(inst, JT_CONV);
    }

  if (!(x > -2147483649.0 && x < 2147483648.0))
    {
      jit_trap(inst, JT_OVERFLOW);
    }

  return (uint32_t)(int32_t)x;
}

uint32_t jit_i32_trunc_f64_u(WASMModuleInstance *inst, uint64_t a)
{
  double x = d_(a);

  if (isnan(x))
    {
      jit_trap(inst, JT_CONV);
    }

  if (!(x > -1.0 && x < 4294967296.0))
    {
      jit_trap(inst, JT_OVERFLOW);
    }

  return (uint32_t)x;
}

uint64_t jit_i64_trunc_f32_s(WASMModuleInstance *inst, uint32_t a)
{
  float x = f_(a);

  if (isnan(x))
    {
      jit_trap(inst, JT_CONV);
    }

  if (!(x >= -9223372036854775808.0f && x < 9223372036854775808.0f))
    {
      jit_trap(inst, JT_OVERFLOW);
    }

  return (uint64_t)(int64_t)x;
}

uint64_t jit_i64_trunc_f32_u(WASMModuleInstance *inst, uint32_t a)
{
  float x = f_(a);

  if (isnan(x))
    {
      jit_trap(inst, JT_CONV);
    }

  if (!(x > -1.0f && x < 18446744073709551616.0f))
    {
      jit_trap(inst, JT_OVERFLOW);
    }

  return (uint64_t)x;
}

uint64_t jit_i64_trunc_f64_s(WASMModuleInstance *inst, uint64_t a)
{
  double x = d_(a);

  if (isnan(x))
    {
      jit_trap(inst, JT_CONV);
    }

  if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0))
    {
      jit_trap(inst, JT_OVERFLOW);
    }

  return (uint64_t)(int64_t)x;
}

uint64_t jit_i64_trunc_f64_u(WASMModuleInstance *inst, uint64_t a)
{
  double x = d_(a);

  if (isnan(x))
    {
      jit_trap(inst, JT_CONV);
    }

  if (!(x > -1.0 && x < 18446744073709551616.0))
    {
      jit_trap(inst, JT_OVERFLOW);
    }

  return (uint64_t)x;
}

/* Saturating truncations */

uint32_t jit_i32_trunc_sat_f64_s(uint64_t a)
{
  double x = d_(a);

  if (isnan(x))
    {
      return 0;
    }

  if (x <= -2147483648.0)
    {
      return 0x80000000u;
    }

  if (x >= 2147483647.0)
    {
      return 0x7fffffffu;
    }

  return (uint32_t)(int32_t)x;
}

uint32_t jit_i32_trunc_sat_f64_u(uint64_t a)
{
  double x = d_(a);

  if (isnan(x) || x <= 0)
    {
      return 0;
    }

  if (x >= 4294967295.0)
    {
      return 0xffffffffu;
    }

  return (uint32_t)x;
}

uint64_t jit_i64_trunc_sat_f32_s(uint32_t a)
{
  float x = f_(a);

  if (isnan(x))
    {
      return 0;
    }

  if (x <= -9223372036854775808.0f)
    {
      return 0x8000000000000000ull;
    }

  if (x >= 9223372036854775808.0f)
    {
      return 0x7fffffffffffffffull;
    }

  return (uint64_t)(int64_t)x;
}

uint64_t jit_i64_trunc_sat_f32_u(uint32_t a)
{
  float x = f_(a);

  if (isnan(x) || x <= 0)
    {
      return 0;
    }

  if (x >= 18446744073709551616.0f)
    {
      return 0xffffffffffffffffull;
    }

  return (uint64_t)x;
}

uint64_t jit_i64_trunc_sat_f64_s(uint64_t a)
{
  double x = d_(a);

  if (isnan(x))
    {
      return 0;
    }

  if (x <= -9223372036854775808.0)
    {
      return 0x8000000000000000ull;
    }

  if (x >= 9223372036854775808.0)
    {
      return 0x7fffffffffffffffull;
    }

  return (uint64_t)(int64_t)x;
}

uint64_t jit_i64_trunc_sat_f64_u(uint64_t a)
{
  double x = d_(a);

  if (isnan(x) || x <= 0)
    {
      return 0;
    }

  if (x >= 18446744073709551616.0)
    {
      return 0xffffffffffffffffull;
    }

  return (uint64_t)x;
}

/* Conversions */








