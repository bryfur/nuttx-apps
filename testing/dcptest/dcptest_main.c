/****************************************************************************
 * apps/testing/dcptest/dcptest_main.c
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

/* The RP2350's double-precision coprocessor against libgcc's soft-float
 * (docs/hardware-accel.md):
 *
 *   dcptest [check] [MILLIONS [SEED]]   every helper, random operands of
 *       every class (zeros, denormals, normals, extremes, infinities,
 *       NaNs, few-bit mantissas for exact ties, near operands for
 *       cancellation), the coprocessor's result (dcpt_*, rp23xx_dcp.S
 *       built into this program) and the C operators' (whatever the
 *       program was linked with: the coprocessor's run-time library with
 *       CONFIG_RP23XX_DCP) against libgcc's (soft_*, renamed copies)
 *       bit for bit; also sqrt() (the firmware's).
 *   dcptest bench               cycles per call, soft-float vs coprocessor
 *   dcptest stress [SECONDS [nomask]]   threads on both cores, preempted
 *       by a higher-priority one every tick, all using the coprocessor:
 *       with the masking every result is right; "nomask" (the sequences
 *       without it) shows why it is there.
 *
 * Exit status 0 if nothing differed.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
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

#define B __attribute__((pcs("aapcs")))

#define DEMCR     (*(volatile uint32_t *)0xe000edfc)
#define DWT_CTRL  (*(volatile uint32_t *)0xe0001000)
#define DWT_CYC   (*(volatile uint32_t *)0xe0001004)
#define CPACR     (*(volatile uint32_t *)0xe000ed88)

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef double (*dd_d)(double, double) B;
typedef int (*dd_i)(double, double) B;
typedef double (*d_d)(double);
typedef float (*d_f)(double) B;
typedef double (*f_d)(float) B;
typedef int32_t (*d_i)(double) B;
typedef double (*i_d)(int32_t) B;
typedef double (*l_d)(int64_t) B;

enum kind
{
  K_DD_D, K_DD_I, K_DD_F, K_D_D, K_D_F, K_F_D, K_D_I, K_I_D, K_L_D
};

struct op_s
{
  const char *name;
  enum kind kind;
  void *dcp;           /* rp23xx_dcp.S, built into this program */
  void *soft;          /* libgcc */
  void *c;             /* the C operator, as the program was linked */
  void *nomask;        /* rp23xx_dcp.S without the interrupt masking */
};

/****************************************************************************
 * External Function Prototypes
 ****************************************************************************/

#define DECL2(p, n) \
  extern double p##n(double, double) B;
#define DECLI(p, n) \
  extern int p##n(double, double) B;

#define SETS(X) X(dcpt_) X(soft_) X(dcpn_)

#define DECLALL(p) \
  DECL2(p, dadd) DECL2(p, dsub) DECL2(p, drsub) DECL2(p, dmul) \
  DECL2(p, ddiv) DECLI(p, dcmpeq) DECLI(p, dcmplt) DECLI(p, dcmple) \
  DECLI(p, dcmpge) DECLI(p, dcmpgt) DECLI(p, dcmpun) \
  DECLI(p, gnu_eqdf2) DECLI(p, gnu_gedf2) DECLI(p, gnu_ledf2) \
  DECLI(p, gnu_gtdf2) DECLI(p, gnu_ltdf2) DECLI(p, gnu_nedf2) \
  DECLI(p, gnu_cmpdf2) \
  extern double p##i2d(int32_t) B; extern double p##ui2d(uint32_t) B; \
  extern double p##l2d(int64_t) B; extern double p##ul2d(uint64_t) B; \
  extern double p##f2d(float) B; extern float p##d2f(double) B; \
  extern int32_t p##d2iz(double) B; extern uint32_t p##d2uiz(double) B; \
  extern int p##cdcmple(double, double) B; \
  extern int p##cdcmpeq(double, double) B; \
  extern int p##cdrcmple(double, double) B; \
  extern double p##__ieee754_sqrt(double);

SETS(DECLALL)

/* dcptest_flags.S: calls a flag-setting comparison, returns its NZCV in
 * bits 31:28 and bit 0 set if it changed r0-r3
 */

