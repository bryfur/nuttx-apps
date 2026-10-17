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
 *   sdtest seek FILE [BYTES [N [sram|psram [SEED]]]]  N reads of BYTES at
 *       random offsets (each piece's CRC summed: the same seed gives the
 *       same sum whatever the file system does underneath)
 *   sdtest fat DIR [OPS [SEED]]           two files grown in turns (their
 *       clusters interleaved), then random reads, writes (also past the
 *       end), truncations and reopens through several handles at once,
 *       every read checked against a copy in memory; removed afterwards
 *   sdtest stress DIR MB [SEED [keep]]    files of random sizes written
 *       in random pieces from unaligned buffers, overwritten in places
 *       (garbage first, then the right data), read back in random pieces
 *       at random offsets and checked byte for byte; removed afterwards
 *   sdtest fatfault                       (the simulator) the FAT and file
 *       caches after a sector read that fails: a RAM disk of its own whose
 *       chosen read scribbles over the buffer and returns -EIO, as a DMA
 *       into the cache whose CRC fails afterwards does; what the caches
 *       then hold must not be taken for the old sector (fs_fat32util.c)
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

#if defined(CONFIG_ARCH_SIM) && defined(CONFIG_FSUTILS_MKFATFS)
#  include <sys/mount.h>
#  include <nuttx/fs/fs.h>
#  include <nuttx/fs/ioctl.h>
#  include "fsutils/mkfatfs.h"
#endif

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

static int cmd_seek(int argc, char **argv)
{
  size_t bytes = argc > 3 ? atoi(argv[3]) : 4096;
  uint32_t n = argc > 4 ? atoi(argv[4]) : 1000;
  const char *kind = argc > 5 ? argv[5] : "psram";
  uint64_t total = 0;
  uint64_t t0;
  uint64_t us;
  uint32_t sum = 0;
  uint32_t back = 0;
  off_t size;
  off_t last = 0;
  void *tofree;
  uint8_t *buf;
  uint32_t i;
  int fd;

  g_rand = argc > 6 ? atoi(argv[6]) : 1;
  if (g_rand == 0)
    {
      g_rand = 1;
    }

  buf = pick_buffer(kind, bytes, &tofree);
  fd = open(argv[2], O_RDONLY);
  if (buf == NULL || fd < 0 || bytes == 0)
    {
      printf("%s: %d\n", argv[2], buf == NULL ? ENOMEM : errno);
      free(tofree);
      return 1;
    }

  size = lseek(fd, 0, SEEK_END);
  if (size <= (off_t)bytes)
    {
      printf("%s: too small\n", argv[2]);
      close(fd);
      free(tofree);
      return 1;
    }

  t0 = now_us();
  for (i = 0; i < n; i++)
    {
      off_t pos = rnd() % (uint32_t)(size - bytes);
      ssize_t got;

      back += pos < last;
      last = pos;
      if (lseek(fd, pos, SEEK_SET) != pos ||
          (got = read(fd, buf, bytes)) != (ssize_t)bytes)
        {
          printf("read error %d at %" PRIu32 "\n", errno, (uint32_t)pos);
          break;
        }

      total += got;
      sum += crc_update(0xffffffffu, buf, bytes);
    }

  us = now_us() - t0;
  close(fd);
  printf("seek: %" PRIu32 " reads of %zu bytes (%" PRIu32 " backwards) in "
         "%" PRIu64 " us: %" PRIu64 " us a read, %" PRIu64 " KB/s, "
         "sum %08" PRIx32 " (%s)\n", i, bytes, back, us, i ? us / i : 0,
         us ? total * 1000000 / 1024 / us : 0, sum, kind);
  free(tofree);
  return i == n ? 0 : 1;
}

/* sdtest fat: the file system's cluster bookkeeping (seeks backwards and
 * forwards, files growing and shrinking under open handles) against a
 * model of the file in memory.
 *
 * Handle 0 writes; it is synced after every change.  Each handle keeps a
 * sector of the file of its own (NuttX's FAT), which another handle's
 * writes do not update: the other handles read whole sectors only, which
 * go past that cache.
 */

