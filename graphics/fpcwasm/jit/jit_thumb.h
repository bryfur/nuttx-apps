/****************************************************************************
 * apps/graphics/fpcwasm/jit/jit_thumb.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Thumb-2 (ARMv8-M Mainline, FPv5 single precision) instruction encoders
 * for the WebAssembly JIT (wasm_jit.c).  Only what the JIT emits: no
 * double precision VFP (the Cortex-M33 has none), no LDRD/STRD/LDM/VLDR
 * on addresses that may be unaligned (linear memory).
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCWASM_JIT_JIT_THUMB_H
#define __APPS_GRAPHICS_FPCWASM_JIT_JIT_THUMB_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/****************************************************************************
 * Registers, conditions
 ****************************************************************************/

#define R0   0
#define R1   1
#define R2   2
#define R3   3
#define R4   4
#define R9   9
#define R10  10
#define R11  11
#define R_IP 12
#define R_SP 13
#define R_LR 14
#define R_PC 15

enum
{
  CC_EQ, CC_NE, CC_HS, CC_LO, CC_MI, CC_PL, CC_VS, CC_VC,
  CC_HI, CC_LS, CC_GE, CC_LT, CC_GT, CC_LE, CC_AL
};

#define CC_INV(c) ((c) ^ 1)

/* Data processing opcodes (modified immediate and shifted register) */

enum
{
  DP_AND = 0, DP_BIC = 1, DP_ORR = 2, DP_ORN = 3, DP_EOR = 4,
  DP_ADD = 8, DP_ADC = 10, DP_SBC = 11, DP_SUB = 13, DP_RSB = 14
};

enum
{
  SH_LSL = 0, SH_LSR = 1, SH_ASR = 2, SH_ROR = 3
};

/* Loads and stores */

enum
{
  LS_STR, LS_STRB, LS_STRH, LS_LDR, LS_LDRB, LS_LDRH, LS_LDRSB, LS_LDRSH
};

/****************************************************************************
 * The code buffer
 ****************************************************************************/

typedef struct jbuf_s
{
  uint8_t *buf;
  uint32_t len;                     /* Bytes emitted */
  uint32_t cap;
  bool oom;
} jbuf_t;

void *jit_realloc(void *p, uint32_t size);

static inline bool jb_grow(jbuf_t *b, uint32_t n)
{
  if (b->len + n > b->cap)
    {
      uint32_t cap = b->cap ? b->cap * 2 : 4096;
      uint8_t *p;

      while (cap < b->len + n)
        {
          cap *= 2;
        }

      p = jit_realloc(b->buf, cap);
      if (p == NULL)
        {
          b->oom = true;
          b->len = 0;               /* Keep writing somewhere harmless */
          return b->cap >= n;
        }

      b->buf = p;
      b->cap = cap;
    }

  return true;
}

static inline void e16(jbuf_t *b, uint32_t hw)
{
  if (b->len + 2 > b->cap && !jb_grow(b, 2))
    {
      return;
    }

  b->buf[b->len]     = (uint8_t)hw;
  b->buf[b->len + 1] = (uint8_t)(hw >> 8);
  b->len += 2;
}

static inline void e32(jbuf_t *b, uint32_t hw1, uint32_t hw2)
{
  e16(b, hw1);
  e16(b, hw2);
}

static inline uint32_t jb_get16(jbuf_t *b, uint32_t pos)
{
  return b->buf[pos] | (b->buf[pos + 1] << 8);
}

static inline void jb_put16(jbuf_t *b, uint32_t pos, uint32_t hw)
{
  b->buf[pos]     = (uint8_t)hw;
  b->buf[pos + 1] = (uint8_t)(hw >> 8);
}

/****************************************************************************
 * Immediates
 ****************************************************************************/

/* The modified immediate (i:imm3:imm8) of v, or -1 */