extern uint32_t dcpt_callflags(void *fn, double a, double b) B;
extern double dcpt_null2(double a, double b) B;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;

/****************************************************************************
 * Private Functions: the C operators
 ****************************************************************************/

static double B c_add(double a, double b) { return a + b; }
static double B c_sub(double a, double b) { return a - b; }
static double B c_mul(double a, double b) { return a * b; }
static double B c_div(double a, double b) { return a / b; }
static int B c_eq(double a, double b) { return a == b; }
static int B c_lt(double a, double b) { return a < b; }
static int B c_le(double a, double b) { return a <= b; }
static int B c_ge(double a, double b) { return a >= b; }
static int B c_gt(double a, double b) { return a > b; }
static int B c_un(double a, double b) { return __builtin_isunordered(a, b); }
static double B c_i2d(int32_t i) { return i; }
static double B c_ui2d(uint32_t i) { return i; }
static double B c_l2d(int64_t i) { return i; }
static double B c_ul2d(uint64_t i) { return i; }
static double B c_f2d(float f) { return f; }
static float B c_d2f(double d) { return d; }
static int32_t B c_d2iz(double d) { return d; }
static uint32_t B c_d2uiz(double d) { return d; }
static double c_sqrt(double d) { return sqrt(d); }

/* Optimisation must not see through these: the operators above must call
 * the helpers.
 */

#define F(x) ((void *)(x))

static const struct op_s g_ops[] =
{
  { "dadd",   K_DD_D, F(dcpt_dadd),  F(soft_dadd),  F(c_add),  F(dcpn_dadd) },
  { "dsub",   K_DD_D, F(dcpt_dsub),  F(soft_dsub),  F(c_sub),  F(dcpn_dsub) },
  { "drsub",  K_DD_D, F(dcpt_drsub), F(soft_drsub), NULL,      F(dcpn_drsub) },
  { "dmul",   K_DD_D, F(dcpt_dmul),  F(soft_dmul),  F(c_mul),  F(dcpn_dmul) },
  { "ddiv",   K_DD_D, F(dcpt_ddiv),  F(soft_ddiv),  F(c_div),  F(dcpn_ddiv) },
  { "sqrt",   K_D_D,  F(dcpt___ieee754_sqrt), F(soft___ieee754_sqrt),
                      F(c_sqrt), F(dcpn___ieee754_sqrt) },
  { "dcmpeq", K_DD_I, F(dcpt_dcmpeq), F(soft_dcmpeq), F(c_eq), F(dcpn_dcmpeq) },
  { "dcmplt", K_DD_I, F(dcpt_dcmplt), F(soft_dcmplt), F(c_lt), F(dcpn_dcmplt) },
  { "dcmple", K_DD_I, F(dcpt_dcmple), F(soft_dcmple), F(c_le), F(dcpn_dcmple) },
  { "dcmpge", K_DD_I, F(dcpt_dcmpge), F(soft_dcmpge), F(c_ge), F(dcpn_dcmpge) },
  { "dcmpgt", K_DD_I, F(dcpt_dcmpgt), F(soft_dcmpgt), F(c_gt), F(dcpn_dcmpgt) },
  { "dcmpun", K_DD_I, F(dcpt_dcmpun), F(soft_dcmpun), F(c_un), F(dcpn_dcmpun) },
  { "eqdf2",  K_DD_I, F(dcpt_gnu_eqdf2), F(soft_gnu_eqdf2), NULL, NULL },
  { "nedf2",  K_DD_I, F(dcpt_gnu_nedf2), F(soft_gnu_nedf2), NULL, NULL },
  { "cmpdf2", K_DD_I, F(dcpt_gnu_cmpdf2), F(soft_gnu_cmpdf2), NULL, NULL },
  { "ltdf2",  K_DD_I, F(dcpt_gnu_ltdf2), F(soft_gnu_ltdf2), NULL, NULL },
  { "ledf2",  K_DD_I, F(dcpt_gnu_ledf2), F(soft_gnu_ledf2), NULL, NULL },
  { "gedf2",  K_DD_I, F(dcpt_gnu_gedf2), F(soft_gnu_gedf2), NULL, NULL },
  { "gtdf2",  K_DD_I, F(dcpt_gnu_gtdf2), F(soft_gnu_gtdf2), NULL, NULL },
  { "cdcmple",  K_DD_F, F(dcpt_cdcmple),  F(soft_cdcmple),  NULL, NULL },
  { "cdcmpeq",  K_DD_F, F(dcpt_cdcmpeq),  F(soft_cdcmpeq),  NULL, NULL },
  { "cdrcmple", K_DD_F, F(dcpt_cdrcmple), F(soft_cdrcmple), NULL, NULL },
  { "d2f",    K_D_F,  F(dcpt_d2f),   F(soft_d2f),   F(c_d2f),  F(dcpn_d2f) },
  { "f2d",    K_F_D,  F(dcpt_f2d),   F(soft_f2d),   F(c_f2d),  F(dcpn_f2d) },
  { "d2iz",   K_D_I,  F(dcpt_d2iz),  F(soft_d2iz),  F(c_d2iz), F(dcpn_d2iz) },
  { "d2uiz",  K_D_I,  F(dcpt_d2uiz), F(soft_d2uiz), F(c_d2uiz), F(dcpn_d2uiz) },
  { "i2d",    K_I_D,  F(dcpt_i2d),   F(soft_i2d),   F(c_i2d),  F(dcpn_i2d) },
  { "ui2d",   K_I_D,  F(dcpt_ui2d),  F(soft_ui2d),  F(c_ui2d), F(dcpn_ui2d) },
  { "l2d",    K_L_D,  F(dcpt_l2d),   F(soft_l2d),   F(c_l2d),  F(dcpn_l2d) },
  { "ul2d",   K_L_D,  F(dcpt_ul2d),  F(soft_ul2d),  F(c_ul2d), F(dcpn_ul2d) },
};

