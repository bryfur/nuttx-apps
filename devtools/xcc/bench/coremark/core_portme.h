/* apps/devtools/xcc/bench/coremark/core_portme.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CoreMark's port for comparing compilers (docs/xcc.md): the same for
 * every compiler and target (WASI, NuttX, Linux), timed with
 * clock_gettime() in microseconds, ITERATIONS fixed at build time.
 */

#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stddef.h>

#define HAS_FLOAT 1
#define HAS_TIME_H 0
#define USE_CLOCK 0
#define HAS_STDIO 1
#define HAS_PRINTF 1
#define MAIN_HAS_NOARGC 0
#define MAIN_HAS_NORETURN 0
#define MULTITHREAD 1
#define USE_PTHREAD 0
#define USE_FORK 0
#define USE_SOCKET 0
#define SEED_METHOD SEED_VOLATILE
#define MEM_METHOD MEM_STATIC
#define MEM_LOCATION "STATIC"

#ifndef COMPILER_VERSION
#  if defined(__XCC)
#    define COMPILER_VERSION "xcc"
#  elif defined(__clang__)
#    define COMPILER_VERSION "clang " __clang_version__
#  elif defined(__GNUC__)
#    define COMPILER_VERSION "GCC " __VERSION__
#  else
#    define COMPILER_VERSION "?"
#  endif
#endif
#ifndef COMPILER_FLAGS
#  define COMPILER_FLAGS "-"
#endif

typedef unsigned long CORE_TICKS;
typedef signed short ee_s16;
typedef unsigned short ee_u16;
typedef signed int ee_s32;
typedef float ee_f32;
typedef unsigned char ee_u8;
typedef unsigned int ee_u32;
typedef unsigned long ee_ptr_int;
typedef size_t ee_size_t;

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

typedef struct CORE_PORTABLE_S
{
  ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) && !defined(VALIDATION_RUN)
#  if (TOTAL_DATA_SIZE == 1200)
#    define PROFILE_RUN 1
#  elif (TOTAL_DATA_SIZE == 2000)
#    define PERFORMANCE_RUN 1
#  else
#    define VALIDATION_RUN 1
#  endif
#endif

extern ee_u32 default_num_contexts;

#endif