static inline int t_modimm(uint32_t v)
{
  uint32_t b = v & 0xff;
  int rot;

  if (v < 256)
    {
      return (int)v;
    }

  if (v == (b | b << 16))
    {
      return 0x100 | b;
    }

  if (v == (((v >> 8) & 0xff) << 8 | ((v >> 8) & 0xff) << 24))
    {
      return 0x200 | ((v >> 8) & 0xff);
    }

  if (v == (b | b << 8 | b << 16 | b << 24))
    {
      return 0x300 | b;
    }

  for (rot = 8; rot < 32; rot++)
    {
      uint32_t x = (v << rot) | (v >> (32 - rot));

      if (x < 256 && (x & 0x80) != 0)
        {
          return (rot << 7) | (x & 0x7f);
        }
    }

  return -1;
}

/****************************************************************************
 * Data processing
 ****************************************************************************/

static inline bool t_lo(int r)
{
  return r < 8;
}

/* op{s} rd, rn, #modimm (enc from t_modimm) */

static inline void t_dp_imm(jbuf_t *b, int op, int s, int rd, int rn,
                            int enc)
{
  e32(b, 0xf000 | ((enc >> 11) & 1) << 10 | op << 5 | s << 4 | rn,
      ((enc >> 8) & 7) << 12 | rd << 8 | (enc & 0xff));
}

/* op{s} rd, rn, rm, shift #amt */

static inline void t_dp_reg(jbuf_t *b, int op, int s, int rd, int rn, int rm,
                            int sh, int amt)
{
  e32(b, 0xea00 | op << 5 | s << 4 | rn,
      (amt >> 2) << 12 | rd << 8 | (amt & 3) << 6 | sh << 4 | rm);
}

/* mov rd, rm (no flags) */

static inline void t_mov(jbuf_t *b, int rd, int rm)
{
  if (rd != rm)
    {
      e16(b, 0x4600 | (rd & 8) << 4 | rm << 3 | (rd & 7));
    }
}

/* movw rd, #imm16; movt rd, #imm16 */

static inline void t_movw(jbuf_t *b, int rd, uint32_t v)
{
  e32(b, 0xf240 | ((v >> 11) & 1) << 10 | ((v >> 12) & 0xf),
      ((v >> 8) & 7) << 12 | rd << 8 | (v & 0xff));
}

static inline void t_movt(jbuf_t *b, int rd, uint32_t v)
{
  e32(b, 0xf2c0 | ((v >> 11) & 1) << 10 | ((v >> 12) & 0xf),
      ((v >> 8) & 7) << 12 | rd << 8 | (v & 0xff));
}

/* rd = v, flags untouched */

static inline void t_movi(jbuf_t *b, int rd, uint32_t v)
{
  int enc = t_modimm(v);

  if (enc >= 0)
    {
      t_dp_imm(b, DP_ORR, 0, rd, 15, enc);           /* mov.w */
      return;
    }

  enc = t_modimm(~v);
  if (enc >= 0)
    {
      t_dp_imm(b, DP_ORN, 0, rd, 15, enc);           /* mvn.w */
      return;
    }

  t_movw(b, rd, v & 0xffff);
  if ((v >> 16) != 0)
    {
      t_movt(b, rd, v >> 16);
    }
}

/* rd = rn + v (no flags); may use ip when rn != ip and rd != ip */

static inline void t_addi(jbuf_t *b, int rd, int rn, int32_t v)
{
  int enc;

  if (v == 0)
    {
      t_mov(b, rd, rn);
      return;
    }

  if (v > 0 && v < 4096)
    {
      e32(b, 0xf200 | ((v >> 11) & 1) << 10 | rn,          /* addw */
          ((v >> 8) & 7) << 12 | rd << 8 | (v & 0xff));
      return;
    }

  if (v < 0 && v > -4096)
    {
      uint32_t n = (uint32_t)-v;

      e32(b, 0xf2a0 | ((n >> 11) & 1) << 10 | rn,          /* subw */
          ((n >> 8) & 7) << 12 | rd << 8 | (n & 0xff));
      return;
    }

  enc = t_modimm((uint32_t)v);
  if (enc >= 0)
    {
      t_dp_imm(b, DP_ADD, 0, rd, rn, enc);
      return;
    }

  enc = t_modimm((uint32_t)-v);
  if (enc >= 0)
    {
      t_dp_imm(b, DP_SUB, 0, rd, rn, enc);
      return;
    }

  t_movi(b, R_IP, (uint32_t)v);
  t_dp_reg(b, DP_ADD, 0, rd, rn, R_IP, SH_LSL, 0);
}

