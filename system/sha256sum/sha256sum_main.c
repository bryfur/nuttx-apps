/****************************************************************************
 * apps/system/sha256sum/sha256sum_main.c
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

/* SHA-256 of files, through /dev/crypto (the RP2350's accelerator on the
 * board, docs/hardware-accel.md) or, without it, in software
 * (crypto/sha2.c):
 *
 *   sha256sum [-s] FILE...    "HASH  FILE" for each (- or none: stdin);
 *                             -s: in software
 *   sha256sum -c FILE         check the "HASH  NAME" lines of FILE
 *   sha256sum -t [MB [SEED]]  self-test: the NIST examples, messages of
 *                             random lengths up to 1 MB in random pieces
 *                             against software, then two tasks at once
 *                             (and one left idle half way)
 *   sha256sum -b              throughput, /dev/crypto and software
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <crypto/sha2.h>
#ifdef CONFIG_CRYPTO_CRYPTODEV
#  include <crypto/cryptodev.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BUFSIZE   4096
#define MAXLEN    (1024 * 1024)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct hash_s
{
  SHA2_CTX sw;
#ifdef CONFIG_CRYPTO_CRYPTODEV
  struct session_op ses;
  int fd;
#endif
  bool hw;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint32_t g_seed = 1;

/****************************************************************************
 * Private Functions: the hash
 ****************************************************************************/

static void h_init(FAR struct hash_s *h, bool soft)
{
  h->hw = false;
#ifdef CONFIG_CRYPTO_CRYPTODEV
  h->fd = -1;
  if (!soft)
    {
      int fd = open("/dev/crypto", O_RDWR);
      int cfd;

      if (fd >= 0)
        {
          if (ioctl(fd, CRIOGET, &cfd) == 0)
            {
              memset(&h->ses, 0, sizeof(h->ses));
              h->ses.mac = CRYPTO_SHA2_256;
              if (ioctl(cfd, CIOCGSESSION, &h->ses) == 0)
                {
                  h->fd = cfd;
                  h->hw = true;
                }
              else
                {
                  close(cfd);
                }
            }

          close(fd);
        }
    }
#endif

  sha256init(&h->sw);
}

static int h_update(FAR struct hash_s *h, FAR const void *data, size_t len)
{
#ifdef CONFIG_CRYPTO_CRYPTODEV
  if (h->hw)
    {
      struct crypt_op cop;

      memset(&cop, 0, sizeof(cop));
      cop.ses = h->ses.ses;
      cop.op = COP_ENCRYPT;
      cop.flags = COP_FLAG_UPDATE;
      cop.src = (caddr_t)data;
      cop.len = len;
      return ioctl(h->fd, CIOCCRYPT, &cop) < 0 ? -errno : 0;
    }
#endif

  sha256update(&h->sw, data, len);
  return 0;
}

static int h_final(FAR struct hash_s *h, FAR uint8_t *out)
{
#ifdef CONFIG_CRYPTO_CRYPTODEV
  if (h->hw)
    {
      struct crypt_op cop;
      int ret = 0;

      memset(&cop, 0, sizeof(cop));
      cop.ses = h->ses.ses;
      cop.op = COP_ENCRYPT;
      cop.mac = (caddr_t)out;
      if (ioctl(h->fd, CIOCCRYPT, &cop) < 0)
        {
          ret = -errno;
        }

      ioctl(h->fd, CIOCFSESSION, &h->ses.ses);
      close(h->fd);
      h->fd = -1;
      return ret;
    }
#endif

  sha256final(out, &h->sw);
  return 0;
}

static void hex(FAR char *s, FAR const uint8_t *d)
{
  int i;

  for (i = 0; i < 32; i++)
    {
      sprintf(s + 2 * i, "%02x", d[i]);
    }
}

static int hash_file(FAR const char *path, bool soft, FAR uint8_t *out)
{
  struct hash_s h;
  FAR uint8_t *buf;
  int fd;
  int ret = 0;

  fd = strcmp(path, "-") ? open(path, O_RDONLY) : 0;
  if (fd < 0)
    {
      fprintf(stderr, "sha256sum: %s: %s\n", path, strerror(errno));
      return -1;
    }

  buf = malloc(BUFSIZE);
  if (buf == NULL)
    {
      if (fd)
        {
          close(fd);
        }

      return -1;
    }

  h_init(&h, soft);
  for (; ; )
    {
      ssize_t n = read(fd, buf, BUFSIZE);

      if (n < 0)
        {
          fprintf(stderr, "sha256sum: %s: %s\n", path, strerror(errno));
          ret = -1;
          break;
        }

      if (n == 0)
        {
          break;
        }

      if (h_update(&h, buf, n) < 0)
        {
          fprintf(stderr, "sha256sum: %s: /dev/crypto failed\n", path);
          ret = -1;
          break;
        }
    }

  if (h_final(&h, out) < 0)
    {
      ret = -1;
    }

  free(buf);
  if (fd)
    {
      close(fd);
    }

  return ret;
}