#define FAT_MAXSIZE  (768 * 1024)
#define FAT_HANDLES  4

struct fatcheck_s
{
  uint8_t *model;                   /* The file as it should be */
  uint8_t *buf;
  uint32_t size;
  int fd[FAT_HANDLES];              /* 0: read-write, the rest read-only */
  const char *path;
  uint32_t errs;
};

static int fat_reopen(struct fatcheck_s *c, int h, int flags)
{
  if (c->fd[h] >= 0)
    {
      close(c->fd[h]);
    }

  c->fd[h] = open(c->path, flags, 0666);
  if (c->fd[h] < 0)
    {
      printf("  open %s: %d\n", c->path, errno);
      c->errs++;
      return -1;
    }

  return 0;
}

static void fat_check(struct fatcheck_s *c, int h, uint32_t pos,
                      uint32_t len, const char *what)
{
  uint32_t want;
  ssize_t n;
  uint32_t i;

  if (h != 0)
    {
      len += pos & 511;
      pos &= ~511;
      len = (len + 511) & ~511;
    }

  want = pos >= c->size ? 0 : len > c->size - pos ? c->size - pos : len;
  if (h != 0 && (want & 511) != 0)
    {
      /* The end of the file: from handle 0 */

      h = 0;
    }

  if (lseek(c->fd[h], pos, SEEK_SET) != (off_t)pos)
    {
      printf("  %s: lseek %" PRIu32 ": %d\n", what, pos, errno);
      c->errs++;
      return;
    }

  n = read(c->fd[h], c->buf, len);
  if (n != (ssize_t)want)
    {
      printf("  %s: handle %d read %" PRIu32 "+%" PRIu32 ": %zd, want %"
             PRIu32 " (size %" PRIu32 ", errno %d)\n", what, h, pos, len, n,
             want, c->size, errno);
      c->errs++;
      return;
    }

  if (memcmp(c->buf, c->model + pos, want) != 0)
    {
      for (i = 0; i < want && c->buf[i] == c->model[pos + i]; i++)
        {
        }

      printf("  %s: handle %d MISMATCH at %" PRIu32 " (%02x, want %02x), "
             "read %" PRIu32 "+%" PRIu32 ", size %" PRIu32 "\n", what, h,
             pos + i, c->buf[i], c->model[pos + i], pos, len, c->size);
      c->errs++;
    }
}

static uint32_t fat_len(void)
{
  uint32_t r = rnd() % 4;

  return r == 0 ? rnd_range(1, 600) : r == 1 ? 512 * rnd_range(1, 16) :
                  rnd_range(1, 40000);
}