/* Shifts by an immediate (1..31) */

static inline void t_shi(jbuf_t *b, int sh, int rd, int rm, int amt)
{
  t_dp_reg(b, DP_ORR, 0, rd, 15, rm, sh, amt);           /* mov.w rd, rm, sh */
}

/* Shifts by a register (no flags) */

static inline void t_shr(jbuf_t *b, int sh, int rd, int rn, int rm)
{
  e32(b, 0xfa00 | sh << 5 | rn, 0xf000 | rd << 8 | rm);
}

/* cmp rn, rm / cmp rn, #enc / cmn rn, #enc */

static inline void t_cmp(jbuf_t *b, int rn, int rm)
{
  if (t_lo(rn) && t_lo(rm))
    {
      e16(b, 0x4280 | rm << 3 | rn);
    }
  else
    {
      e16(b, 0x4500 | (rn & 8) << 4 | rm << 3 | (rn & 7));
    }
}

static inline void t_cmpi_enc(jbuf_t *b, int rn, int enc)
{
  if (t_lo(rn) && enc < 256)
    {
      e16(b, 0x2800 | rn << 8 | enc);
    }
  else
    {
      t_dp_imm(b, DP_SUB, 1, 15, rn, enc);
    }
}

static inline void t_cmni_enc(jbuf_t *b, int rn, int enc)
{
  t_dp_imm(b, DP_ADD, 1, 15, rn, enc);
}

/* tst rn, rm */

static inline void t_tst(jbuf_t *b, int rn, int rm)
{
  t_dp_reg(b, DP_AND, 1, 15, rn, rm, SH_LSL, 0);
}

/* mul, mla, mls, umull, smull, sdiv, udiv */

static inline void t_mul(jbuf_t *b, int rd, int rn, int rm)
{
  e32(b, 0xfb00 | rn, 0xf000 | rd << 8 | rm);
}

static inline void t_mla(jbuf_t *b, int rd, int rn, int rm, int ra)
{
  e32(b, 0xfb00 | rn, ra << 12 | rd << 8 | rm);
}

static inline void t_mls(jbuf_t *b, int rd, int rn, int rm, int ra)
{
  e32(b, 0xfb00 | rn, ra << 12 | rd << 8 | 0x10 | rm);
}

static inline void t_umull(jbuf_t *b, int rdlo, int rdhi, int rn, int rm)
{
  e32(b, 0xfba0 | rn, rdlo << 12 | rdhi << 8 | rm);
}

static inline void t_sdiv(jbuf_t *b, int rd, int rn, int rm)
{
  e32(b, 0xfb90 | rn, 0xf0f0 | rd << 8 | rm);
}

static inline void t_udiv(jbuf_t *b, int rd, int rn, int rm)
{
  e32(b, 0xfbb0 | rn, 0xf0f0 | rd << 8 | rm);
}

static inline void t_clz(jbuf_t *b, int rd, int rm)
{
  e32(b, 0xfab0 | rm, 0xf080 | rd << 8 | rm);
}

static inline void t_rbit(jbuf_t *b, int rd, int rm)
{
  e32(b, 0xfa90 | rm, 0xf0a0 | rd << 8 | rm);
}

/* sxtb, sxth, uxtb, uxth */

static inline void t_sxtb(jbuf_t *b, int rd, int rm)
{
  e32(b, 0xfa4f, 0xf080 | rd << 8 | rm);
}

