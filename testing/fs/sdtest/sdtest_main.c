/****************************************************************************
 * apps/testing/fs/sdtest/sdtest_main.c
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

/* Storage tests for the microSD (docs/storage.md):
 *
 *   sdtest crc FILE...                    CRC-32 and size of each file
 *   sdtest read FILE [KB [sram|psram [MB]]]  read throughput
 *   sdtest write FILE MB [KB [sram|psram]]   write throughput (then fsync)
 *   sdtest stress DIR MB [SEED [keep]]    files of random sizes written
 *       in random pieces from unaligned buffers, overwritten in places
 *       (garbage first, then the right data), read back in random pieces
 *       at random offsets and checked byte for byte; removed afterwards
 *
 * Every byte of a stress file is a function of the seed, the file and the
 * offset, so nothing has to be kept to check it.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/stat.h>
#include <sys/statfs.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SRAM_BYTES   32768
#define WORK_BYTES   (65536 + 64)
#define MAXFILE      (8 * 1024 * 1024)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* In the SRAM while the module runs (fpc_modheap.c), if there is room */

static uint8_t g_sram[SRAM_BYTES] __attribute__((section(".sram.sdtest"),
                                                 aligned(16)));
static uint32_t g_crctab[256];
static uint32_t g_rand;
static uint32_t g_sizes[512];
static bool g_verbose;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint32_t rnd(void)
{
  g_rand ^= g_rand << 13;
  g_rand ^= g_rand >> 17;
  g_rand ^= g_rand << 5;
  return g_rand;
}

static uint32_t rnd_range(uint32_t lo, uint32_t hi)
{
  return lo + rnd() % (hi - lo + 1);
}

static uint32_t mix(uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7feb352d;
  x ^= x >> 15;
  x *= 0x846ca68b;
  x ^= x >> 16;
  return x;
}

/* The content of file f of a stress run at offset pos */

static void fill(uint8_t *buf, uint32_t seed, uint32_t f, uint32_t pos,
                 uint32_t len)
{
  uint32_t key = seed * 0x9e3779b9u ^ f * 0x85ebca6bu;

  while (len > 0)
    {
      uint32_t w = mix(key ^ (pos >> 2));
      uint32_t i = pos & 3;

      for (; i < 4 && len > 0; i++, len--, pos++)
        {
          *buf++ = w >> (8 * i);
        }
    }
}

