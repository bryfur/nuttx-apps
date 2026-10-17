/****************************************************************************
 * apps/testing/strbench/strbench_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* The firmware's memory and string functions (what every module and the
 * kernel call: docs/benchmarks.md "memset and friends"):
 *
 *   strbench [check]        each against a byte-at-a-time reference
 *                           (strcheck.c: lengths, alignments, overlaps)
 *   strbench bench [-p]     cycles a call at 1, 4, 16, 64, 512 and 4096
 *                           bytes, aligned and misaligned, the buffers in
 *                           the SRAM (-p: in the PSRAM, behind the XIP
 *                           cache); the best of 32 calls, less an empty
 *                           function's call
 *
 * Exit status 0 if every check passed.
 */

#include <nuttx/config.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fpc_place.h>

#include "strcheck.h"
#include "strbench.h"

#define DWT_CTRL  (*(volatile uint32_t *)0xe0001000)
#define DWT_CYC   (*(volatile uint32_t *)0xe0001004)

#define FAST      FPC_SRAM_CODE("strb") __attribute__((noinline))

/* The SRAM buffers (the module loader puts them there: fpc_place.h) */

#define SRAM      FPC_SRAM_BSS("strb")

#define BUFSZ     (4096 + 64)
#define REPS      32

typedef uintptr_t (*fn3_t)(uintptr_t a, uintptr_t b, uintptr_t c);

static unsigned char g_sa[BUFSZ] SRAM __attribute__((aligned(16)));
static unsigned char g_sb[BUFSZ] SRAM __attribute__((aligned(16)));

static const struct strbench_impl g_firmware =
{
  "firmware",
  {
    memset, memcpy, memmove, memchr, strlen, strcmp, strcpy
  }
};

#ifdef STRBENCH_VARIANTS
extern const struct strbench_impl g_strbench_variants[];
extern const int g_strbench_nvariants;
#endif

static const size_t g_sizes[] =
{
  1, 4, 16, 64, 512, 4096
};

#define NSIZES (sizeof(g_sizes) / sizeof(g_sizes[0]))

FAST static uintptr_t empty(uintptr_t a, uintptr_t b, uintptr_t c)
{
  __asm__ volatile("" : : : "memory");
  return a;
}

/* The best of REPS calls of F(A, B, C), in cycles */

FAST static uint32_t timeit(fn3_t f, uintptr_t a, uintptr_t b, uintptr_t c)
{
  uint32_t best = UINT32_MAX;
  int i;

  for (i = 0; i < REPS; i++)
    {
      uint32_t t0 = DWT_CYC;
      uint32_t t;

      f(a, b, c);
      t = DWT_CYC - t0;
      if (t < best)
        {
          best = t;
        }
    }

  return best;
}

enum
{
  B_MEMSET, B_MEMCPY, B_MEMMOVE, B_MOVEBACK, B_STRLEN, B_STRCMP, B_STRCPY,
  B_MEMCHR, B_N
};

static const char *g_bname[B_N] =
{
  "memset", "memcpy", "memmove", "memmove<-", "strlen", "strcmp",
  "strcpy", "memchr"
};

/* One case: function B of IMPL at size N, misaligned or not, on the
 * buffers A and B (each BUFSZ)
 */

static uint32_t bench_one(const struct strcheck_fns *f, int b, size_t n,
                          int mis, unsigned char *ba, unsigned char *bb,
                          uint32_t base)
{
  unsigned char *d = ba + (mis ? 1 : 0);
  unsigned char *s = bb + (mis ? 3 : 0);
  uint32_t t = 0;
  size_t i;

  switch (b)
    {
      case B_MEMSET:
        t = timeit((fn3_t)f->memset, (uintptr_t)d, 0, n);
        break;

      case B_MEMCPY:
        t = timeit((fn3_t)f->memcpy, (uintptr_t)d, (uintptr_t)s, n);
        break;

      case B_MEMMOVE:
        t = timeit((fn3_t)f->memmove, (uintptr_t)d, (uintptr_t)s, n);
        break;

      case B_MOVEBACK:

        /* Overlapping, the destination after the source: copied from
         * the end
         */

        t = timeit((fn3_t)f->memmove, (uintptr_t)(ba + 8 + (mis ? 1 : 0)),
                   (uintptr_t)ba, n);
        break;

      case B_STRLEN:
      case B_STRCPY:
      case B_STRCMP:
        for (i = 0; i < n; i++)
          {
            s[i] = 'a' + (i % 23);
          }

        s[n - 1] = 0;           /* n bytes with the NUL */
        if (b == B_STRLEN)
          {
            t = timeit((fn3_t)f->strlen, (uintptr_t)s, 0, 0);
          }
        else if (b == B_STRCPY)
          {
            t = timeit((fn3_t)f->strcpy, (uintptr_t)d, (uintptr_t)s, 0);
          }
        else
          {
            for (i = 0; i < n; i++)
              {
                d[i] = s[i];
              }

            t = timeit((fn3_t)f->strcmp, (uintptr_t)d, (uintptr_t)s, 0);
          }
        break;

      case B_MEMCHR:
        for (i = 0; i < n; i++)
          {
            s[i] = 'a' + (i % 23);
          }

        t = timeit((fn3_t)f->memchr, (uintptr_t)s, 0, n);
        break;
    }

  return t > base ? t - base : 0;
}