static inline void t_sxth(jbuf_t *b, int rd, int rm)
{
  e32(b, 0xfa0f, 0xf080 | rd << 8 | rm);
}

static inline void t_uxtb(jbuf_t *b, int rd, int rm)
{
  e32(b, 0xfa5f, 0xf080 | rd << 8 | rm);
}

static inline void t_uxth(jbuf_t *b, int rd, int rm)
{
  e32(b, 0xfa1f, 0xf080 | rd << 8 | rm);
}

/* it{x{y{z}}} cond: pattern "t", "te", "tt", "tte"... (first is always t) */

static inline void t_it(jbuf_t *b, int cond, const char *pat)
{
  int n = (int)strlen(pat);
  int mask = 0;
  int i;

  for (i = 1; i < n; i++)
    {
      int bit = pat[i] == 't' ? (cond & 1) : !(cond & 1);

      mask |= bit << (4 - i);
    }

  mask |= 1 << (4 - n);
  e16(b, 0xbf00 | cond << 4 | mask);
}

/* mov rd, #small inside an IT block (no flags inside IT) or outside where
 * flags do not matter
 */

static inline void t_movi_it(jbuf_t *b, int rd, uint32_t v)
{
  if (t_lo(rd) && v < 256)
    {
      e16(b, 0x2000 | rd << 8 | v);
    }
  else
    {
      t_movi(b, rd, v);
    }
}

/****************************************************************************
 * Loads, stores
 ****************************************************************************/

static const uint16_t g_ls_t3[] =          /* imm12 forms (Rn in low bits) */
{
  0xf8c0, 0xf880, 0xf8a0, 0xf8d0, 0xf890, 0xf8b0, 0xf990, 0xf9b0
};

static const uint16_t g_ls_t4[] =          /* imm8 and register forms */
{
  0xf840, 0xf800, 0xf820, 0xf850, 0xf810, 0xf830, 0xf910, 0xf930
};

/* op rt, [rn, #imm] (ip used for offsets out of range: rt, rn != ip then) */

static inline void t_ldst(jbuf_t *b, int op, int rt, int rn, int32_t imm)
{
  if (t_lo(rt) && t_lo(rn) && imm >= 0)
    {
      static const int16_t t1[] =
      {
        0x6000, 0x7000, 0x8000, 0x6800, 0x7800, 0x8800, -1, -1
      };

      static const uint8_t sc[] =
      {
        2, 0, 1, 2, 0, 1, 0, 0
      };

      if (t1[op] >= 0 && (imm & ((1 << sc[op]) - 1)) == 0 &&
          (imm >> sc[op]) < 32)
        {
          e16(b, t1[op] | (imm >> sc[op]) << 6 | rn << 3 | rt);
          return;
        }
    }

  if (rn == R_SP && t_lo(rt) && (op == LS_LDR || op == LS_STR) &&
      imm >= 0 && imm < 1024 && (imm & 3) == 0)
    {
      e16(b, (op == LS_LDR ? 0x9800 : 0x9000) | rt << 8 | imm >> 2);
      return;
    }

  if (imm >= 0 && imm < 4096)
    {
      e32(b, g_ls_t3[op] | rn, rt << 12 | imm);
      return;
    }

  if (imm < 0 && imm > -256)
    {
      e32(b, g_ls_t4[op] | rn, rt << 12 | 0xc00 | (-imm));
      return;
    }

  t_movi(b, R_IP, (uint32_t)imm);
  e32(b, g_ls_t4[op] | rn, rt << 12 | R_IP);
}

/* op rt, [rn, rm, lsl #sh] */

static inline void t_ldst_reg(jbuf_t *b, int op, int rt, int rn, int rm,
                              int sh)
{
  if (sh == 0 && t_lo(rt) && t_lo(rn) && t_lo(rm))
    {
      static const uint16_t t1[] =
      {
        0x5000, 0x5400, 0x5200, 0x5800, 0x5c00, 0x5a00, 0x5600, 0x5e00
      };

      e16(b, t1[op] | rm << 6 | rn << 3 | rt);
      return;
    }

  e32(b, g_ls_t4[op] | rn, rt << 12 | sh << 4 | rm);
}

