/* apps/devtools/xcc/bench/coremark/core_portme.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * CoreMark's port for comparing compilers (core_portme.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "coremark.h"

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

static unsigned long g_start;
static unsigned long g_stop;
ee_u32 default_num_contexts = 1;

static unsigned long now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void start_time(void)
{
  g_start = now_us();
}

void stop_time(void)
{
  g_stop = now_us();
}

CORE_TICKS get_time(void)
{
  return g_stop - g_start;
}

secs_ret time_in_secs(CORE_TICKS ticks)
{
  return (secs_ret)ticks / 1000000;
}

void portable_init(core_portable *p, int *argc, char *argv[])
{
  p->portable_id = 1;
}

void portable_fini(core_portable *p)
{
  p->portable_id = 0;
}
