/* apps/devtools/xcc/tests/opt.c
 *
 * SPDX-License-Identifier: MIT
 *
 * What xcc's optimizations could get wrong (move coalescing, copy and cast
 * forwarding, loop rotation, constant-decided jumps, common
 * subexpressions, loop-invariant code motion, indexed loads and stores,
 * dead definitions, narrowing casts left out, inlining): computations
 * printed, to be the same as a reference compiler's (tests/difftest.sh).
 * No undefined or implementation-defined behavior.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t g_sum;

static void out(const char *name, uint32_t v)
{
  g_sum = g_sum * 31 + v;
  printf("%s %u\n", name, (unsigned)v);
}

/* Narrow types: kept extended in registers, masks left out when they fit */

static uint16_t crcu8(uint8_t data, uint16_t crc)
{
  uint8_t i, x16, carry;

  for (i = 0; i < 8; i++)
    {
      x16 = (uint8_t)((data & 1) ^ ((uint8_t)crc & 1));
      data >>= 1;
      if (x16 == 1)
        {
          crc ^= 0x4002;
          carry = 1;
        }
      else
        {
          carry = 0;
        }

      crc >>= 1;
      if (carry)
        crc |= 0x8000;
      else
        crc &= 0x7fff;
    }

  return crc;
}

static void t_narrow(void)
{
  uint16_t crc = 0;
  int8_t s8 = -100;
  int16_t s16 = -30000;
  uint8_t u8 = 200;
  int i;

  for (i = 0; i < 300; i++)
    crc = crcu8((uint8_t)i, crc);
  out("crc", crc);
  s8 = (int8_t)(s8 - 50);           /* wraps */
  out("s8", (uint32_t)(int32_t)s8);
  s16 = (int16_t)(s16 * 3);
  out("s16", (uint32_t)(int32_t)s16);
  u8 = (uint8_t)(u8 + 100);
  out("u8", u8);
  u8 = (uint8_t)(u8 << 3);
  out("u8shl", u8);
  out("u8ext", (uint32_t)(int32_t)(int8_t)u8);
  out("s8shr", (uint32_t)(s8 >> 2));
  out("u8xor", (uint8_t)(u8 ^ 0x1ff));
  out("u16and", (uint16_t)(crc & 0x1ffff));
  {
    uint8_t a = 0xf0, b = 0x0f;
    uint16_t c = (uint16_t)((a | b) << 8);
    int8_t d = (int8_t)(a & 0x7f);
    out("c", c);
    out("d", (uint32_t)(int32_t)d);
    out("ab", (uint32_t)(a + b));        /* 255, no wrap: int arithmetic */
    out("abu8", (uint8_t)(a + b + 1));   /* wraps */
  }
}

/* Loops: rotation, the first test decided, invariants hoisted */

static int g_arr[64];
static int16_t g_s16[40];

static void t_loops(int n)
{
  int i, j, k;
  uint32_t s = 0;

  for (i = 0; i < 64; i++)
    g_arr[i] = i * 7 - 100;
  for (i = 0; i < n; i++)         /* n may be 0: the entry test */
    s += g_arr[i];
  out("sum", s);
  for (i = 10; i < 5; i++)        /* never */
    s += 1000;
  out("never", s);
  i = 0;
  while (i < 64 && g_arr[i] < 100)
    i++;
  out("while", i);
  k = 0;
  for (i = 0; i < 8; i++)
    {
      for (j = 0; j < 8; j++)
        {
          if (j == i)
            continue;               /* continue: the rotated test */
          if (j > 6)
            break;
          k += g_arr[i * 8 + j] * (i + 1);
        }
    }
  out("nest", (uint32_t)k);
  for (i = 0; i < 40; i++)
    g_s16[i] = (int16_t)(i * -1000);
  s = 0;
  for (i = 39; i >= 0; i--)
    s = s * 3 + (uint32_t)g_s16[i];  /* indexed signed halfword loads */
  out("s16arr", s);
  /* An invariant made inside: n * 3 is not changed in the loop, k is */
  s = 0;
  for (i = 0; i < 10; i++)
    {
      int inv = n * 3 + 1;
      k = k * 2 + inv;
      s += (uint32_t)k;
    }
  out("inv", s);
  /* The loop's bound changes in it */
  s = 0;
  for (i = 0, j = 20; i < j; i++)
    {
      s += i;
      if (i == 5)
        j = 10;
    }
  out("bound", s);
  /* do-while and a condition with a side effect (not rotated) */
  i = 0;
  s = 0;
  do
    s += (uint32_t)i * i;
  while (++i < 9);
  out("dowhile", s);
  i = 0;
  while (g_arr[i++] < 50)
    ;
  out("sidefx", i);
}

/* Common subexpressions with an assignment between them */

static void t_cse(int a, int b)
{
  int x = a * b + 3;
  int y;

  out("cse1", x);
  a = a + 1;
  y = a * b + 3;                    /* not the same as x */
  out("cse2", y);
  y = (a * b + 3) + (a * b + 3);
  out("cse3", y);
  {
    int *p = &g_arr[a];
    int v = p[b] + p[b];
    p[b] = 5;
    out("cse4", v + p[b]);
  }
}

/* Indexed stores and loads, negative and large indices, bytes */

static uint8_t g_buf[300];

static void t_index(int base)
{
  int i;
  uint32_t s = 0;
  uint8_t *p = g_buf + 150;

  for (i = -150; i < 150; i++)
    p[i] = (uint8_t)(i * 3);
  for (i = 0; i < 300; i += 7)
    s = s * 5 + g_buf[i];
  out("idx", s);
  g_arr[base + 3] = 77;
  out("idx2", (uint32_t)g_arr[base + 3] + (uint32_t)g_arr[3 + base]);
  {
    uint16_t h[8];
    for (i = 0; i < 8; i++)
      h[i] = (uint16_t)(0xfff0 + i);
    s = 0;
    for (i = 7; i >= 0; i--)
      s += h[i] * (uint32_t)(i + 1);
    out("idx16", s);
  }
}

/* Moves: a variable assigned from an expression, swaps, parameters */

static uint32_t t_moves(uint32_t a, uint32_t b)
{
  uint32_t t;
  int i;

  for (i = 0; i < 10; i++)
    {
      t = a;
      a = b;
      b = t + b;                    /* fibonacci by swaps */
    }
  out("fibswap", b);
  a = a ^ b;
  b = a ^ b;
  a = a ^ b;
  out("xorswap", a * 3 + b);
  return a - b;
}

/* Inlined small functions with parameters assigned in them */

static int clampadd(int x, int y)
{
  x += y;
  if (x > 1000)
    x = 1000;
  return x;
}

static int64_t mul64(int32_t a, int32_t b)
{
  return (int64_t)a * b;
}

static void t_inline(void)
{
  int i, v = 0;
  int64_t s = 0;

  for (i = 0; i < 50; i++)
    v = clampadd(v, i);
  out("clamp", v);
  for (i = -5; i < 5; i++)
    s += mul64(i * 100000, 70000 - i);
  out("mul64lo", (uint32_t)s);
  out("mul64hi", (uint32_t)((uint64_t)s >> 32));
}

int main(int argc, char *argv[])
{
  (void)argv;
  t_narrow();
  t_loops(argc + 30);
  t_loops(0);
  t_cse(argc + 4, 7);
  t_index(argc);
  out("moves", t_moves(1, 1));
  t_inline();
  printf("total %u\n", (unsigned)g_sum);
  return 0;
}