static int check(FAR const char *list, bool soft)
{
  char line[300];
  FAR FILE *f = fopen(list, "r");
  int bad = 0;
  int n = 0;

  if (f == NULL)
    {
      fprintf(stderr, "sha256sum: %s: %s\n", list, strerror(errno));
      return 1;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      uint8_t d[32];
      char got[65];
      FAR char *name;
      size_t l = strlen(line);

      while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
        {
          line[--l] = '\0';
        }

      if (l < 66 || line[64] != ' ')
        {
          continue;
        }

      name = line + 65;
      if (*name == ' ' || *name == '*')
        {
          name++;
        }

      line[64] = '\0';
      n++;
      if (hash_file(name, soft, d) < 0)
        {
          printf("%s: FAILED open or read\n", name);
          bad++;
          continue;
        }

      hex(got, d);
      if (strcasecmp(got, line) == 0)
        {
          printf("%s: OK\n", name);
        }
      else
        {
          printf("%s: FAILED\n", name);
          bad++;
        }
    }

  fclose(f);
  if (bad)
    {
      fprintf(stderr, "sha256sum: WARNING: %d of %d computed checksums did"
              " NOT match\n", bad, n);
    }

  return bad != 0;
}

/****************************************************************************
 * Private Functions: tests
 ****************************************************************************/

static uint32_t rnd(void)
{
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return g_seed;
}

/* Hash buf[0..len) in random pieces (hardware unless soft) */

static int hash_pieces(FAR const uint8_t *buf, size_t len, bool soft,
                       FAR uint8_t *out, int idle_ms)
{
  struct hash_s h;
  size_t off = 0;
  int ret = 0;

  h_init(&h, soft);
  while (off < len)
    {
      size_t n;

      switch (rnd() % 4)
        {
          case 0:
            n = rnd() % 70;
            break;

          case 1:
            n = 64 * (rnd() % 8);
            break;

          default:
            n = rnd() % 65536;
            break;
        }

      if (n > len - off)
        {
          n = len - off;
        }

      if (h_update(&h, buf + off, n) < 0)
        {
          ret = -EIO;
        }

      off += n;
      if (idle_ms && off > len / 2)
        {
          usleep(idle_ms * 1000);
          idle_ms = 0;
        }
    }

  if (h_final(&h, out) < 0)
    {
      ret = -EIO;
    }

  return ret;
}

struct kat_s
{
  FAR const char *msg;
  int repeat;
  FAR const char *hash;
};

