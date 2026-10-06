/* apps/devtools/xcc/tests/opt.c
 *
 * SPDX-License-Identifier: MIT
 *
 * What xcc's optimizations could get wrong (move coalescing, copy and cast
 * forwarding, loop rotation, constant-decided jumps, common
 * subexpressions, loop-invariant code motion, indexed loads and stores,
 * dead definitions, narrowing casts left out, inlining, conditions as
 * jumps, range checks, jump threading, loads known again): computations
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

/* Conditions: 0/1 diamonds, & and | of comparisons as jumps, range checks
 * (signed, unsigned, narrow, at the bounds) */

static uint8_t isdig(uint8_t c)
{
  uint8_t r;
  r = ((c >= '0') & (c <= '9')) ? 1 : 0;
  return r;
}

static int inrange_s(int x)
{
  return x >= -5 && x <= 7;
}

static int outrange_u(unsigned x)
{
  if (x < 10 || x > 20)
    return 1;
  return 0;
}

static int either(int a, int b)
{
  return ((a > 3) | (b < -2)) ? 11 : 22;
}

static int notboth(int8_t a, int16_t b)
{
  if ((a == 1) & (b != 2))
    return 5;
  return 6;
}

static void t_conds(void)
{
  uint32_t s = 0;
  for (int i = -300; i < 300; i++)
    {
      s = s * 3 + isdig((uint8_t)i) + 2 * inrange_s(i) + 4 * outrange_u((unsigned)i);
      s += either(i % 9, i % 7) + notboth((int8_t)i, (int16_t)(i / 3));
      int8_t c = (int8_t)i;
      if (c >= -120 && c <= -100)
        s ^= 0x1234;
      if ((uint16_t)i > 65000 || (uint16_t)i < 3)
        s += 77;
    }
  out("conds", s);
  out("lim", inrange_s(-2147483647 - 1) + 2 * inrange_s(2147483647) +
             4 * outrange_u(4294967295u) + 8 * outrange_u(0));
}

/* A state machine in a loop (jump threading) */

static int scan(const char *p, int *counts)
{
  int state = 0, n = 0;
  for (; *p && state != 9; p++)
    {
      switch (state)
        {
        case 0:
          if (*p >= '0' && *p <= '9')
            state = 1;
          else if (*p == '-')
            state = 2;
          else
            state = 9;
          counts[0]++;
          break;
        case 1:
          if (*p == '.')
            state = 3;
          else if (!(*p >= '0' && *p <= '9'))
            state = 9;
          counts[1]++;
          break;
        case 2:
          state = (*p >= '0' && *p <= '9') ? 1 : 9;
          counts[2]++;
          break;
        default:
          if (!(*p >= '0' && *p <= '9'))
            state = 9;
          counts[3]++;
          break;
        }
      n++;
    }
  return state * 100 + n;
}

static void t_thread(void)
{
  static const char *in[] = {"123", "-4.5", "7.x", "--1", "", "0.25e", "-", "99.9"};
  int counts[4] = {0, 0, 0, 0};
  uint32_t s = 0;
  for (int i = 0; i < 8; i++)
    s = s * 7 + scan(in[i], counts);
  out("scan", s);
  out("counts", counts[0] + 10 * counts[1] + 100 * counts[2] + 1000 * counts[3]);
}

/* Loads known again in a block and the blocks after it (not across a
 * store, a call, or a change of the address); base + offset used twice */

struct node
{
  struct node *next;
  int val;
};

static struct node g_nodes[8];

static int walk(struct node *p, int stop)
{
  int s = 0;
  while (p != NULL && p->val != stop)
    {
      s += p->val;
      p = p->next;
      if (p != NULL && p->next != NULL)
        s += p->next->val;
    }
  return s;
}

static int alias(int *a, int *b)
{
  int x = *a;
  *b = x + 1;
  return x + *a;  /* a may be b */
}