#define NOPS (sizeof(g_ops) / sizeof(g_ops[0]))

/****************************************************************************
 * Private Functions: operands
 ****************************************************************************/

static inline uint64_t rnd(void)
{
  uint64_t x = g_rng;

  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  g_rng = x;
  return x * 0x2545f4914f6cdd1dull;
}

static inline double d_of(uint64_t b)
{
  double d;

  memcpy(&d, &b, 8);
  return d;
}

static inline uint64_t b_of(double d)
{
  uint64_t b;

  memcpy(&b, &d, 8);
  return b;
}

static inline uint32_t fb_of(float f)
{
  uint32_t b;

  memcpy(&b, &f, 4);
  return b;
}

static inline float f_of(uint32_t b)
{
  float f;

  memcpy(&f, &b, 4);
  return f;
}

static uint64_t mk(uint64_t sign, uint64_t exp, uint64_t man)
{
  return (sign << 63) | ((exp & 0x7ff) << 52) | (man & 0xfffffffffffffull);
}

/* A mantissa: random, or with few significant bits (exact results and
 * ties), or all ones, or zero
 */

static uint64_t man(void)
{
  uint64_t r = rnd();

  switch (r & 7)
    {
      case 0:
      case 1:
        {
          int k = (r >> 3) % 28;
          return (rnd() >> (64 - k)) << (52 - k);
        }

      case 2:
        return 0xfffffffffffffull >> ((r >> 3) % 4);

      case 3:
        return (r >> 8) & 0x3;   /* 0..3: near powers of two */

      default:
        return rnd();
    }
}

/* A double of any class */

static uint64_t gen(void)
{
  uint64_t r = rnd();
  uint64_t s = (r >> 8) & 1;
  int c = r % 100;

  if (c < 4)
    {
      return mk(s, 0, 0);                         /* zero */
    }
  else if (c < 12)
    {
      return mk(s, 0, man() | ((r >> 9) & 1));    /* denormal */
    }
  else if (c < 15)
    {
      return mk(s, 0x7ff, 0);                     /* infinity */
    }
  else if (c < 20)
    {
      uint64_t m = rnd() & 0xfffffffffffffull;    /* NaN, quiet or not */
      return mk(s, 0x7ff, m ? m : 1);
    }
  else if (c < 30)
    {
      uint64_t e = (r >> 16) % 6;                 /* near the extremes */
      return mk(s, (r >> 20) & 1 ? 0x7fe - e : 1 + e, man());
    }
  else if (c < 40)
    {
      return rnd();                               /* any bits */
    }
  else if (c < 50)
    {
      /* integers and near them */

      return mk(s, 1023 + (r >> 16) % 66, man());
    }
  else
    {
      return mk(s, 1023 - 60 + (r >> 16) % 120, man());
    }
}