static const struct kat_s g_kat[] =
{
  { "", 1,
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
  { "abc", 1,
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
  { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1,
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
  { "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
    "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", 1,
    "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1" },
  { "a", 1000000,
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0" },
  { "01234567012345670123456701234567", 20,   /* 640 bytes, 10 blocks */
    "594847328451bdfa85056225462cc1d867d877fb388df0ce35f25ab5562bfbb5" },
};

static int kat(void)
{
  int fails = 0;
  unsigned int i;
  int soft;

  for (soft = 0; soft < 2; soft++)
    {
      for (i = 0; i < sizeof(g_kat) / sizeof(g_kat[0]); i++)
        {
          struct hash_s h;
          uint8_t d[32];
          char s[65];
          int r;

          h_init(&h, soft);
          for (r = 0; r < g_kat[i].repeat; r++)
            {
              h_update(&h, g_kat[i].msg, strlen(g_kat[i].msg));
            }

          h_final(&h, d);
          hex(s, d);
          if (strcmp(s, g_kat[i].hash))
            {
              printf("KAT %u (%s): %s, want %s\n", i,
                     soft ? "software" : "/dev/crypto", s, g_kat[i].hash);
              fails++;
            }
        }
    }

  printf("NIST examples: %s\n", fails ? "FAILED" : "ok");
  return fails;
}

static FAR uint8_t *g_buf;
static volatile int g_tfails;

struct tparam_s
{
  int n;
  uint32_t seed;
  int idle_ms;
};

static void *task_hash(void *arg)
{
  FAR struct tparam_s *p = arg;
  int i;

  for (i = 0; i < p->n; i++)
    {
      size_t len;
      size_t start;
      uint8_t d1[32];
      uint8_t d2[32];
      struct hash_s h;
      int ret;

      sched_lock();
      g_seed = p->seed;
      len = rnd() % (MAXLEN / 4);
      start = rnd() % (MAXLEN - len);
      p->seed = g_seed;
      sched_unlock();

      ret = hash_pieces(g_buf + start, len, false, d1,
                        (i & 1) ? p->idle_ms : 0);
      h_init(&h, true);
      h_update(&h, g_buf + start, len);
      h_final(&h, d2);
      if (ret < 0 || memcmp(d1, d2, 32))
        {
          printf("task: %zu bytes at %zu: differs (%d)\n", len, start, ret);
          g_tfails++;
        }
    }

  return NULL;
}

static int selftest(int n)
{
  int fails = kat();
  struct hash_s h;
  uint8_t d[32];
  size_t i;
  int k;

  h_init(&h, false);
  printf("engine: %s\n", h.hw ? "/dev/crypto" :
         "software (no SHA-256 in /dev/crypto)");
  h_final(&h, d);                   /* (closes the session) */

  g_buf = malloc(MAXLEN);
  if (g_buf == NULL)
    {
      printf("no memory\n");
      return 1;
    }

  for (i = 0; i < MAXLEN; i++)
    {
      g_buf[i] = rnd() >> 7;
    }

  /* Random lengths and pieces, against software */

  {
    int bad = 0;

    for (k = 0; k < n; k++)
      {
        size_t len = k < 130 ? k : rnd() % (MAXLEN + 1);
        size_t start = rnd() % (MAXLEN - len + 1);
        uint8_t d1[32];
        uint8_t d2[32];
        struct hash_s h;

        if (rnd() & 1)
          {
            start = start & ~3;   /* aligned and not */
          }

        if (hash_pieces(g_buf + start, len, false, d1, 0) < 0)
          {
            printf("%zu bytes at %zu: /dev/crypto failed\n", len, start);
            bad++;
            continue;
          }

        h_init(&h, true);
        h_update(&h, g_buf + start, len);
        h_final(&h, d2);
        if (memcmp(d1, d2, 32))
          {
            printf("%zu bytes at %zu: differs from software\n", len, start);
            bad++;
          }
      }

    printf("random messages: %d, %d differ\n", n, bad);
    fails += bad;
  }

  /* Two tasks at once, each with its own session; every other message
   * of the second one waits 100 ms half way (losing the accelerator to
   * the other, which continues in software)
   */

  {
    struct tparam_s p[2] =
      {
        { n / 4 + 1, 12345, 0 },
        { n / 4 + 1, 67890, 100 }
      };

    pthread_attr_t attr;
    pthread_t t[2];

    g_tfails = 0;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);
    pthread_create(&t[0], &attr, task_hash, &p[0]);
    pthread_create(&t[1], &attr, task_hash, &p[1]);
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
    printf("two tasks: %d messages each, %d differ\n", p[0].n, g_tfails);
    fails += g_tfails;
  }

  free(g_buf);
  printf("sha256sum -t: %s\n", fails ? "FAILED" : "all ok");
  return fails != 0;
}

static double now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static double rate(FAR const uint8_t *buf, size_t len, bool soft)
{
  size_t total = 0;
  double t0 = now();
  double t;

  do
    {
      struct hash_s h;
      uint8_t d[32];

      h_init(&h, soft);
      h_update(&h, buf, len);
      h_final(&h, d);
      total += len;
      t = now() - t0;
    }
  while (t < 0.5);

  return total / t / 1e6;
}

/* A message in the SRAM: on this thread's stack (the kernel heap holds
 * the stacks, CONFIG_ARM_STACKS_KERNEL_HEAP; the user heap is the PSRAM)
 */

static void *bench_sram(void *arg)
{
  uint8_t buf[16384];

  memset(buf, 0x5a, sizeof(buf));
  printf("%-7zu B *  %9.2f MB/s %7.2f MB/s\n", sizeof(buf),
         rate(buf, sizeof(buf), false), rate(buf, sizeof(buf), true));
  return NULL;
}

static void bench(void)
{
  static const size_t sizes[] =
    {
      64, 1024, 16384, MAXLEN
    };

  FAR uint8_t *buf = malloc(MAXLEN);
  unsigned int i;
  int soft;

  if (buf == NULL)
    {
      return;
    }

  memset(buf, 0x5a, MAXLEN);
  printf("%-12s %12s %12s\n", "message", "/dev/crypto", "software");
  for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
    {
      double mbs[2];

      for (soft = 0; soft < 2; soft++)
        {
          mbs[soft] = rate(buf, sizes[i], soft);
        }

      printf("%-7zu B    %9.2f MB/s %7.2f MB/s\n", sizes[i], mbs[0],
             mbs[1]);
    }

  free(buf);

  {
    pthread_attr_t attr;
    pthread_t t;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384 + 4096);
    if (pthread_create(&t, &attr, bench_sram, NULL) == 0)
      {
        pthread_join(t, NULL);
        printf("(* in the SRAM; the others in the PSRAM, a new session "
               "each)\n");
      }
  }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  bool soft = false;
  int ret = 0;
  int i = 1;

  if (argc > 1 && !strcmp(argv[1], "-t"))
    {
      if (argc > 3)
        {
          g_seed = strtoul(argv[3], NULL, 0) | 1;
        }

      return selftest(argc > 2 ? atoi(argv[2]) : 200);
    }

  if (argc > 1 && !strcmp(argv[1], "-b"))
    {
      bench();
      return 0;
    }

  if (argc > 1 && !strcmp(argv[1], "-s"))
    {
      soft = true;
      i++;
    }

  if (argc > i + 1 && !strcmp(argv[i], "-c"))
    {
      return check(argv[i + 1], soft);
    }

  if (i >= argc)
    {
      argv[argc] = "-";
      argc++;
    }

  for (; i < argc; i++)
    {
      uint8_t d[32];
      char s[65];

      if (hash_file(argv[i], soft, d) < 0)
        {
          ret = 1;
          continue;
        }

      hex(s, d);
      printf("%s  %s\n", s, argv[i]);
    }

  return ret;
}