/* push/pop of a register list (bit n = rn) */

static inline void t_push(jbuf_t *b, uint32_t list)
{
  if ((list & ~0x40ffu) == 0)
    {
      e16(b, 0xb400 | ((list >> 14) & 1) << 8 | (list & 0xff));
    }
  else if ((list & (list - 1)) == 0)
    {
      /* One high register (PUSH.W wants two): str r, [sp, #-4]! */

      e32(b, 0xf84d, __builtin_ctz(list) << 12 | 0xd04);
    }
  else
    {
      e32(b, 0xe92d, list);
    }
}

static inline void t_pop(jbuf_t *b, uint32_t list)
{
  if ((list & ~0x80ffu) == 0)
    {
      e16(b, 0xbc00 | ((list >> 15) & 1) << 8 | (list & 0xff));
    }
  else if ((list & (list - 1)) == 0)
    {
      /* ldr r, [sp], #4 */

      e32(b, 0xf85d, __builtin_ctz(list) << 12 | 0xb04);
    }
  else
    {
      e32(b, 0xe8bd, list);
    }
}

/* sp += imm / sp -= imm (multiples of 4) */

static inline void t_sp_add(jbuf_t *b, int32_t imm)
{
  if (imm == 0)
    {
      return;
    }

  if (imm > 0 && imm < 512)
    {
      e16(b, 0xb000 | imm >> 2);
    }
  else if (imm < 0 && imm > -512)
    {
      e16(b, 0xb080 | (-imm) >> 2);
    }
  else
    {
      t_addi(b, R_SP, R_SP, imm);
    }
}

/****************************************************************************
 * Branches (offsets are patched by the JIT: placeholders here)
 ****************************************************************************/

/* B.W / BL / Bcc.W to a byte offset from the instruction */

static inline void t_enc_b(uint32_t *hw1, uint32_t *hw2, int32_t off, bool bl)
{
  uint32_t imm = (uint32_t)(off - 4) >> 1;
  uint32_t s   = ((uint32_t)(off - 4) >> 31) & 1;
  uint32_t i1  = (imm >> 22) & 1;
  uint32_t i2  = (imm >> 21) & 1;
  uint32_t j1  = (!(i1 ^ s)) & 1;
  uint32_t j2  = (!(i2 ^ s)) & 1;

  *hw1 = 0xf000 | s << 10 | ((imm >> 11) & 0x3ff);
  *hw2 = (bl ? 0xd000 : 0x9000) | j1 << 13 | j2 << 11 | (imm & 0x7ff);
}

static inline void t_enc_bcc(uint32_t *hw1, uint32_t *hw2, int cond,
                             int32_t off)
{
  uint32_t imm = (uint32_t)(off - 4) >> 1;
  uint32_t s   = ((uint32_t)(off - 4) >> 31) & 1;
  uint32_t j2  = (imm >> 18) & 1;
  uint32_t j1  = (imm >> 17) & 1;

  *hw1 = 0xf000 | s << 10 | cond << 6 | ((imm >> 11) & 0x3f);
  *hw2 = 0x8000 | j1 << 13 | j2 << 11 | (imm & 0x7ff);
}

static inline void t_b_at(jbuf_t *b, uint32_t pos, int32_t off, bool bl)
{
  uint32_t hw1;
  uint32_t hw2;

  t_enc_b(&hw1, &hw2, off, bl);
  jb_put16(b, pos, hw1);
  jb_put16(b, pos + 2, hw2);
}

static inline void t_bcc_at(jbuf_t *b, uint32_t pos, int cond, int32_t off)
{
  uint32_t hw1;
  uint32_t hw2;

  t_enc_bcc(&hw1, &hw2, cond, off);
  jb_put16(b, pos, hw1);
  jb_put16(b, pos + 2, hw2);
}