/* A second operand: unrelated, or near the first (cancellation, the
 * alignment shifts around 53, equal, negated)
 */

static uint64_t gen2(uint64_t a)
{
  uint64_t r = rnd();
  int c = r % 100;
  uint64_t ea = (a >> 52) & 0x7ff;

  if (c < 45)
    {
      return gen();
    }
  else if (c < 70)
    {
      int64_t d = (int64_t)((r >> 8) % 120) - 60;
      int64_t e = (int64_t)ea + d;

      if (e < 0)
        {
          e = 0;
        }

      if (e > 0x7fe)
        {
          e = 0x7fe;
        }

      return mk((r >> 20) & 1, e, man());
    }
  else if (c < 80)
    {
      return a + (int64_t)((r >> 8) % 9) - 4;      /* a few ulps away */
    }
  else if (c < 85)
    {
      return a ^ (1ull << 63);                    /* -a */
    }
  else if (c < 90)
    {
      return a;
    }
  else
    {
      return (a ^ ((r >> 30) & 1ull) << 63) + ((int64_t)((r >> 8) % 5) - 2);
    }
}

/* Doubles for conversions to integers: around the int32 range */

static uint64_t gen_toint(void)
{
  uint64_t r = rnd();

  if ((r & 3) == 0)
    {
      return gen();
    }

  return mk((r >> 8) & 1, 1023 - 2 + (r >> 16) % 36, man());
}

/* Doubles for conversions to float: around the float range */

static uint64_t gen_tofloat(void)
{
  uint64_t r = rnd();
  uint64_t m;

  if ((r & 3) == 0)
    {
      return gen();
    }

  /* Mantissas with ties at float precision (29 bits dropped) */

  m = man();
  if ((r >> 20) & 1)
    {
      m = (m & ~0x1fffffffull) | ((r >> 21) & 1 ? 0x10000000 : 0);
      m |= (r >> 22) & 1;
    }

  return mk((r >> 8) & 1, 1023 - 160 + (r >> 16) % 300, m);
}

static uint32_t gen_float(void)
{
  uint64_t r = rnd();
  uint32_t s = (r >> 8) & 1;
  int c = r % 10;
  uint32_t m = rnd() & 0x7fffff;

  switch (c)
    {
      case 0: return s << 31;
      case 1: return (s << 31) | (m ? m : 1);              /* denormal */
      case 2: return (s << 31) | 0x7f800000;
      case 3: return (s << 31) | 0x7f800000 | (m ? m : 1); /* NaN */
      default: return (uint32_t)rnd();
    }
}

static int64_t gen_long(void)
{
  uint64_t r = rnd();
  int k = r % 65;
  uint64_t v = k ? rnd() >> (64 - k) : 0;

  if ((r >> 8) & 1)
    {
      v = (v & ~0x3ffull) | ((r >> 9) & 1 ? 0x200 : 0) | ((r >> 10) & 1);
    }

  return (int64_t)v;
}

/****************************************************************************
 * Private Functions: checking
 ****************************************************************************/

struct res_s
{
  uint64_t r;     /* result bits */
  uint32_t flags;
};

static struct res_s call(const struct op_s *op, void *fn, uint64_t a,
                         uint64_t b)
{
  struct res_s res = { 0, 0 };
  double da = d_of(a);
  double db = d_of(b);

  switch (op->kind)
    {
      case K_DD_D: res.r = b_of(((dd_d)fn)(da, db)); break;
      case K_DD_I: res.r = (uint32_t)((dd_i)fn)(da, db); break;
      case K_DD_F:
        /* Z and C (the ABI's); libgcc's cdrcmple changes r0-r3, which
         * the coprocessor's must not (checked below)
         */

        res.flags = dcpt_callflags(fn, da, db);
        res.r = res.flags & 0x60000000;
        break;
      case K_D_D:  res.r = b_of(((d_d)fn)(da)); break;
      case K_D_F:  res.r = fb_of(((d_f)fn)(da)); break;
      case K_F_D:  res.r = b_of(((f_d)fn)(f_of((uint32_t)a))); break;
      case K_D_I:  res.r = (uint32_t)((d_i)fn)(da); break;
      case K_I_D:  res.r = b_of(((i_d)fn)((int32_t)a)); break;
      case K_L_D:  res.r = b_of(((l_d)fn)((int64_t)a)); break;
    }

  return res;
}

