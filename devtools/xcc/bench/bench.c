/****************************************************************************
 * apps/devtools/xcc/bench/bench.c
 *
 * SPDX-License-Identifier: MIT
 *
 * Small benchmarks for comparing compilers (docs/xcc.md): the same file
 * built by xcc (wcc, xcc) and by gcc/clang.  Each kernel prints its time
 * and a checksum (which must be the same whatever compiled it).
 *
 *   bench [SCALE]       SCALE (default 1) multiplies the work
 *
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Recursion and calls */

static int fib(int n)
{
  return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static unsigned k_fib(int scale)
{
  unsigned s = 0;
  int i;

  for (i = 0; i < scale; i++)
    {
      s += fib(24);
    }

  return s;
}

/* Byte array loops */

static unsigned k_sieve(int scale)
{
  static char flags[8192];
  unsigned count = 0;
  int iter;
  int i;
  int k;

  for (iter = 0; iter < 20 * scale; iter++)
    {
      count = 0;
      memset(flags, 1, sizeof(flags));
      for (i = 2; i < (int)sizeof(flags); i++)
        {
          if (flags[i])
            {
              for (k = i + i; k < (int)sizeof(flags); k += i)
                {
                  flags[k] = 0;
                }

              count++;
            }
        }
    }

  return count;
}

/* Integer matrix multiply (2D arrays, index arithmetic) */

#define N 40

static unsigned k_matmul(int scale)
{
  static int a[N][N];
  static int b[N][N];
  static int c[N][N];
  unsigned s = 0;
  int iter;
  int i;
  int j;
  int k;

  for (i = 0; i < N; i++)
    {
      for (j = 0; j < N; j++)
        {
          a[i][j] = i + j;
          b[i][j] = i - j;
        }
    }

  for (iter = 0; iter < 4 * scale; iter++)
    {
      for (i = 0; i < N; i++)
        {
          for (j = 0; j < N; j++)
            {
              int sum = 0;

              for (k = 0; k < N; k++)
                {
                  sum += a[i][k] * b[k][j];
                }

              c[i][j] = sum;
            }
        }

      s += c[iter % N][(iter * 7) % N];
    }

  return s;
}

/* Single precision floating point */

static unsigned k_float(int scale)
{
  float x = 0.5f;
  float y = 0.25f;
  float acc = 0;
  int i;

  for (i = 0; i < 100000 * scale; i++)
    {
      float t = x * y + 0.125f;

      acc += t / (1.0f + x);
      x = y * 0.999f + 0.001f;
      y = t - (float)(int)t;
    }

  return (unsigned)(acc * 1000);
}

/* Double precision (software on the M33) */

static unsigned k_double(int scale)
{
  double x = 0.5;
  double acc = 0;
  int i;

  for (i = 0; i < 20000 * scale; i++)
    {
      acc += x * x / (1.0 + x);
      x = x * 0.9999 + 0.0001;
    }

  return (unsigned)(acc * 1000);
}

/* 64-bit integers */

static unsigned k_long(int scale)
{
  unsigned long long h = 1469598103934665603ULL;
  int i;

  for (i = 0; i < 100000 * scale; i++)
    {
      h ^= (unsigned long long)i;
      h *= 1099511628211ULL;
      h += h >> 29;
    }

  return (unsigned)(h ^ (h >> 32));
}

/* Structs, pointers and a linked list */

struct node
{
  struct node *next;
  int key;
  int val;
};

static unsigned k_list(int scale)
{
  static struct node nodes[1000];
  unsigned s = 0;
  int iter;
  int i;

  for (i = 0; i < 1000; i++)
    {
      nodes[i].next = &nodes[(i * 337 + 1) % 1000];
      nodes[i].key = i;
      nodes[i].val = i * 3;
    }

  for (iter = 0; iter < 100 * scale; iter++)
    {
      struct node *p = &nodes[iter % 1000];

      for (i = 0; i < 1000; i++)
        {
          s += p->val ^ p->key;
          p = p->next;
        }
    }

  return s;
}

/* Library calls: qsort with a comparison function, string functions */

static int cmp_int(const void *a, const void *b)
{
  int x = *(const int *)a;
  int y = *(const int *)b;
  return x < y ? -1 : x > y;
}

static unsigned k_qsort(int scale)
{
  static int v[2000];
  unsigned s = 0;
  unsigned r = 12345;
  int iter;
  int i;

  for (iter = 0; iter < 2 * scale; iter++)
    {
      for (i = 0; i < 2000; i++)
        {
          r = r * 1103515245 + 12345;
          v[i] = (int)(r >> 8);
        }

      qsort(v, 2000, sizeof(int), cmp_int);
      s += v[iter] ^ v[1999 - iter];
    }

  return s;
}

/* Bit twiddling: CRC-32 */

static unsigned k_crc(int scale)
{
  static unsigned char buf[4096];
  unsigned crc = 0xffffffff;
  int iter;
  int i;
  int k;

  for (i = 0; i < (int)sizeof(buf); i++)
    {
      buf[i] = (unsigned char)(i * 7 + 3);
    }

  for (iter = 0; iter < 4 * scale; iter++)
    {
      for (i = 0; i < (int)sizeof(buf); i++)
        {
          crc ^= buf[i];
          for (k = 0; k < 8; k++)
            {
              crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
            }
        }
    }

  return ~crc;
}

struct kernel
{
  const char *name;
  unsigned (*fn)(int scale);
};

static const struct kernel g_kernels[] =
{
  { "fib",    k_fib },
  { "sieve",  k_sieve },
  { "matmul", k_matmul },
  { "float",  k_float },
  { "double", k_double },
  { "long",   k_long },
  { "list",   k_list },
  { "qsort",  k_qsort },
  { "crc",    k_crc },
};

int main(int argc, char *argv[])
{
  int scale = argc > 1 ? atoi(argv[1]) : 1;
  long long total = 0;
  unsigned i;

  if (scale < 1)
    {
      scale = 1;
    }

  for (i = 0; i < sizeof(g_kernels) / sizeof(g_kernels[0]); i++)
    {
      long long t0 = now_us();
      unsigned r = g_kernels[i].fn(scale);
      long long t = now_us() - t0;

      total += t;
      printf("%-7s %8lld us  %08x\n", g_kernels[i].name, t, r);
    }

  printf("total   %8lld us\n", total);
  return 0;
}