static void bump(int *c)
{
  c[2]++;
  c[3] += c[2];
}

static void t_loads(void)
{
  for (int i = 0; i < 8; i++)
    {
      g_nodes[i].next = i < 7 ? &g_nodes[i + 1] : NULL;
      g_nodes[i].val = i * i + 1;
    }
  int c[4] = {1, 2, 3, 4};
  int v = 5;
  bump(c);
  bump(c);
  out("walk", walk(&g_nodes[0], 26) + 1000 * walk(&g_nodes[2], -1));
  out("alias", alias(&v, &v) * 100 + alias(&c[0], &c[1]));
  out("bump", c[2] * 100 + c[3]);
}

/* Multiply-accumulate and bit fields (ARM32: mla, mls, ubfx) */

static int16_t g_ma[16] = {3, -7, 120, -32768, 32767, 5, -1, 0, 9, -300, 77, 1000, -2, 4, 8, 16};

static void t_fuse(void)
{
  int32_t acc = 0, dif = 1000000;
  uint32_t bits = 0;
  for (int i = 0; i < 16; i++)
    {
      int32_t p = (int32_t)g_ma[i] * g_ma[15 - i];
      acc += (int32_t)g_ma[i] * g_ma[(i + 3) & 15];
      dif -= p;
      bits += ((p >> 2) & 15) * ((p >> 5) & 127);
      bits ^= ((uint32_t)p >> 28) & 0xf;
      bits += (p >> 30) & 7;
      bits += ((int32_t)(uint32_t)p >> 31) & 1;
    }
  out("mla", (uint32_t)acc);
  out("mls", (uint32_t)dif);
  out("ubfx", bits);
}

/* Shifted operands (narrow results too), -(x & 1), (float)(int)f, constants
 * of a loop in registers */

static void t_shift(void)
{
  uint32_t s = 0;
  uint8_t b8 = 0;
  int16_t h = 0;
  float f = 0.3f, acc = 0;
  for (int i = 0; i < 300; i++)
    {
      uint32_t x = (uint32_t)i * 2654435761u;
      s += x >> 7;
      s ^= (x << 3) | (s >> 29);
      s -= (uint32_t)((int32_t)x >> 5);
      b8 = (uint8_t)(b8 | (i << 4));
      h = (int16_t)(h + (i << 9));
      s += (uint32_t)(-(int32_t)(x & 1)) & 0x1234567u;
      s ^= 0xedb88320u & -(s & 1);
      acc += f * 1.5f - (float)(int)(f * 7.25f);
      f = f * 1.0001f + 0.37f;
      if (f > 100.0f)
        f -= 99.5f;
    }
  out("shift", s);
  out("narrow", b8 * 65536u + (uint16_t)h);
  out("float", (uint32_t)(int32_t)(acc * 100.0f));
}

/* A state machine whose states stay (threading on a test's edge, with the
 * value switched on known in its case), offset and repeated cases */

enum st { ST_A = 3, ST_B, ST_C, ST_D, ST_E, ST_X = 11 };

static int scan2(const unsigned char *p, int *cnt)
{
  enum st s = ST_A;
  int n = 0;
  for (; *p; p++, n++)
    {
      switch (s)
        {
        case ST_A:
          if (*p >= 'a' && *p <= 'z')
            break;
          s = *p == '_' ? ST_B : ST_C;
          cnt[0]++;
          break;
        case ST_B:
        case ST_D:
          if (*p == '_')
            break;
          s = (*p & 1) ? ST_D : ST_E;
          cnt[1]++;
          break;
        case ST_C:
          if (*p >= '0' && *p <= '9')
            s = ST_A;
          cnt[2]++;
          break;
        case ST_E:
          s = *p == 'q' ? ST_X : ST_A;
          break;
        default:
          cnt[3]++;
          if (*p == '!')
            return n;
          break;
        }
    }
  return n * 16 + (int)s;
}