static void operands(const struct op_s *op, uint64_t *a, uint64_t *b)
{
  switch (op->kind)
    {
      case K_DD_D:
      case K_DD_I:
      case K_DD_F:
        *a = gen();
        *b = gen2(*a);
        break;
      case K_D_D: *a = gen(); break;
      case K_D_F: *a = gen_tofloat(); break;
      case K_F_D: *a = gen_float(); break;
      case K_D_I: *a = gen_toint(); break;
      case K_I_D: *a = (uint32_t)gen_long(); break;
      case K_L_D: *a = gen_long(); break;
    }
}

static const char *cls(uint64_t x)
{
  uint64_t e = (x >> 52) & 0x7ff;
  uint64_t m = x & 0xfffffffffffffull;

  if (e == 0)
    {
      return m ? "denorm" : "zero";
    }

  if (e == 0x7ff)
    {
      return m ? "nan" : "inf";
    }

  return "normal";
}

static int check(unsigned long n)
{
  int fails = 0;
  unsigned int i;

  for (i = 0; i < NOPS; i++)
    {
      const struct op_s *op = &g_ops[i];
      unsigned long bad = 0;
      unsigned long cbad = 0;
      unsigned long j;
      int shown = 0;

      for (j = 0; j < n; j++)
        {
          uint64_t a = 0;
          uint64_t b = 0;
          struct res_s rs;
          struct res_s rd;

          operands(op, &a, &b);
          rs = call(op, op->soft, a, b);
          rd = call(op, op->dcp, a, b);
          if (rs.r != rd.r || (op->kind == K_DD_F && (rd.flags & 1)))
            {
              bad++;
              if (shown++ < 6)
                {
                  printf("  %s %016" PRIx64 " (%s) %016" PRIx64
                         " (%s): dcp %016" PRIx64 " soft %016" PRIx64 "\n",
                         op->name, a, cls(a), b, cls(b), rd.r, rs.r);
                }
            }

          if (op->c != NULL)
            {
              struct res_s rc = call(op, op->c, a, b);

              /* sqrt(): newlib's wrapper returns __ieee754_sqrt's result
               * but for a negative number (EDOM)
               */

              if (op->kind == K_D_D && (a >> 63) && (a << 1) != 0 &&
                  (a << 1) <= 0xffe0000000000000ull)
                {
                  rc = rs;
                }

              if (rc.r != rs.r)
                {
                  cbad++;
                  if (shown++ < 6)
                    {
                      printf("  C %s %016" PRIx64 " %016" PRIx64
                             ": C %016" PRIx64 " soft %016" PRIx64 "\n",
                             op->name, a, b, rc.r, rs.r);
                    }
                }
            }
        }

      printf("%-9s %lu: %lu differ%s", op->name, n, bad,
             op->c ? "" : "\n");
      if (op->c)
        {
          printf(", C operator %lu\n", cbad);
        }

      fflush(stdout);
      fails += bad + cbad != 0;
    }

  return fails;
}

/****************************************************************************
 * Private Functions: timing
 ****************************************************************************/

static void cyc_init(void)
{
  DEMCR |= 1u << 24;  /* TRCENA */
  DWT_CYC = 0;
  DWT_CTRL |= 1;
}

#define NB 256

