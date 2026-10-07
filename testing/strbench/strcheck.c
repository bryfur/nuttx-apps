/****************************************************************************
 * apps/testing/strbench/strcheck.c
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

/* Each function against a byte-at-a-time reference: every length 0 to
 * 260 and some up to 4097, every alignment 0 to 7 of each pointer,
 * overlaps both ways (memmove), bytes 0x01, 0x7f, 0x80, 0xff around the
 * end of a string or the first difference, fill values past a byte
 * (memset, memchr), and the bytes around the destination untouched.
 * Uses nothing of the C library but printf, so that it can check that
 * library's own functions.
 */

#include <stdint.h>
#include <stdio.h>

#include "strcheck.h"

#define GUARD   64
#define MAXN    4097
#define BUFSZ   (MAXN + 4 * GUARD + 16)
#define MAXFAIL 5

/* The references must stay byte loops (not calls to the functions they
 * check)
 */

#define REF __attribute__((noinline, optimize("no-tree-loop-distribute-patterns")))

static unsigned char g_a[BUFSZ] __attribute__((aligned(16)));
static unsigned char g_r[BUFSZ] __attribute__((aligned(16)));
static unsigned char g_s[BUFSZ] __attribute__((aligned(16)));

static const size_t g_big[] =
{
  300, 511, 512, 513, 1000, 1023, 1024, 1025, 4095, 4096, 4097
};

#define NBIG (sizeof(g_big) / sizeof(g_big[0]))

static const unsigned char g_odd[] =
{
  0x01, 0x7f, 0x80, 0xff, 0x41, 0xfe
};

#define NODD (sizeof(g_odd) / sizeof(g_odd[0]))

static int g_fail;
static int g_ffail;

/* Lengths: 0..260, then the big ones */

static size_t len_at(int i)
{
  return i <= 260 ? (size_t)i : g_big[i - 261];
}

#define NLEN (261 + (int)NBIG)

REF static void fill(unsigned char *p, size_t n, unsigned seed)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      p[i] = (unsigned char)(i * 7 + seed * 13 + (i >> 8));
    }
}

/* Non-zero bytes, all 255 values */

REF static void fillnz(unsigned char *p, size_t n, unsigned seed)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      p[i] = (unsigned char)(1 + (i * 37 + seed) % 255);
    }
}

REF static void copy(unsigned char *d, const unsigned char *s, size_t n)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      d[i] = s[i];
    }
}

REF static void move(unsigned char *d, const unsigned char *s, size_t n)
{
  size_t i;

  if (d < s)
    {
      for (i = 0; i < n; i++)
        {
          d[i] = s[i];
        }
    }
  else
    {
      for (i = n; i-- > 0; )
        {
          d[i] = s[i];
        }
    }
}

REF static int same(const unsigned char *a, const unsigned char *b,
                    size_t n)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      if (a[i] != b[i])
        {
          return 0;
        }
    }

  return 1;
}

static void fail(const char *fn, const char *what, size_t n, int o1, int o2,
                 long x)
{
  g_fail++;
  if (g_ffail++ < MAXFAIL)
    {
      printf("FAIL %s: %s n=%u off=%d,%d (%ld)\n", fn, what, (unsigned)n,
             o1, o2, x);
    }
}

static void done(const char *fn, long cases)
{
  printf("%-8s %7ld cases, %d failed\n", fn, cases, g_ffail);
  g_ffail = 0;
}

static void check_memset(const struct strcheck_fns *f)
{
  static const int vals[] =
  {
    0, 0xff, 0x5a, 0x1a5, -1, -128
  };

  long cases = 0;
  int li;
  int o;
  int v;

  for (li = 0; li < NLEN; li++)
    {
      size_t n = len_at(li);

      for (o = 0; o < 8; o++)
        {
          for (v = 0; v < 6; v++)
            {
              unsigned char *d = g_a + GUARD + o;
              size_t w = n + 2 * GUARD;
              size_t i;
              void *ret;

              if (n > 260 && v > 1)
                {
                  break;
                }

              fill(g_a, w + 8, li + o + v);
              copy(g_r, g_a, w + 8);
              for (i = 0; i < n; i++)
                {
                  g_r[GUARD + o + i] = (unsigned char)vals[v];
                }

              ret = f->memset(d, vals[v], n);
              cases++;
              if (ret != d)
                {
                  fail("memset", "return", n, o, v, 0);
                }

              if (!same(g_a, g_r, w + 8))
                {
                  fail("memset", "bytes", n, o, v, vals[v]);
                }
            }
        }
    }

  done("memset", cases);
}