static void t_thread2(void)
{
  static const char *in[] = {"abc_def", "x__y1z", "q!r", "hello_world_42", "a1b2c3", "__", "eqz!w", "zz9_q"};
  int cnt[4] = {0, 0, 0, 0};
  uint32_t s = 0;
  for (int i = 0; i < 8; i++)
    s = s * 13 + scan2((const unsigned char *)in[i], cnt);
  out("scan2", s);
  out("cnt2", cnt[0] + 10 * cnt[1] + 100 * cnt[2] + 1000 * cnt[3]);
}

/* A load in a loop known from its first test before it (no store in the
 * loop), and one that is not (a store to the same place in the loop) */

struct key
{
  int16_t pad;
  int16_t idx;
};

static struct node *find(struct node *p, const struct key *k)
{
  while (p != NULL && p->val != k->idx)
    p = p->next;
  return p;
}

static int find_bump(struct node *p, struct key *k)
{
  int n = 0;
  while (p != NULL && p->val != k->idx)
    {
      k->idx++;  /* changes what the test loads */
      p = p->next;
      n++;
    }
  return n * 100 + k->idx;
}

static void t_loopload(void)
{
  struct key k = {0, 26};
  struct node *f = find(&g_nodes[0], &k);
  out("find", f != NULL ? (uint32_t)(f - g_nodes) : 99);
  k.idx = 3;
  out("findbump", find_bump(&g_nodes[0], &k));
  k.idx = 1000;
  out("findnone", find(&g_nodes[0], &k) == NULL);
}

/* Addresses stepped by pointers of their own (loop strength reduction,
 * post-indexed accesses): steps up and down, scaled, stores too, an access
 * after the step (not reduced) */

static int16_t g_sr16[64];
static uint8_t g_sr8[200];
static int32_t g_sr32[64];

static void t_loopsr(int n)
{
  uint32_t s = 0;
  for (int i = 0; i < 64; i++)
    g_sr16[i] = (int16_t)(i * 37 - 900);
  for (int i = 0; i < n; i++)
    s += (uint32_t)g_sr16[i] * 3u;
  for (int i = n - 1; i >= 0; i--)
    s = s * 5 + (uint32_t)g_sr16[i];
  for (int i = 0; i < n / 3; i++)
    g_sr8[i * 3 + 1] = (uint8_t)(i ^ s);
  for (int i = 0; i < n; i++)
    {
      g_sr32[i] = g_sr16[63 - i] + i;
      s ^= g_sr8[i * 3 + 1];
    }
  for (int i = 0; i < n; )
    {
      i += 2;
      s += (uint32_t)g_sr32[i - 1];  /* after the step */
    }
  out("loopsr", s);
}

/* Narrow loop counters (no cast when they can't wrap), bit operations
 * simplified ((a & m) ^ (b & m), x & m with x within m, b == 1) */

static void t_narrowloop(void)
{
  uint32_t s = 0;
  for (uint8_t i = 0; i < 200; i += 3)
    s = s * 3 + i;
  for (uint16_t i = 7; i <= 65000; i += 1000)
    s ^= i;
  int guard = 0;
  for (uint8_t i = 250; i < 255 && guard < 100; i += 4, guard++)  /* wraps */
    s += i;
  for (uint8_t a = 0, b = 0; a < 40; a++, b += 7)
    {
      uint8_t x = (uint8_t)((a & 1) ^ (b & 1));
      if (x == 1)
        s += 5;
      uint16_t h = (uint16_t)(s >> 1) & 0x7fff;
      s ^= h | ((a | b) & 6);
    }
  out("narrowloop", s);
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
  t_conds();
  t_thread();
  t_loads();
  t_fuse();
  t_shift();
  t_thread2();
  t_loopload();
  t_loopsr(argc + 59);
  t_narrowloop();
  printf("total %u\n", (unsigned)g_sum);
  return 0;
}