static void bench(void)
{
  static uint64_t a[NB];
  static uint64_t b[NB];
  cpu_set_t set;
  unsigned int i;
  int j;

  CPU_ZERO(&set);
  CPU_SET(0, &set);
  sched_setaffinity(0, sizeof(set), &set);
  usleep(2000);
  cyc_init();

  printf("cycles per call (300 MHz), call overhead included\n");
  printf("%-9s %8s %8s %8s %7s\n", "op", "soft", "dcp", "C op", "speedup");

  for (i = 0; i < NOPS; i++)
    {
      const struct op_s *op = &g_ops[i];
      void *fns[3] =
        {
          op->soft, op->dcp, op->c
        };

      uint32_t cyc[3] =
        {
          0, 0, 0
        };

      int k;

      /* Normal operands (the common case) */

      for (j = 0; j < NB; j++)
        {
          do
            {
              operands(op, &a[j], &b[j]);
            }
          while (op->kind < K_D_F && (!strcmp(cls(a[j]), "normal") == 0 ||
                 (op->kind != K_D_D && strcmp(cls(b[j]), "normal") != 0) ||
                 (op->kind == K_D_D && (a[j] >> 63))));
        }

      for (k = 0; k < 3; k++)
        {
          uint32_t best = UINT32_MAX;
          int rep;

          if (fns[k] == NULL)
            {
              continue;
            }

          for (rep = 0; rep < 5; rep++)
            {
              uint32_t t0;
              uint32_t t;

              t0 = DWT_CYC;
              for (j = 0; j < NB; j++)
                {
                  call(op, fns[k], a[j], b[j]);
                }

              t = DWT_CYC - t0;
              if (t < best)
                {
                  best = t;
                }
            }

          cyc[k] = best;
        }

      /* The harness's own cost: a call to a function returning its first
       * argument
       */

      {
        uint32_t best = UINT32_MAX;
        int rep;
        struct op_s nop = *op;

        nop.kind = K_DD_D;
        for (rep = 0; rep < 5; rep++)
          {
            uint32_t t0 = DWT_CYC;
            uint32_t t;

            for (j = 0; j < NB; j++)
              {
                call(&nop, (void *)dcpt_null2, a[j], b[j]);
              }

            t = DWT_CYC - t0;
            if (t < best)
              {
                best = t;
              }
          }

        for (k = 0; k < 3; k++)
          {
            if (cyc[k])
              {
                cyc[k] = cyc[k] > best ? cyc[k] - best : 0;
              }
          }
      }

      printf("%-9s %8.1f %8.1f %8.1f %6.1fx\n", op->name,
             cyc[0] / (float)NB, cyc[1] / (float)NB, cyc[2] / (float)NB,
             cyc[1] ? (float)cyc[0] / cyc[1] : 0.0f);
    }
}

/****************************************************************************
 * Private Functions: stress
 ****************************************************************************/

#define NS 512

struct case_s
{
  uint8_t op;
  uint64_t a;
  uint64_t b;
  uint64_t r;
};

static struct case_s g_cases[NS];
static volatile bool g_stop;
static volatile unsigned long g_errors;
static volatile unsigned long g_done;
static bool g_nomask;

static const uint8_t g_stressops[] =
{
  0, 1, 3, 4, 5, 22, 24, 26, 28
};

static void *worker(void *arg)
{
  unsigned long n = 0;
  unsigned long err = 0;
  int period = (int)(intptr_t)arg;

  while (!g_stop)
    {
      int i;

      for (i = 0; i < NS; i++)
        {
          const struct case_s *c = &g_cases[i];
          const struct op_s *op = &g_ops[c->op];
          struct res_s r = call(op, g_nomask ? op->nomask : op->dcp,
                                c->a, c->b);

          err += r.r != c->r;
          n++;
        }

      if (period)
        {
          usleep(period);
        }
    }

  sched_lock();
  g_errors += err;
  g_done += n;
  sched_unlock();
  return NULL;
}

