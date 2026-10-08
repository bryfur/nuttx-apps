/****************************************************************************
 * apps/testing/mm/psramtest/psramtest.c
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

/* A test of the RP2350's QSPI PSRAM, all of it (rp23xx_psram.c; the fpc
 * tree's docs/psram.md): what was detected, the data and address lines,
 * address-in-address, random patterns, byte/halfword/word and unaligned
 * accesses, bursts across pages and dies, retention, and speed.
 *
 * The whole PSRAM is overwritten.  Anything the firmware keeps at its
 * start (.psram_data/.psram_bss up to _epsram_bss: the built-in
 * applications' variables, NSH's among them) is saved in the SRAM and put
 * back at the end, so the firmware must not have the PSRAM in a heap
 * (RP23XX_PSRAM_HEAP_NONE) and this program keeps no variables of its own
 * (no static data: it would be in the PSRAM too).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arch/chip/psram.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The XIP cache (RP2350 datasheet 4.4): 16 KB, 8-byte lines, shared by the
 * flash and the PSRAM; maintenance by set/way through the top of the
 * maintenance window (the Pico SDK's xip_cache.c, RP2350-E11)
 */

#define XIP_CACHE_SIZE      0x4000
#define XIP_CACHE_LINE      8
#define XIP_MAINT_SETWAY    (0x18000000 + 0x04000000 - XIP_CACHE_SIZE)
#define XIP_OP_INVALIDATE   0
#define XIP_OP_CLEAN        1

#define QMI_M1_TIMING       0x400d0020

#define MB                  (1024 * 1024)
#define SHADOW              (64 * 1024)   /* mix: a window's SRAM copy */
#define BUFSZ               (32 * 1024)   /* burst: SRAM buffers */
#define MAXSHOW             8             /* errors printed per test */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct pt_s
{
  struct rp23xx_psram_info_s info;
  uint32_t size;
  uintptr_t c;               /* The PSRAM through the cache */
  uintptr_t u;               /* ... and not */
  unsigned long errors;      /* All tests */
  unsigned long terr;        /* This test */
  uint8_t *save;             /* The firmware's own data at the start */
  size_t savelen;
  uint8_t *a;                /* SRAM buffers */
  uint8_t *b;
};

struct __attribute__((packed)) u16p_s
{
  uint16_t v;
};