static inline void t_bx(jbuf_t *b, int rm)
{
  e16(b, 0x4700 | rm << 3);
}

static inline void t_blx(jbuf_t *b, int rm)
{
  e16(b, 0x4780 | rm << 3);
}

/****************************************************************************
 * VFP (single precision)
 ****************************************************************************/

static inline void t_vmov_sr(jbuf_t *b, int sn, int rt)   /* vmov sn, rt */
{
  e32(b, 0xee00 | (sn >> 1), rt << 12 | 0x0a10 | (sn & 1) << 7);
}

static inline void t_vmov_rs(jbuf_t *b, int rt, int sn)   /* vmov rt, sn */
{
  e32(b, 0xee10 | (sn >> 1), rt << 12 | 0x0a10 | (sn & 1) << 7);
}

/* vldr/vstr sd, [rn, #imm] (imm a multiple of 4, |imm| <= 1020) */

static inline void t_vldst(jbuf_t *b, bool load, int sd, int rn, int32_t imm)
{
  uint32_t u = imm >= 0;
  uint32_t a = (uint32_t)(imm >= 0 ? imm : -imm) >> 2;

  e32(b, (load ? 0xed10 : 0xed00) | u << 7 | (sd & 1) << 6 | rn,
      (sd >> 1) << 12 | 0x0a00 | a);
}

/* Three operand: vadd/vsub/vmul/vdiv sd, sn, sm */

enum
{
  VOP_ADD, VOP_SUB, VOP_MUL, VOP_DIV, VOP_MAXNM, VOP_MINNM
};

static inline void t_vop3(jbuf_t *b, int op, int sd, int sn, int sm)
{
  static const uint16_t h1[] =
  {
    0xee30, 0xee30, 0xee20, 0xee80, 0xfe80, 0xfe80
  };

  static const uint16_t h2[] =
  {
    0x0a00, 0x0a40, 0x0a00, 0x0a00, 0x0a00, 0x0a40
  };

  e32(b, h1[op] | (sd & 1) << 6 | (sn >> 1),
      (sd >> 1) << 12 | h2[op] | (sn & 1) << 7 | (sm & 1) << 5 | (sm >> 1));
}

/* Two operand: sd = op(sm) */

enum
{
  VOP_MOV, VOP_ABS, VOP_NEG, VOP_SQRT,
  VOP_CVT_F32_S32, VOP_CVT_F32_U32, VOP_CVT_S32_F32, VOP_CVT_U32_F32,
  VOP_RINTZ, VOP_RINTN, VOP_RINTP, VOP_RINTM
};

static inline void t_vop2(jbuf_t *b, int op, int sd, int sm)
{
  static const uint16_t h1[] =
  {
    0xeeb0, 0xeeb0, 0xeeb1, 0xeeb1,
    0xeeb8, 0xeeb8, 0xeebd, 0xeebc,
    0xeeb6, 0xfeb9, 0xfeba, 0xfebb
  };

  static const uint16_t h2[] =
  {
    0x0a40, 0x0ac0, 0x0a40, 0x0ac0,
    0x0ac0, 0x0a40, 0x0ac0, 0x0ac0,
    0x0ac0, 0x0a40, 0x0a40, 0x0a40
  };

  e32(b, h1[op] | (sd & 1) << 6,
      (sd >> 1) << 12 | h2[op] | (sm & 1) << 5 | (sm >> 1));
}

/* vcmp.f32 sd, sm; vmrs APSR_nzcv, fpscr */

static inline void t_vcmp(jbuf_t *b, int sd, int sm)
{
  e32(b, 0xeeb4 | (sd & 1) << 6,
      (sd >> 1) << 12 | 0x0a40 | (sm & 1) << 5 | (sm >> 1));
  e32(b, 0xeef1, 0xfa10);
}

#endif /* __APPS_GRAPHICS_FPCWASM_JIT_JIT_THUMB_H */