static int stress(int seconds, bool nomask)
{
  pthread_t th[5];
  pthread_attr_t attr;
  struct sched_param sp;
  int i;

  g_nomask = nomask;
  g_stop = false;
  g_errors = 0;
  g_done = 0;

  for (i = 0; i < NS; i++)
    {
      struct case_s *c = &g_cases[i];
      const struct op_s *op;

      c->op = g_stressops[rnd() % sizeof(g_stressops)];
      op = &g_ops[c->op];
      do
        {
          operands(op, &c->a, &c->b);
        }
      while (op->kind == K_D_D && (c->a >> 63));

      c->r = call(op, op->soft, c->a, c->b).r;
    }

  /* Four workers at one priority (more threads than cores, so they move
   * between cores) and one above them waking every tick
   */

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  for (i = 0; i < 5; i++)
    {
      sp.sched_priority = i < 4 ? 90 : 110;
      pthread_attr_setschedparam(&attr, &sp);
      pthread_attr_setschedpolicy(&attr, SCHED_RR);
      pthread_create(&th[i], &attr, worker,
                     (void *)(intptr_t)(i < 4 ? 0 : 1000));
    }

  sleep(seconds);
  g_stop = true;
  for (i = 0; i < 5; i++)
    {
      pthread_join(th[i], NULL);
    }

  printf("stress%s: %lu operations in %d s on both cores, %lu wrong\n",
         nomask ? " (no masking)" : "", g_done, seconds, g_errors);
  return g_errors != 0;
}

/****************************************************************************
 * Private Functions: probe
 ****************************************************************************/

/* Where the coprocessor's addition is wrong: random pairs by exponent
 * difference, the raw sequence (dcpn_dadd/dsub without the checks would
 * still take the software path for tiny operands: these are mid-range)
 */

static void probe(unsigned long n)
{
  static unsigned long bad[70];
  static unsigned long tot[70];
  unsigned long i;
  int shown = 0;
  int d;

  for (i = 0; i < n; i++)
    {
      uint64_t r = rnd();
      int dd = r % 66;
      uint64_t ea = 1023 + (int)((r >> 8) % 200) - 100;
      uint64_t a = mk((r >> 20) & 1, ea, man());
      uint64_t b = mk((r >> 21) & 1, ea - dd, man());
      uint64_t x;
      uint64_t y;

      if ((r >> 22) & 1)
        {
          uint64_t t = a;

          a = b;
          b = t;
        }

      x = b_of(dcpt_dadd(d_of(a), d_of(b)));
      y = b_of(soft_dadd(d_of(a), d_of(b)));
      tot[dd]++;
      if (x != y)
        {
          bad[dd]++;
          if (shown++ < 12)
            {
              printf("  %016" PRIx64 " + %016" PRIx64 ": %016" PRIx64
                     " want %016" PRIx64 "\n", a, b, x, y);
            }
        }
    }

  for (d = 0; d < 66; d++)
    {
      if (bad[d])
        {
          printf("exponent difference %2d: %lu of %lu wrong\n", d, bad[d],
                 tot[d]);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  const char *cmd = argc > 1 ? argv[1] : "check";
  int i;

  /* Coprocessor 4 on both cores (the firmware does it with
   * CONFIG_RP23XX_DCP; done here too so this runs on any firmware)
   */

  for (i = 0; i < CONFIG_SMP_NCPUS; i++)
    {
      cpu_set_t set;

      CPU_ZERO(&set);
      CPU_SET(i, &set);
      sched_setaffinity(0, sizeof(set), &set);
      usleep(1000);
      CPACR |= 3u << 8;
      __asm__ volatile ("dsb\n isb\n mrc p4, #0, r0, c0, c0, #1"
                        ::: "r0", "memory");
    }

  {
    cpu_set_t set;

    CPU_ZERO(&set);
    for (i = 0; i < CONFIG_SMP_NCPUS; i++)
      {
        CPU_SET(i, &set);
      }

    sched_setaffinity(0, sizeof(set), &set);
  }

  if (!strcmp(cmd, "bench"))
    {
      bench();
      return 0;
    }

  if (!strcmp(cmd, "probe"))
    {
      probe(argc > 2 ? strtoul(argv[2], NULL, 0) : 1000000);
      return 0;
    }

  if (!strcmp(cmd, "stress"))
    {
      return stress(argc > 2 ? atoi(argv[2]) : 10,
                    argc > 3 && !strcmp(argv[3], "nomask"));
    }

  if (!strcmp(cmd, "check"))
    {
      argc--;
      argv++;
    }

  {
    unsigned long n = argc > 1 ? strtoul(argv[1], NULL, 0) : 1;
    int fails;

    if (argc > 2)
      {
        g_rng = strtoull(argv[2], NULL, 0) | 1;
      }

    fails = check(n * 1000000ul);
    printf("dcptest: %s\n", fails ? "FAILED" : "all bit-exact");
    return fails != 0;
  }
}