struct __attribute__((packed)) u32p_s
{
  uint32_t v;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

extern uint8_t _epsram_bss[] __attribute__((weak));

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void xip_op(int op)
{
  volatile uint8_t *m = (volatile uint8_t *)XIP_MAINT_SETWAY;
  uint32_t off;

  for (off = 0; off < XIP_CACHE_SIZE; off += XIP_CACHE_LINE)
    {
      m[off + op] = 0;
    }

  __asm__ __volatile__("dsb\n\tisb" ::: "memory");
}

/* Dirty lines to the PSRAM, then nothing of it cached */

static void xip_flush(void)
{
  xip_op(XIP_OP_CLEAN);
  xip_op(XIP_OP_INVALIDATE);
}

static uint64_t now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static inline uint32_t xs32(uint32_t *s)
{
  uint32_t x = *s;

  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *s = x;
  return x;
}

static void bad(struct pt_s *pt, uintptr_t addr, uint32_t exp, uint32_t got)
{
  pt->terr++;
  pt->errors++;
  if (pt->terr <= MAXSHOW)
    {
      printf("    ERROR at %08" PRIxPTR ": expected %08" PRIx32
             " got %08" PRIx32 " (xor %08" PRIx32 ")\n",
             addr, exp, got, exp ^ got);
    }
}

static void begin(struct pt_s *pt, const char *name)
{
  pt->terr = 0;
  printf("  %-44s ", name);
  fflush(stdout);
}

static void end(struct pt_s *pt, uint64_t t0)
{
  unsigned long ms = (unsigned long)((now_us() - t0) / 1000);

  if (pt->terr == 0)
    {
      printf("PASS (%lu ms)\n", ms);
    }
  else
    {
      printf("\n  -> FAIL: %lu errors (%lu ms)\n", pt->terr, ms);
    }
}

/* Word fill and check over the whole PSRAM through base (cached or not):
 * kind 0 the address, 1 its complement, 2 random from seed
 */

static uint32_t val(uint32_t kind, uintptr_t a, uint32_t *s)
{
  return kind == 0 ? (uint32_t)a : kind == 1 ? ~(uint32_t)a : xs32(s);
}

static void fill(struct pt_s *pt, uintptr_t base, uint32_t kind,
                 uint32_t seed)
{
  volatile uint32_t *p = (volatile uint32_t *)base;
  uint32_t n = pt->size / 4;
  uint32_t i;

  for (i = 0; i < n; i++)
    {
      p[i] = val(kind, pt->c + i * 4, &seed);
    }
}

static void check(struct pt_s *pt, uintptr_t base, uint32_t kind,
                  uint32_t seed)
{
  volatile uint32_t *p = (volatile uint32_t *)base;
  uint32_t n = pt->size / 4;
  uint32_t i;

  for (i = 0; i < n; i++)
    {
      uint32_t e = val(kind, pt->c + i * 4, &seed);
      uint32_t g = p[i];

      if (g != e)
        {
          bad(pt, base + i * 4, e, g);
        }
    }
}

/****************************************************************************
 * Name: t_info
 ****************************************************************************/

static void t_info(struct pt_s *pt)
{
  struct rp23xx_psram_info_s *in = &pt->info;
  uint32_t t = *(volatile uint32_t *)QMI_M1_TIMING;
  int i;

  printf("PSRAM: %s\n", in->name ? in->name : "(none)");
  printf("  Read ID (9Fh):");
  for (i = 0; i < 8; i++)
    {
      printf(" %02x", in->id[i]);
    }

  printf("  (MF ID %02x, KGD %02x, EID %02x: density bits %u)\n",
         in->id[0], in->id[1], in->id[2], in->id[2] >> 5);
  printf("  size: ID %" PRIu32 " KB, probe %" PRIu32 " KB, in use %"
         PRIu32 " KB\n", in->id_size / 1024, in->probe_size / 1024,
         in->size / 1024);
  printf("  M1_TIMING %08" PRIx32 ": CLKDIV %" PRIu32 " RXDELAY %" PRIu32
         " MIN_DESELECT %" PRIu32 " MAX_SELECT %" PRIu32
         " SELECT_HOLD %" PRIu32 " SELECT_SETUP %" PRIu32
         " PAGEBREAK %" PRIu32 " COOLDOWN %" PRIu32 "\n",
         t, t & 0xff, (t >> 8) & 7, (t >> 12) & 0x1f, (t >> 17) & 0x3f,
         (t >> 23) & 3, (t >> 25) & 1, (t >> 28) & 3, t >> 30);
  printf("  M1 RFMT %08" PRIx32 " RCMD %08" PRIx32 " WFMT %08" PRIx32
         " WCMD %08" PRIx32 "\n",
         *(volatile uint32_t *)0x400d0024, *(volatile uint32_t *)0x400d0028,
         *(volatile uint32_t *)0x400d002c,
         *(volatile uint32_t *)0x400d0030);
  printf("  firmware data at the start: %zu bytes (saved, put back)\n",
         pt->savelen);
}

/****************************************************************************
 * Name: t_bus
 *
 * Walking ones and zeros on the data lines (with the complement written
 * next to it, so that a floating bus cannot read back the last value), then
 * each address line on its own (a word at each power of two).
 *
 ****************************************************************************/

static void t_bus(struct pt_s *pt)
{
  static const uint32_t where[] =
  {
    0, 4 * MB + 4, 8 * MB - 8, 8 * MB, 12 * MB + 1028, 16 * MB - 8
  };

  uint64_t t0 = now_us();
  uint32_t w;
  uint32_t b;
  uint32_t k;

  begin(pt, "data lines: walking 1s/0s");
  for (w = 0; w < sizeof(where) / sizeof(where[0]); w++)
    {
      volatile uint32_t *p;

      if (where[w] + 8 > pt->size)
        {
          continue;
        }

      p = (volatile uint32_t *)(pt->u + where[w]);
      for (b = 0; b < 64; b++)
        {
          uint32_t v = b < 32 ? 1u << b : ~(1u << (b - 32));
          uint32_t g;

          p[0] = v;
          p[1] = ~v;
          g = p[0];
          if (g != v)
            {
              bad(pt, (uintptr_t)p, v, g);
            }

          g = p[1];
          if (g != ~v)
            {
              bad(pt, (uintptr_t)&p[1], ~v, g);
            }
        }
    }

  end(pt, t0);

  t0 = now_us();
  begin(pt, "address lines: a word at each 2^n");
  {
    volatile uint32_t *p = (volatile uint32_t *)pt->u;
    uint32_t n;
    uint32_t g;

    /* 0 and every power of two below the size, each in turn set to
     * 55555555h: only it may change
     */

    p[0] = 0xaaaaaaaa;
    for (n = 4; n < pt->size; n <<= 1)
      {
        p[n / 4] = 0xaaaaaaaa;
      }

    for (k = 0; k < pt->size; k = k ? k << 1 : 4)
      {
        p[k / 4] = 0x55555555;
        for (n = 0; n < pt->size; n = n ? n << 1 : 4)
          {
            uint32_t e = n == k ? 0x55555555 : 0xaaaaaaaa;

            if ((g = p[n / 4]) != e)
              {
                bad(pt, (uintptr_t)&p[n / 4], e, g);
              }
          }

        p[k / 4] = 0xaaaaaaaa;
      }
  }

  end(pt, t0);
}

/****************************************************************************
 * Name: t_addr
 ****************************************************************************/

static void t_addr(struct pt_s *pt)
{
  uint64_t t0;

  t0 = now_us();
  begin(pt, "address-in-address, uncached");
  fill(pt, pt->u, 0, 0);
  check(pt, pt->u, 0, 0);
  end(pt, t0);

  t0 = now_us();
  begin(pt, "complement, uncached");
  fill(pt, pt->u, 1, 0);
  check(pt, pt->u, 1, 0);
  end(pt, t0);

  t0 = now_us();
  begin(pt, "address-in-address, cached write/uncached read");
  fill(pt, pt->c, 0, 0);
  xip_flush();
  check(pt, pt->u, 0, 0);
  end(pt, t0);

  t0 = now_us();
  begin(pt, "complement, uncached write/cached read");
  fill(pt, pt->u, 1, 0);
  xip_flush();
  check(pt, pt->c, 1, 0);
  end(pt, t0);
}

/****************************************************************************
 * Name: t_rand
 ****************************************************************************/

static void t_rand(struct pt_s *pt, int passes, uint32_t seed0)
{
  char name[48];
  int i;

  for (i = 0; i < passes; i++)
    {
      uint32_t seed = seed0 + 0x9e3779b9u * (uint32_t)i;
      uint64_t t0 = now_us();
      int cw = i & 1;

      seed = seed ? seed : 1;
      snprintf(name, sizeof(name), "random, seed %08" PRIx32 ", %s",
               seed, cw ? "cached w/uncached r" : "uncached w/cached r");
      begin(pt, name);
      fill(pt, cw ? pt->c : pt->u, 2, seed);
      xip_flush();
      check(pt, cw ? pt->u : pt->c, 2, seed);
      end(pt, t0);
    }
}

/****************************************************************************
 * Name: t_mix
 *
 * Windows of 64 KB (one across the dies' boundary at 8 MB): random byte,
 * halfword and word stores, aligned and not, to the PSRAM and to a copy in
 * the SRAM, with random reads of each kind checked against the copy, then
 * the whole window compared through the other alias.
 *
 ****************************************************************************/

static void t_mix(struct pt_s *pt, uint32_t seed)
{
  uint64_t t0 = now_us();
  uint8_t *sh = pt->a;
  uint32_t nwin = 24;
  uint32_t w;

  begin(pt, "byte/half/word, unaligned, 24 x 64 KB");
  for (w = 0; w < nwin; w++)
    {
      uint32_t off;
      uintptr_t base;
      uintptr_t other;
      uint32_t i;

      off = (w == 0 ? 8 * MB - SHADOW / 2 :
             (pt->size / nwin) * w + (w * 1021u) % 4096) & ~3u;
      if (off + SHADOW > pt->size)
        {
          off = pt->size - SHADOW;
        }

      base  = (w & 1) ? pt->c + off : pt->u + off;
      other = (w & 1) ? pt->u + off : pt->c + off;

      for (i = 0; i < SHADOW; i += 4)
        {
          *(uint32_t *)(sh + i) = xs32(&seed);
          *(volatile uint32_t *)(base + i) = *(uint32_t *)(sh + i);
        }

      for (i = 0; i < 30000; i++)
        {
          uint32_t r = xs32(&seed);
          uint32_t v = xs32(&seed);
          uint32_t o = (r >> 8) % (SHADOW - 4);
          uint32_t g;
          uint32_t e;

          switch (r & 15)
            {
              case 0:
              case 1:
              case 2:
                *(volatile uint8_t *)(base + o) = (uint8_t)v;
                sh[o] = (uint8_t)v;
                continue;

              case 3:
              case 4:
                o &= ~1u;
                *(volatile uint16_t *)(base + o) = (uint16_t)v;
                *(uint16_t *)(sh + o) = (uint16_t)v;
                continue;

              case 5:
                ((volatile struct u16p_s *)(base + o))->v = (uint16_t)v;
                ((struct u16p_s *)(sh + o))->v = (uint16_t)v;
                continue;

              case 6:
              case 7:
                o &= ~3u;
                *(volatile uint32_t *)(base + o) = v;
                *(uint32_t *)(sh + o) = v;
                continue;

              case 8:
              case 9:
                ((volatile struct u32p_s *)(base + o))->v = v;
                ((struct u32p_s *)(sh + o))->v = v;
                continue;

              case 10:
                g = *(volatile uint8_t *)(base + o);
                e = sh[o];
                break;

              case 11:
                g = ((volatile struct u16p_s *)(base + o))->v;
                e = ((struct u16p_s *)(sh + o))->v;
                break;

              case 12:
              case 13:
                g = ((volatile struct u32p_s *)(base + o))->v;
                e = ((struct u32p_s *)(sh + o))->v;
                break;

              default:
                o &= ~3u;
                g = *(volatile uint32_t *)(base + o);
                e = *(uint32_t *)(sh + o);
                break;
            }

          /* The loads: against the copy */

          if (g != e)
            {
              bad(pt, base + o, e, g);
            }
        }

      xip_flush();
      for (i = 0; i < SHADOW; i += 4)
        {
          uint32_t g = *(volatile uint32_t *)(other + i);
          uint32_t e = *(uint32_t *)(sh + i);

          if (g != e)
            {
              bad(pt, other + i, e, g);
            }
        }
    }

  end(pt, t0);
}

/****************************************************************************
 * Name: t_burst
 *
 * memcpy() of blocks to, from and within the PSRAM at odd offsets across
 * 1 KB pages, 4 KB and the 8 MB die boundary, through either alias.
 *
 ****************************************************************************/

static int cmpbuf(struct pt_s *pt, const uint8_t *e, const uint8_t *g,
                  size_t n, uintptr_t at)
{
  size_t i;
  int nbad = 0;

  if (memcmp(e, g, n) == 0)
    {
      return 0;
    }

  for (i = 0; i < n; i++)
    {
      if (e[i] != g[i])
        {
          bad(pt, at + i, e[i], g[i]);
          nbad++;
        }
    }

  return nbad;
}

static void t_burst(struct pt_s *pt, uint32_t seed)
{
  static const uint32_t offs[] =
  {
    1024 - 1, 4096 - 3, 3 * 1024 + 5, 1 * MB - 17, 4 * MB - 1000,
    8 * MB - 16 * 1024 - 7, 8 * MB - 1, 8 * MB + 513, 12 * MB - 2,
    16 * MB - 32 * 1024
  };

  static const uint32_t lens[] =
  {
    32 * 1024, 17 * 1024 + 3, 1025, 1023, 4097, 64, 7
  };

  uint64_t t0 = now_us();
  uint32_t i;
  uint32_t j;
  uint32_t k;

  begin(pt, "memcpy bursts across pages/dies (SRAM<->PSRAM)");
  for (i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
    {
      for (j = 0; j < sizeof(lens) / sizeof(lens[0]); j++)
        {
          uint32_t off = offs[i];
          uint32_t len = lens[j];
          int cw = (i + j) & 1;

          if (off + len > pt->size)
            {
              continue;
            }

          for (k = 0; k < len; k++)
            {
              pt->a[k] = (uint8_t)xs32(&seed);
            }

          memcpy((void *)((cw ? pt->c : pt->u) + off), pt->a, len);
          xip_flush();
          memcpy(pt->b, (void *)((cw ? pt->u : pt->c) + off), len);
          cmpbuf(pt, pt->a, pt->b, len, (cw ? pt->u : pt->c) + off);
        }
    }

  end(pt, t0);

  /* Within the PSRAM: 1 MB from below the die boundary to across it and
   * back, through the cache and not
   */

  t0 = now_us();
  begin(pt, "memcpy 1 MB PSRAM->PSRAM across the 8 MB boundary");
  if (pt->size >= 16 * MB)
    {
      static const uint32_t pairs[][2] =
      {
        { 2 * MB + 3, 8 * MB - 512 * 1024 + 1 },
        { 8 * MB - 300 * 1024 - 5, 13 * MB + 7 },
        { 9 * MB + 1, 1 * MB - 9 },
      };

      for (i = 0; i < 3; i++)
        {
          uint32_t src = pairs[i][0];
          uint32_t dst = pairs[i][1];
          uint32_t s = seed + i;
          uint32_t m = MB;
          uintptr_t sb = (i & 1) ? pt->c : pt->u;
          uintptr_t db = (i & 1) ? pt->u : pt->c;

          for (k = 0; k < m; k++)
            {
              *(volatile uint8_t *)(pt->u + src + k) = (uint8_t)xs32(&s);
            }

          xip_flush();
          memcpy((void *)(db + dst), (void *)(sb + src), m);
          xip_flush();

          s = seed + i;
          for (k = 0; k < m; k++)
            {
              uint8_t e = (uint8_t)xs32(&s);
              uint8_t g = *(volatile uint8_t *)(pt->u + dst + k);

              if (e != g)
                {
                  bad(pt, pt->u + dst + k, e, g);
                }
            }
        }
    }
  else
    {
      printf("(not 16 MB) ");
    }

  end(pt, t0);
}

/****************************************************************************
 * Name: t_retain
 ****************************************************************************/

static void t_retain(struct pt_s *pt, int secs, uint32_t seed)
{
  char name[48];
  uint64_t t0 = now_us();

  snprintf(name, sizeof(name), "retention: write, %d s idle, check", secs);
  begin(pt, name);
  fill(pt, pt->u, 2, seed);
  sleep(secs);
  check(pt, pt->u, 2, seed);
  end(pt, t0);
}

/****************************************************************************
 * Name: t_perf
 ****************************************************************************/

static void rate(const char *name, uint32_t bytes, uint64_t us)
{
  printf("  %-44s %6.1f MB/s\n", name,
         us ? (double)bytes / (double)us : 0.0);
}

static void t_perf(struct pt_s *pt)
{
  uint32_t n = pt->size;
  uint64_t t0;
  uint64_t us;
  uint32_t i;
  int pass;
  volatile uint32_t sink = 0;

  printf("  (MB/s = 10^6 bytes/s; %" PRIu32 " KB each)\n", n / 1024);
  for (pass = 0; pass < 2; pass++)
    {
      uintptr_t base = pass ? pt->u : pt->c;
      const char *an = pass ? "uncached" : "cached";
      char name[48];

      /* Word stores, 8 a time */

      xip_flush();
      t0 = now_us();
      for (i = 0; i < n; i += 32)
        {
          volatile uint32_t *p = (volatile uint32_t *)(base + i);

          p[0] = i; p[1] = i; p[2] = i; p[3] = i;
          p[4] = i; p[5] = i; p[6] = i; p[7] = i;
        }

      xip_op(XIP_OP_CLEAN);
      us = now_us() - t0;
      snprintf(name, sizeof(name), "write, words, %s", an);
      rate(name, n, us);

      xip_flush();
      t0 = now_us();
      {
        uint32_t sum = 0;

        for (i = 0; i < n; i += 32)
          {
            volatile uint32_t *p = (volatile uint32_t *)(base + i);

            sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
          }

        sink = sum;
      }

      us = now_us() - t0;
      snprintf(name, sizeof(name), "read, words, %s", an);
      rate(name, n, us);

      xip_flush();
      t0 = now_us();
      for (i = 0; i < n; i += BUFSZ)
        {
          memcpy(pt->a, (void *)(base + i), BUFSZ);
        }

      us = now_us() - t0;
      snprintf(name, sizeof(name), "memcpy PSRAM->SRAM, %s", an);
      rate(name, n, us);

      xip_flush();
      t0 = now_us();
      for (i = 0; i < n; i += BUFSZ)
        {
          memcpy((void *)(base + i), pt->a, BUFSZ);
        }

      xip_op(XIP_OP_CLEAN);
      us = now_us() - t0;
      snprintf(name, sizeof(name), "memcpy SRAM->PSRAM, %s", an);
      rate(name, n, us);

      /* Random word reads, one at a time (each address depends on the
       * last value read, so they cannot overlap)
       */

      xip_flush();
      {
        uint32_t s = 12345;
        uint32_t cnt = 1u << 19;
        uint32_t v = 0;

        t0 = now_us();
        for (i = 0; i < cnt; i++)
          {
            uint32_t a = (xs32(&s) ^ (v & 1)) & (n - 4);

            v = *(volatile uint32_t *)(base + a);
          }

        us = now_us() - t0;
        sink = v;
        printf("  %-44s %6.1f ns\n",
               pass ? "random word read latency, uncached" :
                      "random word read latency, cached (misses)",
               (double)us * 1000.0 / cnt);
      }
    }

  /* The same loop in the SRAM, for what the loop itself costs */

  {
    uint32_t s = 12345;
    uint32_t cnt = 1u << 19;
    uint32_t v = 0;

    t0 = now_us();
    for (i = 0; i < cnt; i++)
      {
        uint32_t a = (xs32(&s) ^ (v & 1)) & (BUFSZ - 4);

        v = *(volatile uint32_t *)(pt->a + a);
      }

    us = now_us() - t0;
    sink = v;
    printf("  %-44s %6.1f ns\n", "(same loop on the SRAM)",
           (double)us * 1000.0 / cnt);
  }

  (void)sink;
}

/****************************************************************************
 * Name: set_timing
 ****************************************************************************/

static void set_timing(uint32_t t)
{
  xip_flush();
  rp23xx_psram_set_timing(t);
  xip_flush();
}

static void usage(const char *prog)
{
  printf("Usage: %s [-s seed] [-n passes] [-r secs] [-t m1timing] "
         "[test...]\n"
         "  tests: info bus addr rand mix burst retain perf all "
         "(default: all)\n"
         "  -t: set QMI M1_TIMING (hex) first, kept until reboot\n"
         "  -l N: run the tests N times\n", prog);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct pt_s pt;
  uint32_t seed = 0x2545f491;
  int passes = 4;
  int secs = 10;
  int loops = 1;
  int first;
  int l;
  int i;

  memset(&pt, 0, sizeof(pt));

  for (i = 1; i < argc && argv[i][0] == '-'; i++)
    {
      if (i + 1 >= argc)
        {
          usage(argv[0]);
          return 1;
        }

      switch (argv[i][1])
        {
          case 's':
            seed = strtoul(argv[++i], NULL, 0);
            break;
          case 'n':
            passes = atoi(argv[++i]);
            break;
          case 'r':
            secs = atoi(argv[++i]);
            break;
          case 'l':
            loops = atoi(argv[++i]);
            break;
          case 't':
            set_timing(strtoul(argv[++i], NULL, 16));
            break;
          default:
            usage(argv[0]);
            return 1;
        }
    }

  first = i;
  rp23xx_psram_info(&pt.info);
  pt.size = pt.info.size;
  pt.c = RP23XX_PSRAM_CACHED;
  pt.u = RP23XX_PSRAM_UNCACHED;

  if (_epsram_bss != NULL && (uintptr_t)_epsram_bss > pt.c &&
      (uintptr_t)_epsram_bss < pt.c + RP23XX_PSRAM_WINDOW)
    {
      pt.savelen = ((uintptr_t)_epsram_bss - pt.c + 7) & ~7u;
    }

  if (first == argc || strcmp(argv[first], "info") == 0 ||
      strcmp(argv[first], "all") == 0)
    {
      t_info(&pt);
    }

  if (pt.size < 1 * MB)
    {
      printf("psramtest: no PSRAM\n");
      return 1;
    }

  if (first < argc && strcmp(argv[first], "info") == 0 && argc - first == 1)
    {
      return 0;
    }

  pt.a = malloc(SHADOW);
  pt.b = malloc(BUFSZ);
  pt.save = pt.savelen ? malloc(pt.savelen) : NULL;
  if (pt.a == NULL || pt.b == NULL || (pt.savelen && pt.save == NULL))
    {
      printf("psramtest: no memory\n");
      return 1;
    }

  /* From here on until it is put back, NSH's variables are not there: no
   * leaving early (no Ctrl-C)
   */

  signal(SIGINT, SIG_IGN);
  if (pt.savelen)
    {
      memcpy(pt.save, (void *)pt.c, pt.savelen);
    }

  xip_flush();

  for (l = 0; l < loops; l++)
    {
      int all = first == argc;

      if (loops > 1)
        {
          printf("--- loop %d/%d\n", l + 1, loops);
        }

      for (i = first; all || i < argc; i++)
        {
          const char *t = all ? "all" : argv[i];
          int a = strcmp(t, "all") == 0;

          if (a || strcmp(t, "bus") == 0)
            {
              t_bus(&pt);
            }

          if (a || strcmp(t, "addr") == 0)
            {
              t_addr(&pt);
            }

          if (a || strcmp(t, "rand") == 0)
            {
              t_rand(&pt, passes, seed + l * 77);
            }

          if (a || strcmp(t, "mix") == 0)
            {
              t_mix(&pt, seed + l);
            }

          if (a || strcmp(t, "burst") == 0)
            {
              t_burst(&pt, seed + l);
            }

          if (a || strcmp(t, "retain") == 0)
            {
              t_retain(&pt, secs, seed ^ 0x5555 ^ l);
            }

          if (a || strcmp(t, "perf") == 0)
            {
              t_perf(&pt);
            }

          if (all)
            {
              break;
            }
        }
    }

  /* Put the firmware's data back */

  xip_flush();
  if (pt.savelen)
    {
      memcpy((void *)pt.c, pt.save, pt.savelen);
      xip_op(XIP_OP_CLEAN);
    }

  printf("psramtest: %s (%lu errors)\n",
         pt.errors ? "FAIL" : "ALL PASS", pt.errors);

  free(pt.save);
  free(pt.b);
  free(pt.a);
  return pt.errors ? 1 : 0;
}