static void bench(const struct strbench_impl *impl, int nimpl,
                  unsigned char *ba, unsigned char *bb, const char *where)
{
  uint32_t base;
  int b;
  int mis;
  int k;
  size_t i;

  DWT_CTRL |= 1;
  base = timeit(empty, 0, 0, 0);

  printf("cycles a call, buffers in the %s (best of %d, less %lu for an "
         "empty call); misaligned: dest+1, src+3\n", where, REPS,
         (unsigned long)base);
  printf("%-9s %-9s %-4s", "function", "impl", "mis");
  for (i = 0; i < NSIZES; i++)
    {
      printf(" %7u", (unsigned)g_sizes[i]);
    }

  printf("\n");

  for (b = 0; b < B_N; b++)
    {
      for (mis = 0; mis < 2; mis++)
        {
          for (k = 0; k < nimpl; k++)
            {
              const struct strcheck_fns *f = &impl[k].fns;
              int have;

              switch (b)
                {
                  case B_MEMSET:   have = f->memset != NULL; break;
                  case B_MEMCPY:   have = f->memcpy != NULL; break;
                  case B_MEMMOVE:
                  case B_MOVEBACK: have = f->memmove != NULL; break;
                  case B_STRLEN:   have = f->strlen != NULL; break;
                  case B_STRCMP:   have = f->strcmp != NULL; break;
                  case B_STRCPY:   have = f->strcpy != NULL; break;
                  default:         have = f->memchr != NULL; break;
                }

              if (!have)
                {
                  continue;
                }

              printf("%-9s %-9s %-4s", g_bname[b], impl[k].name,
                     mis ? "yes" : "no");
              for (i = 0; i < NSIZES; i++)
                {
                  printf(" %7lu", (unsigned long)
                         bench_one(f, b, g_sizes[i], mis, ba, bb, base));
                }

              printf("\n");
            }
        }
    }
}

int main(int argc, char *argv[])
{
  const struct strbench_impl *impl = &g_firmware;
  int nimpl = 1;
  int psram = 0;
  int all = 0;
  int i;

  for (i = 2; i < argc; i++)
    {
      if (strcmp(argv[i], "-p") == 0)
        {
          psram = 1;
        }
      else if (strcmp(argv[i], "-a") == 0)
        {
          all = 1;
        }
    }

#ifdef STRBENCH_VARIANTS
  if (all)
    {
      impl = g_strbench_variants;
      nimpl = g_strbench_nvariants;
    }
#else
  (void)all;
#endif

  if (argc > 1 && strcmp(argv[1], "bench") == 0)
    {
      unsigned char *ba = g_sa;
      unsigned char *bb = g_sb;

      if (psram)
        {
          ba = malloc(BUFSZ);
          bb = malloc(BUFSZ);
          if (ba == NULL || bb == NULL)
            {
              printf("no memory\n");
              return 1;
            }
        }

      bench(impl, nimpl, ba, bb, psram ? "PSRAM" : "SRAM");
      if (psram)
        {
          free(ba);
          free(bb);
        }

      return 0;
    }

  if (argc > 1 && strcmp(argv[1], "check") != 0)
    {
      printf("usage: strbench [check] | bench [-p]\n");
      return 1;
    }

  for (i = 0; i < nimpl; i++)
    {
      int n;

      printf("%s:\n", impl[i].name);
      n = strcheck(&impl[i].fns);
      if (n != 0)
        {
          printf("strbench: %d FAILED\n", n);
          return 1;
        }
    }

  printf("strbench: ok\n");
  return 0;
}