static int cmd_fat(int argc, char **argv)
{
  const char *dir = argv[2];
  uint32_t ops = argc > 3 ? atoi(argv[3]) : 3000;
  uint32_t seed = argc > 4 ? strtoul(argv[4], NULL, 0) : 1;
  struct fatcheck_s c;
  bool v = argc > 5 && strchr(argv[5], 'v') != NULL;
  bool paranoid = argc > 5 && strchr(argv[5], 'p') != NULL;
  char path[128];
  char other[128];
  uint32_t i = 0;
  int fo;
  int h;

  memset(&c, 0, sizeof(c));
  g_rand = seed * 2654435761u | 1;
  c.model = malloc(FAT_MAXSIZE + 65536);
  c.buf = malloc(65536);
  if (c.model == NULL || c.buf == NULL)
    {
      printf("no memory\n");
      free(c.model);
      free(c.buf);
      return 1;
    }

  mkdir(dir, 0777);
  snprintf(path, sizeof(path), "%s/fat.bin", dir);
  snprintf(other, sizeof(other), "%s/other.bin", dir);
  c.path = path;
  for (h = 0; h < FAT_HANDLES; h++)
    {
      c.fd[h] = -1;
    }

  /* The two files grown in turns, in pieces of a few clusters at most,
   * so that their chains interleave
   */

  if (fat_reopen(&c, 0, O_RDWR | O_CREAT | O_TRUNC) < 0)
    {
      goto out;
    }

  fo = open(other, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  while (fo >= 0 && c.size < FAT_MAXSIZE / 2)
    {
      uint32_t len = rnd_range(1, 9000);

      fill(c.model + c.size, seed, 1, c.size, len);
      if (write(c.fd[0], c.model + c.size, len) != (ssize_t)len ||
          fsync(c.fd[0]) < 0 ||
          write(fo, c.buf, rnd_range(1, 9000)) <= 0)
        {
          printf("  write: %d\n", errno);
          c.errs++;
          break;
        }

      c.size += len;
    }

  if (fo >= 0)
    {
      close(fo);
    }

  for (h = 1; h < FAT_HANDLES; h++)
    {
      fat_reopen(&c, h, O_RDONLY);
    }

  for (i = 0; i < ops && c.errs == 0; i++)
    {
      uint32_t r = rnd() % 100;
      uint32_t pos;
      uint32_t len;

      if (paranoid && i > 0)
        {
          /* All of it, through handle 0 */

          for (pos = 0; pos < c.size && c.errs == 0; pos += 8192)
            {
              fat_check(&c, 0, pos, 8192, "paranoid");
            }

          if (c.errs != 0)
            {
              break;
            }
        }

      h = rnd() % FAT_HANDLES;
      if (r < 60)
        {
          /* A read anywhere (some past the end) */

          len = fat_len();
          pos = rnd() % (c.size + 2048);
          fat_check(&c, h, pos, len, "read");
        }
      else if (r < 64)
        {
          /* Reads backwards through the file */

          len = fat_len();
          for (pos = c.size; pos > 0 && c.errs == 0; )
            {
              pos = pos > len * 7 ? pos - len * 7 : 0;
              fat_check(&c, h, pos, len, "back");
            }
        }
      else if (r < 84)
        {
          /* A write, sometimes past the end (a gap of zeros) */

          len = fat_len();
          pos = rnd() % (c.size + (rnd() % 4 == 0 ? 20000 : 1));
          if (pos + len > FAT_MAXSIZE)
            {
              continue;
            }

          if (pos > c.size)
            {
              memset(c.model + c.size, 0, pos - c.size);
            }

          if (v)
            {
              printf("  %" PRIu32 ": write %" PRIu32 "+%" PRIu32 " (size %"
                     PRIu32 ")\n", i, pos, len, c.size);
            }

          fill(c.model + pos, seed, i + 2, pos, len);
          if (lseek(c.fd[0], pos, SEEK_SET) != (off_t)pos ||
              write(c.fd[0], c.model + pos, len) != (ssize_t)len)
            {
              printf("  write %" PRIu32 "+%" PRIu32 ": %d\n", pos, len,
                     errno);
              c.errs++;
              break;
            }

          if (pos + len > c.size)
            {
              c.size = pos + len;
            }

          fsync(c.fd[0]);

          /* Read back at once through another handle */

          fat_check(&c, 1 + rnd() % (FAT_HANDLES - 1), pos, len, "written");
        }
      else if (r < 90)
        {
          /* ftruncate: shorter or longer */

          uint32_t n = rnd() % 3 == 0 ? rnd() % (c.size + 1) :
                       rnd() % 2 ? c.size + rnd() % 30000 :
                                   c.size - c.size / 8;

          if (n > FAT_MAXSIZE)
            {
              continue;
            }

          if (n > c.size)
            {
              memset(c.model + c.size, 0, n - c.size);
            }

          if (v)
            {
              printf("  %" PRIu32 ": ftruncate %" PRIu32 " -> %" PRIu32 "\n",
                     i, c.size, n);
            }

          if (ftruncate(c.fd[0], n) < 0)
            {
              printf("  ftruncate %" PRIu32 ": %d\n", n, errno);
              c.errs++;
              break;
            }

          c.size = n;
          fsync(c.fd[0]);
          fat_check(&c, h, n > 5000 ? n - 5000 : 0, 10000, "truncated");
        }
      else if (r < 92)
        {
          /* Opened again with O_TRUNC while the others are open */

          if (v)
            {
              printf("  %" PRIu32 ": O_TRUNC\n", i);
            }

          if (fat_reopen(&c, 0, O_RDWR | O_TRUNC) < 0)
            {
              break;
            }

          c.size = 0;
        }
      else if (r < 96)
        {
          /* A read-only handle closed and opened again */

          h = 1 + rnd() % (FAT_HANDLES - 1);
          fat_reopen(&c, h, O_RDONLY);
        }
      else
        {
          /* The other file grows (new clusters interleave), or starts
           * again (the room it had to be taken again)
           */

          fo = open(other, rnd() % 8 == 0 ? O_WRONLY | O_TRUNC :
                                            O_WRONLY | O_APPEND);
          if (fo >= 0)
            {
              if (lseek(fo, 0, SEEK_END) < 1024 * 1024)
                {
                  write(fo, c.buf, rnd_range(1, 20000));
                }

              close(fo);
            }
        }
    }

  /* Everything once more, through a new handle, in pieces */

  if (c.errs == 0)
    {
      uint32_t pos;

      fat_reopen(&c, 1, O_RDONLY);
      for (pos = 0; pos < c.size && c.errs == 0; pos += 4096)
        {
          fat_check(&c, 1, pos, 4096, "final");
        }
    }

out:
  for (h = 0; h < FAT_HANDLES; h++)
    {
      if (c.fd[h] >= 0)
        {
          close(c.fd[h]);
        }
    }

  unlink(path);
  unlink(other);
  rmdir(dir);
  free(c.model);
  free(c.buf);
  printf("fat: %" PRIu32 " operations, size %" PRIu32 ": %s\n", i, c.size,
         c.errs == 0 ? "PASS" : "FAIL");
  return c.errs == 0 ? 0 : 1;
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
 * The caches after a failed read (sdtest fatfault, the simulator)
 ****************************************************************************/

#if defined(CONFIG_ARCH_SIM) && defined(CONFIG_FSUTILS_MKFATFS)

#define FF_SECTORS  8192                  /* 4 MB: FAT16, a sector a cluster */
#define FF_DEV      "/dev/fatfault"
#define FF_MNT      "/mnt/fatfault"

static FAR uint8_t *g_ffdisk;
static volatile int64_t g_fffault = -1;   /* The sector whose read fails */
static volatile int g_ffhits;

static int ff_open(FAR struct inode *inode)
{
  return OK;
}

static int ff_close(FAR struct inode *inode)
{
  return OK;
}

static ssize_t ff_read(FAR struct inode *inode, FAR unsigned char *buffer,
                       blkcnt_t start, unsigned int n)
{
  if (start + n > FF_SECTORS)
    {
      return -EIO;
    }

  if (g_fffault >= start && g_fffault < start + (int64_t)n)
    {
      /* What a DMA straight into the buffer leaves before its CRC is
       * found wrong: other bytes, and an error
       */

      memset(buffer, 0xa5, n * 512);
      g_fffault = -1;
      g_ffhits++;
      return -EIO;
    }

  memcpy(buffer, g_ffdisk + start * 512, n * 512);
  return n;
}

static ssize_t ff_write(FAR struct inode *inode,
                        FAR const unsigned char *buffer, blkcnt_t start,
                        unsigned int n)
{
  if (start + n > FF_SECTORS)
    {
      return -EIO;
    }

  memcpy(g_ffdisk + start * 512, buffer, n * 512);
  return n;
}

static int ff_geometry(FAR struct inode *inode, FAR struct geometry *geo)
{
  memset(geo, 0, sizeof(*geo));
  geo->geo_available    = true;
  geo->geo_writeenabled = true;
  geo->geo_nsectors     = FF_SECTORS;
  geo->geo_sectorsize   = 512;
  return OK;
}

static int ff_ioctl(FAR struct inode *inode, int cmd, unsigned long arg)
{
  return -ENOTTY;
}

static const struct block_operations g_ffops =
{
  ff_open, ff_close, ff_read, ff_write, ff_geometry, ff_ioctl
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  , NULL
#endif
};

static uint8_t ff_byte(int id, uint32_t off)
{
  return (uint8_t)(id * 131 + off * 7 + (off >> 9));
}

/* Write n bytes of file id's pattern, from offset off */

static int ff_put(FAR const char *path, int flags, int id, uint32_t off,
                  uint32_t n)
{
  uint8_t buf[512];
  int fd = open(path, flags, 0666);
  uint32_t i;

  if (fd < 0)
    {
      return -1;
    }

  while (n > 0)
    {
      uint32_t k = n < sizeof(buf) ? n : sizeof(buf);

      for (i = 0; i < k; i++)
        {
          buf[i] = ff_byte(id, off + i);
        }

      if (write(fd, buf, k) != (ssize_t)k)
        {
          close(fd);
          return -1;
        }

      off += k;
      n   -= k;
    }

  return close(fd);
}

/* Bytes of the file that are not file id's pattern (its first `head`
 * bytes: file hid's); -1 if it cannot be read whole
 */

static long ff_check(FAR const char *path, int id, uint32_t size,
                     int hid, uint32_t head)
{
  uint8_t buf[512];
  uint32_t off = 0;
  long bad = 0;
  int fd = open(path, O_RDONLY);
  ssize_t n;

  if (fd < 0)
    {
      return -1;
    }

  while ((n = read(fd, buf, sizeof(buf))) > 0)
    {
      ssize_t i;

      for (i = 0; i < n; i++, off++)
        {
          if (buf[i] != (off < head ? ff_byte(hid, off) : ff_byte(id, off)))
            {
              bad++;
            }
        }
    }

  close(fd);
  return n < 0 || off != size ? -1 : bad;
}

static int ff_remount(void)
{
  umount(FF_MNT);
  return mount(FF_DEV, FF_MNT, "vfat", 0, NULL);
}

static int ff_format(void)
{
  struct fat_format_s fmt = FAT_FORMAT_INITIALIZER;

  memset(g_ffdisk, 0, FF_SECTORS * 512);
  fmt.ff_fattype    = 16;
  fmt.ff_clustshift = 0;
  if (mkfatfs(FF_DEV, &fmt) < 0)
    {
      return -1;
    }

  return mount(FF_DEV, FF_MNT, "vfat", 0, NULL);
}

static int cmd_fatfault(int argc, FAR char **argv)
{
  uint32_t rsvd;
  uint32_t nfats;
  uint32_t rootsec;
  uint32_t fatsz;
  uint32_t data0;
  uint8_t buf[100];
  ssize_t got;
  long bada;
  long badb;
  long badc;
  bool fatok;
  bool fileok;
  int fd;

  g_ffdisk = malloc(FF_SECTORS * 512);
  if (g_ffdisk == NULL ||
      register_blockdriver(FF_DEV, &g_ffops, 0666, NULL) < 0)
    {
      printf("fatfault: no RAM disk\n");
      free(g_ffdisk);
      return 1;
    }

  /* 1. The FAT's cache.  A (clusters 2-201), then B (202-401): B's chain
   * goes on from the FAT's first sector into its second.  B read whole
   * fails at the second, with the first in the cache before.  Then A,
   * opened before (no lookup, which would read a directory sector into
   * the cache first), is read whole: its chain, all in the FAT's first
   * sector, comes from the cache, rightly only if the cache really holds
   * that sector.  A must read back right, then and after a remount.
   */

  if (ff_format() < 0)
    {
      printf("fatfault: mkfatfs or mount failed\n");
      goto errout;
    }

  rsvd    = g_ffdisk[14] | g_ffdisk[15] << 8;
  nfats   = g_ffdisk[16];
  rootsec = ((g_ffdisk[17] | g_ffdisk[18] << 8) * 32 + 511) / 512;
  fatsz   = g_ffdisk[22] | g_ffdisk[23] << 8;
  data0   = rsvd + nfats * fatsz + rootsec;

  ff_put(FF_MNT "/a", O_WRONLY | O_CREAT | O_TRUNC, 1, 0, 200 * 512);
  ff_put(FF_MNT "/b", O_WRONLY | O_CREAT | O_TRUNC, 2, 0, 200 * 512);
  ff_remount();

  g_ffhits = 0;
  bada = -1;
  fd = open(FF_MNT "/a", O_RDONLY);
  if (fd >= 0)
    {
      uint8_t sec[512];
      uint32_t off = 0;
      ssize_t n;

      g_fffault = rsvd + 1;               /* The FAT's second sector */
      ff_check(FF_MNT "/b", 2, 200 * 512, 0, 0);
      g_fffault = -1;
      bada = 0;
      while ((n = read(fd, sec, sizeof(sec))) > 0)
        {
          ssize_t i;

          for (i = 0; i < n; i++, off++)
            {
              bada += sec[i] != ff_byte(1, off);
            }
        }

      if (n < 0 || off != 200 * 512)
        {
          bada = -1;
        }

      close(fd);
    }

  ff_remount();
  badb  = ff_check(FF_MNT "/a", 1, 200 * 512, 0, 0);
  fatok = g_ffhits == 1 && bada == 0 && badb == 0;
  printf("fatfault: FAT cache: %d read(s) failed; A read then: %ld, after a "
         "remount: %ld wrong bytes (-1: unreadable): %s\n", g_ffhits, bada,
         badb, fatok ? "PASS" : "FAIL");

  /* 2. A file's cache.  C (8 sectors from cluster 2): 100 bytes read at 0
   * leave its first sector in the cache; 100 at 600 fail on its second;
   * then 10 bytes written at 0 must go into the real first sector.
   */

  umount(FF_MNT);
  if (ff_format() < 0)
    {
      printf("fatfault: mkfatfs or mount failed\n");
      goto errout;
    }

  ff_put(FF_MNT "/c", O_WRONLY | O_CREAT | O_TRUNC, 3, 0, 8 * 512);
  ff_remount();
  got = 0;
  g_ffhits = 0;
  fd = open(FF_MNT "/c", O_RDWR);
  if (fd >= 0)
    {
      uint32_t i;

      read(fd, buf, 100);
      g_fffault = data0 + 1;              /* C's second sector */
      lseek(fd, 600, SEEK_SET);
      got = read(fd, buf, 100);
      g_fffault = -1;
      for (i = 0; i < 10; i++)
        {
          buf[i] = ff_byte(9, i);
        }

      lseek(fd, 0, SEEK_SET);
      write(fd, buf, 10);
      close(fd);
    }

  ff_remount();
  badc   = ff_check(FF_MNT "/c", 3, 8 * 512, 9, 10);
  fileok = g_ffhits == 1 && got < 0 && badc == 0;
  printf("fatfault: file cache: %d read(s) failed; afterwards C: %ld wrong "
         "bytes (-1: unreadable): %s\n", g_ffhits, badc,
         fileok ? "PASS" : "FAIL");

  umount(FF_MNT);
  unregister_blockdriver(FF_DEV);
  free(g_ffdisk);
  printf("fatfault: %s\n", fatok && fileok ? "PASS" : "FAIL");
  return fatok && fileok ? 0 : 1;

errout:
  umount(FF_MNT);
  unregister_blockdriver(FF_DEV);
  free(g_ffdisk);
  return 1;
}
#endif

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
  else if (argc >= 3 && strcmp(argv[1], "fat") == 0)
    {
      return cmd_fat(argc, argv);
    }
  else if (argc >= 3 && strcmp(argv[1], "seek") == 0)
    {
      return cmd_seek(argc, argv);
    }
  else if (argc >= 4 && strcmp(argv[1], "stress") == 0)
    {
      return cmd_stress(argc, argv);
    }
#if defined(CONFIG_ARCH_SIM) && defined(CONFIG_FSUTILS_MKFATFS)
  else if (argc >= 2 && strcmp(argv[1], "fatfault") == 0)
    {
      return cmd_fatfault(argc, argv);
    }
#endif

  printf("sdtest crc FILE...\n"
         "sdtest read FILE [KB [sram|psram [MB]]]\n"
         "sdtest write FILE MB [KB [sram|psram]]\n"
         "sdtest seek FILE [BYTES [N [sram|psram [SEED]]]]\n"
         "sdtest fat DIR [OPS [SEED]]\n"
         "sdtest stress DIR MB [SEED [keep|v|keepv]]\n"
         "sdtest fatfault\n");
  return 2;
}