static void crc_init(void)
{
  uint32_t i;
  int b;

  for (i = 0; i < 256; i++)
    {
      uint32_t c = i;

      for (b = 0; b < 8; b++)
        {
          c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        }

      g_crctab[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
  while (n-- > 0)
    {
      crc = g_crctab[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    }

  return crc;
}

static uint8_t *pick_buffer(const char *kind, size_t bytes, void **tofree)
{
  *tofree = NULL;
  if (kind != NULL && strcmp(kind, "sram") == 0)
    {
      return bytes <= SRAM_BYTES ? g_sram : NULL;
    }

  *tofree = malloc(bytes + 16);
  return *tofree;
}

static int cmd_crc(int argc, char **argv)
{
  int ret = 0;
  int i;

  for (i = 2; i < argc; i++)
    {
      uint32_t crc = 0xffffffffu;
      uint64_t size = 0;
      ssize_t n;
      int fd;

      fd = open(argv[i], O_RDONLY);
      if (fd < 0)
        {
          printf("%s: %d\n", argv[i], errno);
          ret = 1;
          continue;
        }

      while ((n = read(fd, g_sram, SRAM_BYTES)) > 0)
        {
          crc = crc_update(crc, g_sram, n);
          size += n;
        }

      if (n < 0)
        {
          printf("%s: read error %d\n", argv[i], errno);
          ret = 1;
        }

      close(fd);
      printf("%08" PRIx32 " %10" PRIu64 " %s\n", ~crc, size, argv[i]);
    }

  return ret;
}

static int cmd_read(int argc, char **argv)
{
  size_t kb = argc > 3 ? atoi(argv[3]) : 32;
  const char *kind = argc > 4 ? argv[4] : "psram";
  uint64_t limit = (argc > 5 ? atoi(argv[5]) : 1 << 30) * 1048576ull;
  uint64_t total = 0;
  uint64_t t0;
  uint64_t us;
  void *tofree;
  uint8_t *buf;
  ssize_t n;
  int fd;

  buf = pick_buffer(kind, kb * 1024, &tofree);
  if (buf == NULL)
    {
      printf("no buffer\n");
      return 1;
    }

  fd = open(argv[2], O_RDONLY);
  if (fd < 0)
    {
      printf("%s: %d\n", argv[2], errno);
      free(tofree);
      return 1;
    }

  t0 = now_us();
  while (total < limit && (n = read(fd, buf, kb * 1024)) > 0)
    {
      total += n;
    }

  us = now_us() - t0;
  close(fd);
  printf("read %" PRIu64 " bytes in %" PRIu64 " us: %" PRIu64 " KB/s "
         "(%zu KB pieces into %s at %p)\n", total, us,
         us ? total * 1000000 / 1024 / us : 0, kb, kind, buf);
  free(tofree);
  return 0;
}

static int cmd_write(int argc, char **argv)
{
  uint64_t bytes = (uint64_t)atoi(argv[3]) * 1048576;
  size_t kb = argc > 4 ? atoi(argv[4]) : 32;
  const char *kind = argc > 5 ? argv[5] : "psram";
  uint64_t total = 0;
  uint64_t t0;
  uint64_t us;
  void *tofree;
  uint8_t *buf;
  int fd;

  buf = pick_buffer(kind, kb * 1024, &tofree);
  if (buf == NULL)
    {
      printf("no buffer\n");
      return 1;
    }

  fill(buf, 1, 1, 0, kb * 1024);
  fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      printf("%s: %d\n", argv[2], errno);
      free(tofree);
      return 1;
    }

  t0 = now_us();
  while (total < bytes)
    {
      ssize_t n = write(fd, buf, kb * 1024);

      if (n <= 0)
        {
          printf("write error %d at %" PRIu64 "\n", errno, total);
          break;
        }

      total += n;
    }

  fsync(fd);
  close(fd);
  us = now_us() - t0;
  printf("wrote %" PRIu64 " bytes in %" PRIu64 " us: %" PRIu64 " KB/s "
         "(%zu KB pieces from %s)\n", total, us,
         us ? total * 1000000 / 1024 / us : 0, kb, kind);
  free(tofree);
  return 0;
}

/* Write a piece of file f, [pos, pos + len), from an unaligned place in
 * one of the buffers
 */

static int put(int fd, uint8_t *work, uint32_t seed, uint32_t f,
               uint32_t pos, uint32_t len, bool garbage)
{
  uint8_t *p;
  ssize_t n;

  if (len <= SRAM_BYTES - 16 && (rnd() & 1))
    {
      p = g_sram + (rnd() & 15);
    }
  else
    {
      p = work + (rnd() & 15);
    }

  if (garbage)
    {
      uint32_t i;

      for (i = 0; i < len; i++)
        {
          p[i] = rnd();
        }
    }
  else
    {
      fill(p, seed, f, pos, len);
    }

  if (lseek(fd, pos, SEEK_SET) != pos)
    {
      return -1;
    }

  n = write(fd, p, len);
  return n == len ? 0 : -1;
}

/* Read [pos, pos + len) of file f into an unaligned place and check it */

static int check(int fd, uint8_t *work, uint8_t *want, uint32_t seed,
                 uint32_t f, uint32_t pos, uint32_t len, uint64_t *errs)
{
  uint8_t *p;
  ssize_t n;
  uint32_t i;

  if (len <= SRAM_BYTES - 16 && (rnd() & 1))
    {
      p = g_sram + (rnd() & 15);
    }
  else
    {
      p = work + (rnd() & 15);
    }

  if (lseek(fd, pos, SEEK_SET) != pos)
    {
      return -1;
    }

  n = read(fd, p, len);
  if (n != len)
    {
      printf("  file %" PRIu32 ": read %zd of %" PRIu32 " at %" PRIu32
             " (errno %d)\n", f, n, len, pos, errno);
      return -1;
    }

  fill(want, seed, f, pos, len);
  if (memcmp(p, want, len) != 0)
    {
      for (i = 0; i < len && p[i] == want[i]; i++)
        {
        }

      printf("  file %" PRIu32 ": MISMATCH at %" PRIu32 " (%02x, want "
             "%02x), piece %" PRIu32 "+%" PRIu32 " at %p\n", f, pos + i,
             p[i], want[i], pos, len, p);
      (*errs)++;
    }

  return 0;
}

static int cmd_stress(int argc, char **argv)
{
  const char *dir = argv[2];
  uint64_t goal = (uint64_t)atoi(argv[3]) * 1048576;
  uint32_t seed = argc > 4 ? strtoul(argv[4], NULL, 0) : 1;
  bool keep = argc > 5 && strstr(argv[5], "keep") != NULL;
  uint32_t *sizes = g_sizes;
  uint64_t written = 0;
  uint64_t readb = 0;
  uint64_t errs = 0;
  uint64_t t0;
  uint64_t tw;
  uint64_t tr;
  struct statfs sf;
  char path[128];
  uint8_t *work;
  uint8_t *want;
  uint32_t nfiles = 0;
  uint32_t f;
  int fails = 0;

  g_rand = seed * 2654435761u | 1;
  g_verbose = argc > 5 && strchr(argv[5], 'v') != NULL;

  if (statfs("/mnt/sd0", &sf) == 0 &&
      (uint64_t)sf.f_bavail * sf.f_bsize < goal + goal / 4 + 16777216)
    {
      printf("not enough room: %" PRIu64 " MB free\n",
             (uint64_t)sf.f_bavail * sf.f_bsize / 1048576);
      return 1;
    }

  work = malloc(WORK_BYTES);
  want = malloc(WORK_BYTES);
  if (work == NULL || want == NULL)
    {
      printf("no memory\n");
      free(work);
      free(want);
      return 1;
    }

  mkdir(dir, 0777);

  /* Sizes: some tiny, some around a cluster, most up to 8 MB */

  while (written < goal && nfiles < 512)
    {
      uint32_t r = rnd() % 8;
      uint32_t s = r == 0 ? rnd_range(1, 4096) :
                   r == 1 ? rnd_range(4096, 65536) :
                            rnd_range(65536, MAXFILE);

      if (s > goal - written)
        {
          s = goal - written > 0 ? goal - written : 1;
        }

      sizes[nfiles++] = s;
      written += s;
    }

  printf("stress: %" PRIu32 " files, %" PRIu64 " bytes, seed %" PRIu32
         ", in %s\n", nfiles, written, seed, dir);
  written = 0;

  /* Write: random pieces in order, then overwrites (garbage, then right) */

  t0 = now_us();
  for (f = 0; f < nfiles && fails == 0; f++)
    {
      uint32_t pos = 0;
      int k;
      int fd;

      snprintf(path, sizeof(path), "%s/f%03" PRIu32 ".bin", dir, f);
      fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (fd < 0)
        {
          printf("  %s: %d\n", path, errno);
          fails++;
          break;
        }

      while (pos < sizes[f])
        {
          uint32_t len = (rnd() & 3) == 0 ? 512 * rnd_range(1, 128) :
                                            rnd_range(1, 65536);

          if (len > sizes[f] - pos)
            {
              len = sizes[f] - pos;
            }

          if (g_verbose)
            {
              printf("  f%" PRIu32 " put %" PRIu32 "+%" PRIu32 "\n", f, pos,
                     len);
              fflush(stdout);
            }

          if (put(fd, work, seed, f, pos, len, false) < 0)
            {
              printf("  %s: write %" PRIu32 "+%" PRIu32 ": %d\n", path, pos,
                     len, errno);
              fails++;
              break;
            }

          pos += len;
          written += len;
        }

      for (k = 0; k < 4 && fails == 0 && sizes[f] > 1; k++)
        {
          uint32_t at = rnd() % sizes[f];
          uint32_t len = rnd_range(1, 32768);

          if (len > sizes[f] - at)
            {
              len = sizes[f] - at;
            }

          if (put(fd, work, seed, f, at, len, true) < 0 ||
              put(fd, work, seed, f, at, len, false) < 0)
            {
              printf("  %s: overwrite: %d\n", path, errno);
              fails++;
            }

          written += 2 * len;
        }

      if (close(fd) < 0)
        {
          printf("  %s: close: %d\n", path, errno);
          fails++;
        }
    }

  tw = now_us() - t0;

  /* Read back: the whole of each file in random pieces, then random
   * pieces anywhere
   */

  t0 = now_us();
  for (f = 0; f < nfiles && fails == 0; f++)
    {
      struct stat st;
      uint32_t pos = 0;
      int k;
      int fd;

      snprintf(path, sizeof(path), "%s/f%03" PRIu32 ".bin", dir, f);
      fd = open(path, O_RDONLY);
      if (fd < 0 || fstat(fd, &st) < 0 || st.st_size != sizes[f])
        {
          printf("  %s: open/size: %d\n", path, errno);
          fails++;
          if (fd >= 0)
            {
              close(fd);
            }

          break;
        }

      while (pos < sizes[f] && fails == 0)
        {
          uint32_t len = (rnd() & 3) == 0 ? 512 * rnd_range(1, 128) :
                                            rnd_range(1, 65536);

          if (len > sizes[f] - pos)
            {
              len = sizes[f] - pos;
            }

          if (check(fd, work, want, seed, f, pos, len, &errs) < 0)
            {
              fails++;
            }

          pos += len;
          readb += len;
        }

      for (k = 0; k < 8 && fails == 0; k++)
        {
          uint32_t at = rnd() % sizes[f];
          uint32_t len = rnd_range(1, 65536);

          if (len > sizes[f] - at)
            {
              len = sizes[f] - at;
            }

          if (check(fd, work, want, seed, f, at, len, &errs) < 0)
            {
              fails++;
            }

          readb += len;
        }

      close(fd);
    }

  tr = now_us() - t0;

  printf("stress: wrote %" PRIu64 " bytes in %" PRIu64 " ms (%" PRIu64
         " KB/s), read %" PRIu64 " in %" PRIu64 " ms (%" PRIu64 " KB/s): "
         "%" PRIu64 " mismatches, %d failures\n",
         written, tw / 1000, tw ? written * 1000000 / 1024 / tw : 0,
         readb, tr / 1000, tr ? readb * 1000000 / 1024 / tr : 0,
         errs, fails);

  if (!keep)
    {
      for (f = 0; f < nfiles; f++)
        {
          snprintf(path, sizeof(path), "%s/f%03" PRIu32 ".bin", dir, f);
          unlink(path);
        }

      rmdir(dir);
    }

  free(work);
  free(want);
  printf("stress: %s\n", errs == 0 && fails == 0 ? "PASS" : "FAIL");
  return errs == 0 && fails == 0 ? 0 : 1;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  crc_init();

  if (argc >= 3 && strcmp(argv[1], "crc") == 0)
    {
      return cmd_crc(argc, argv);
    }
  else if (argc >= 3 && strcmp(argv[1], "read") == 0)
    {
      return cmd_read(argc, argv);
    }
  else if (argc >= 4 && strcmp(argv[1], "write") == 0)
    {
      return cmd_write(argc, argv);
    }
  else if (argc >= 4 && strcmp(argv[1], "stress") == 0)
    {
      return cmd_stress(argc, argv);
    }

  printf("sdtest crc FILE...\n"
         "sdtest read FILE [KB [sram|psram [MB]]]\n"
         "sdtest write FILE MB [KB [sram|psram]]\n"
         "sdtest stress DIR MB [SEED [keep|v|keepv]]\n");
  return 2;
}