static void check_memcpy(const struct strcheck_fns *f, int move_)
{
  const char *fn = move_ ? "memmove" : "memcpy";
  long cases = 0;
  int li;
  int od;
  int os;

  for (li = 0; li < NLEN; li++)
    {
      size_t n = len_at(li);

      for (od = 0; od < 8; od++)
        {
          for (os = 0; os < 8; os++)
            {
              unsigned char *d = g_a + GUARD + od;
              unsigned char *s = g_s + GUARD + os;
              size_t w = n + 2 * GUARD + 8;
              void *ret;

              if (n > 260 && ((od ^ os) & 1) != 0 && od > 1)
                {
                  continue;
                }

              fill(g_a, w, li * 3 + od);
              fill(g_s, w, li * 5 + os + 77);
              copy(g_r, g_a, w);
              copy(g_r + GUARD + od, s, n);

              ret = move_ ? f->memmove(d, s, n) : f->memcpy(d, s, n);
              cases++;
              if (ret != d)
                {
                  fail(fn, "return", n, od, os, 0);
                }

              if (!same(g_a, g_r, w))
                {
                  fail(fn, "bytes", n, od, os, 0);
                }
            }
        }
    }

  done(fn, cases);
}

/* memmove within one buffer: the destination DELTA bytes after (or
 * before) the source
 */

static void check_overlap(const struct strcheck_fns *f)
{
  static const int bigd[] =
  {
    -65, -64, -5, -4, -3, -1, 1, 3, 4, 5, 64, 65
  };

  long cases = 0;
  int li;
  int os;
  int di;

  for (li = 0; li < NLEN; li++)
    {
      size_t n = len_at(li);
      int nd = n > 260 ? 12 : 129;

      for (os = 0; os < 8; os++)
        {
          for (di = 0; di < nd; di++)
            {
              int delta = n > 260 ? bigd[di] : di - 64;
              unsigned char *s = g_a + 2 * GUARD + os;
              unsigned char *d = s + delta;
              size_t w = n + 4 * GUARD + 8;
              void *ret;

              if (delta == 0)
                {
                  continue;
                }

              fill(g_a, w, li + os * 11 + di);
              copy(g_r, g_a, w);
              move(g_r + 2 * GUARD + os + delta, g_r + 2 * GUARD + os, n);

              ret = f->memmove(d, s, n);
              cases++;
              if (ret != d)
                {
                  fail("memmove", "overlap return", n, os, delta, 0);
                }

              if (!same(g_a, g_r, w))
                {
                  fail("memmove", "overlap bytes", n, os, delta, 0);
                }
            }
        }
    }

  done("memmove~", cases);
}

static void check_strlen(const struct strcheck_fns *f)
{
  long cases = 0;
  int li;
  int o;
  int k;

  for (li = 0; li < NLEN; li++)
    {
      size_t n = len_at(li);

      for (o = 0; o < 16; o++)
        {
          for (k = 0; k < (int)NODD + 1; k++)
            {
              unsigned char *s = g_s + GUARD + o;
              size_t r;

              fillnz(g_s, n + 2 * GUARD + 16, li + o + k);
              if (k > 0 && n > 0)
                {
                  /* An odd byte just before the end, and right after it */

                  s[n - 1] = g_odd[k - 1];
                  s[n + 1] = g_odd[NODD - k];
                }

              s[n] = 0;
              r = f->strlen((const char *)s);
              cases++;
              if (r != n)
                {
                  fail("strlen", "length", n, o, k, (long)r);
                }
            }
        }
    }

  done("strlen", cases);
}

static int sign(int x)
{
  return x < 0 ? -1 : x > 0;
}

static void check_strcmp(const struct strcheck_fns *f)
{
  long cases = 0;
  int li;
  int oa;
  int ob;

  for (li = 0; li < NLEN; li++)
    {
      size_t n = len_at(li);

      if (n > 80 && n != 260 && n != 300 && n != 1025)
        {
          continue;
        }

      for (oa = 0; oa < 8; oa++)
        {
          for (ob = 0; ob < 8; ob++)
            {
              unsigned char *a = g_a + GUARD + oa;
              unsigned char *b = g_s + GUARD + ob;
              size_t step = n > 40 ? n / 7 + 1 : 1;
              size_t k;
              int r;

              /* Equal */

              fillnz(a, n, li);
              copy(b, a, n);
              a[n] = 0;
              b[n] = 0;
              a[n + 1] = 0x41;
              b[n + 1] = 0x42;
              r = f->strcmp((const char *)a, (const char *)b);
              cases++;
              if (r != 0)
                {
                  fail("strcmp", "equal", n, oa, ob, r);
                }

              /* First difference at K (K == n: one string longer) */

              for (k = 0; k <= n; k += (k + step > n && k < n) ?
                                       n - k : step)
                {
                  int p = (int)(k + oa * 8 + ob + li) % (int)NODD;
                  int q = (p + 1 + (int)k % ((int)NODD - 1)) % (int)NODD;
                  int x = g_odd[p];
                  int y = k == n ? 0 : g_odd[q];
                  unsigned char sa = a[k];
                  unsigned char sb = b[k];
                  int want;

                  if (k < n && x == y)
                    {
                      y = 0x02;
                    }

                  /* the bytes after the end can be anything */

                  a[k] = (unsigned char)x;
                  b[k] = (unsigned char)y;
                  if (k == n)
                    {
                      a[k + 1] = 0;
                    }

                  want = x - y;
                  r = f->strcmp((const char *)a, (const char *)b);
                  cases++;
                  if (sign(r) != sign(want))
                    {
                      fail("strcmp", "order", n, oa, ob, (long)k);
                    }

                  r = f->strcmp((const char *)b, (const char *)a);
                  cases++;
                  if (sign(r) != -sign(want))
                    {
                      fail("strcmp", "order swapped", n, oa, ob, (long)k);
                    }

                  a[k] = sa;
                  b[k] = sb;
                  if (k == n)
                    {
                      a[k + 1] = 0x41;
                    }

                  if (k == n)
                    {
                      break;
                    }
                }
            }
        }
    }

  done("strcmp", cases);
}

static void check_strcpy(const struct strcheck_fns *f)
{
  long cases = 0;
  int li;
  int od;
  int os;

  for (li = 0; li < NLEN; li++)
    {
      size_t n = len_at(li);

      for (od = 0; od < 8; od++)
        {
          for (os = 0; os < 8; os++)
            {
              unsigned char *d = g_a + GUARD + od;
              unsigned char *s = g_s + GUARD + os;
              size_t w = n + 2 * GUARD + 8;
              char *ret;

              fill(g_a, w, li + od);
              fillnz(g_s, w, li * 3 + os);
              if (n > 0)
                {
                  s[n - 1] = g_odd[(od + os) % NODD];
                }

              s[n] = 0;
              copy(g_r, g_a, w);
              copy(g_r + GUARD + od, s, n + 1);

              ret = f->strcpy((char *)d, (const char *)s);
              cases++;
              if (ret != (char *)d)
                {
                  fail("strcpy", "return", n, od, os, 0);
                }

              if (!same(g_a, g_r, w))
                {
                  fail("strcpy", "bytes", n, od, os, 0);
                }
            }
        }
    }

  done("strcpy", cases);
}

static void check_memchr(const struct strcheck_fns *f)
{
  static const int targets[] =
  {
    0, 0x80, 0xff, 0x41, 0x01
  };

  long cases = 0;
  int li;
  int o;
  int t;

  for (li = 0; li < NLEN; li++)
    {
      size_t n = len_at(li);

      for (o = 0; o < 8; o++)
        {
          for (t = 0; t < 5; t++)
            {
              unsigned char *s = g_s + GUARD + o;
              int c = targets[t];
              size_t w = n + 2 * GUARD + 8;
              size_t where[6];
              int nw = 0;
              int i;
              size_t j;

              /* Not found, then found at 0, 1, the middle, the last */

              where[nw++] = (size_t)-1;
              if (n > 0)
                {
                  where[nw++] = 0;
                  where[nw++] = n - 1;
                }

              if (n > 1)
                {
                  where[nw++] = 1;
                  where[nw++] = n / 2;
                }

              for (i = 0; i < nw; i++)
                {
                  const unsigned char *want;
                  void *r;
                  int cv;

                  /* Everything but the target, the target right after */

                  fill(g_s, w, li + o + t + i);
                  for (j = 0; j < w; j++)
                    {
                      if (g_s[j] == (unsigned char)c)
                        {
                          g_s[j] ^= 0x10;
                        }
                    }

                  s[n] = (unsigned char)c;
                  want = NULL;
                  if (where[i] != (size_t)-1)
                    {
                      s[where[i]] = (unsigned char)c;
                      want = s + where[i];
                    }

                  /* c as is, with bits past the byte, negative */

                  cv = (i % 3) == 0 ? c : (i % 3) == 1 ? c + 0x300 : c - 256;
                  r = f->memchr(s, cv, n);
                  cases++;
                  if (r != (void *)want)
                    {
                      fail("memchr", "pointer", n, o, c,
                           r ? (long)((unsigned char *)r - s) : -1L);
                    }
                }
            }
        }
    }

  done("memchr", cases);
}

int strcheck(const struct strcheck_fns *f)
{
  g_fail = 0;
  if (f->memset)
    {
      check_memset(f);
    }

  if (f->memcpy)
    {
      check_memcpy(f, 0);
    }

  if (f->memmove)
    {
      check_memcpy(f, 1);
      check_overlap(f);
    }

  if (f->memchr)
    {
      check_memchr(f);
    }

  if (f->strlen)
    {
      check_strlen(f);
    }

  if (f->strcmp)
    {
      check_strcmp(f);
    }

  if (f->strcpy)
    {
      check_strcpy(f);
    }

  return g_fail;
}
