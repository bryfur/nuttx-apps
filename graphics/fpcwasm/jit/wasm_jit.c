/****************************************************************************
 * apps/graphics/fpcwasm/jit/wasm_jit.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebAssembly to Thumb-2 (ARMv8-M Mainline, Cortex-M33, FPv5-SP), one
 * pass over each function, for WAMR (wasm_jit.h, docs/wasm.md "The JIT").
 *
 * The code:
 *   - r11 holds the module instance, r10 the linear memory's base, r9 its
 *     size: every access is checked against r9 (a trap past it), as
 *     wamrc's --bounds-checks=1 does.  They are callee-saved, so the host
 *     functions the code calls keep them; they are loaded again after any
 *     call (the memory may grow).
 *   - The wasm value stack is mapped to registers (r0-r3, lr and the
 *     callee-saved registers no local holds; s0-s15 for f32), values
 *     spilled to a slot of the frame for their depth when registers run
 *     out, at a call and where control flow merges (a block's values are
 *     in their slots at its start and its end).  Constants and locals are
 *     pushed lazily.
 *   - The most used i32 locals live in r4-r8, f32 locals in s16-s31
 *     (callee-saved: they survive calls); the rest in the frame.
 *   - Comparisons are fused with the br_if, if or select after them;
 *     br_table is a TBH jump table.
 *   - i64 values are register pairs (helpers for division and variable
 *     shifts), f32 is the FPU's, f64 software (the firmware's soft-float,
 *     through C helpers taking bit patterns).
 *   - Calls between functions are direct (BL), with their own convention
 *     (argument words in r0-r3 then on the stack, results in r0-r1);
 *     call_indirect goes through a table of every function's code and
 *     type, checked; imported host functions through a stub each, which
 *     calls a function with i32 arguments directly (WAMR's convention:
 *     exec_env first) and the others through WAMR's generic invocation.
 *   - Traps set WAMR's exception and unwind to the entry (the sp saved
 *     there), as do host functions that raise one.  Every loop looks at
 *     the exception (WAMR's terminate request sets it: the watchdog,
 *     Ctrl-C); every function with calls checks the native stack in its
 *     prologue.
 ****************************************************************************/

#ifdef __NuttX__
#  include <nuttx/config.h>
#  include <nuttx/kmalloc.h>
#  include <malloc.h>
#endif

#include "jit_internal.h"
#include "jit_thumb.h"

#include "wasm_opcode.h"
#include "bh_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__thumb2__) && (defined(CONFIG_FPCWASM_JIT) || \
                             defined(WASM_JIT_FORCE))
#  define JIT_HAVE 1
#else
#  define JIT_HAVE 0
#endif

#if JIT_HAVE

/****************************************************************************
 * Types
 ****************************************************************************/

enum
{
  T_I32, T_I64, T_F32, T_F64
};

#define IS64(t)      ((t) == T_I64 || (t) == T_F64)
#define TWORDS(t)    (IS64(t) ? 2 : 1)

enum
{
  V_CONST, V_REG, V_LOCAL, V_SLOT,
  V_LOCK                          /* An i32 local's register + c (lazy) */
};

typedef struct val_s
{
  uint8_t kind;
  uint8_t type;
  int8_t r;                       /* V_REG: core (lo) or S register */
  int8_t r2;                      /* V_REG 64-bit: hi */
  uint32_t local;                 /* V_LOCAL, V_LOCK */
  uint32_t depth;                 /* Its slot (the stack index it had) */
  uint64_t c;                     /* V_CONST; V_LOCK: the addend */
} val_t;

enum
{
  L_FRAME, L_CORE, L_SREG, L_NONE,         /* (L_NONE: never used) */
  L_KEPT                                    /* (assigning: the frame) */
};

enum
{
  CK_FUNC, CK_BLOCK, CK_LOOP, CK_IF, CK_ELSE
};

typedef struct ctrl_s
{
  uint8_t kind;
  bool dead;                      /* Opened in unreachable code */
  bool unreach;                   /* The rest of it is unreachable */
  uint16_t np;                    /* Parameters */
  uint16_t nr;                    /* Results */
  const uint8_t *pt;              /* Their wasm types (pt == NULL: none) */
  const uint8_t *rt;
  uint8_t rt1;                    /* A single result's type (rt points here) */
  uint32_t height;                /* Its stack's bottom (the parameters') */
  int label;                      /* Branches go here (end, loop start) */
  int else_label;
  int8_t rreg;                    /* Its one i32 result's register, -1: its
                                   * results go through their slots */
} ctrl_t;

/* Labels and the branches waiting for them */

enum
{
  F_B, F_BCC, F_BL, F_TBH
};

typedef struct label_s
{
  int32_t pos;                    /* -1: not yet bound */
  int32_t fix;                    /* First fixup waiting, -1 none */
} label_t;

typedef struct fixup_s
{
  uint32_t pos;
  uint32_t base;                  /* F_TBH: the table's base */
  int32_t next;
  uint8_t kind;
  uint8_t cond;
} fixup_t;

/* A branch from a body to the module's code (a trap, the unwinding), or a
 * call of a function: patched when the body is placed
 */

typedef struct far_s
{
  uint32_t pos;
  uint32_t target;                /* Module position, or function index */
  uint8_t kind;
  uint8_t cond;
} far_t;

/* An exact bounds check out of line (1 and 2-byte accesses) */

typedef struct cold_s
{
  int entry;
  int back;
  uint32_t k;                     /* The offset added (ip = ra + k) */
  int8_t ra;
  uint8_t size;
  uint8_t mode;                   /* COLD_* */
} cold_t;

enum
{
  COLD_EXACT,                     /* r9 the size - 3: ra (+ k) exactly */
  COLD_GUARD,                     /* r9 the size - JIT_GUARD: ra + k */
  COLD_GUARD_IP                   /* ... the address in ip (ra + k) */
};

/* r9 is the memory's size less JIT_GUARD where the memory has at least
 * 64 KB when the module is compiled (it only grows): an access at ra +
 * k of size bytes, k + size <= JIT_GUARD, is in when ra < r9 (cmp, bhs:
 * no adding of k, with its carry, first); ra at or past r9 is checked
 * exactly out of line (the memory's last JIT_GUARD bytes)
 */

#define JIT_GUARD    4096

typedef struct jc_s
{
  /* The module */

  WASMModuleInstance *inst;
  WASMModule *mod;
  jit_module_t *jm;
  jbuf_t mb;                      /* The module's code */
  uint32_t *func_off;             /* Each function's position in mb */
  uint32_t *typeid;               /* The canonical index of each type */
  uint32_t trap_pos[JT_COUNT];    /* mb positions of the trap entries */
  uint32_t unwind_pos;
  uint32_t exc_pos;               /* Unwinding with the exception set */
  uint32_t mem_off;               /* The memory instance (from r11) */
  bool has_mem;
  uint32_t guard;                 /* JIT_GUARD, or 0 (r9 the size - 3) */
  uint8_t *may_grow;              /* Each function: may grow the memory */
  far_t *calls;                   /* BLs to functions (mb positions) */
  uint32_t ncalls;
  uint32_t calls_cap;
  uint32_t *mloops;               /* Loop heads (mb positions) */
  uint32_t nmloops;
  uint32_t mlcap;
  char *err;
  size_t errlen;
  bool failed;

  /* The function */

  jbuf_t fb;                      /* Its body */
  WASMFunction *func;
  uint32_t fidx;                  /* Module function index (no imports) */
  WASMFuncType *ftype;
  const uint8_t *code;
  const uint8_t *code_end;
  uint32_t nparams;
  uint32_t nlocals;
  uint8_t *ltype;                 /* T_* of each local */
  uint8_t *lkind;                 /* L_* */
  int8_t *lreg;
  int8_t *lreg2;                  /* An i64 local's high word's register */
  uint32_t *loff;
  uint32_t *lweight;
  uint8_t *lfirst;                /* 0 untouched, 1 read (or set inside a
                                   * block) first, 2 set first at the top
                                   * level: no starting value needed */
  uint32_t out_words;             /* Outgoing stack argument words */
  uint32_t nslots;                /* Value stack slots used */
  uint32_t local_base;
  uint32_t slot_base;
  uint32_t frame_size;
  uint32_t ncore_pushed;
  uint32_t ns_pushed;
  bool makes_calls;
  uint32_t callw;                 /* Calls, weighted as the locals' uses */
  int16_t csl[4];                 /* Locals in caller-saved registers of a
                                   * function that calls: saved, loaded
                                   * again around each call */
  uint32_t ncsl;

  val_t *vs;
  uint32_t vsp;
  uint32_t vcap;
  ctrl_t *cs;
  uint32_t csp;
  uint32_t ccap;
  label_t *labels;
  uint32_t nlabels;
  uint32_t lcap;
  fixup_t *fix;
  uint32_t nfix;
  uint32_t fcap;
  far_t *fars;
  uint32_t nfars;
  uint32_t farcap;
  cold_t *cold;
  uint32_t ncold;
  uint32_t coldcap;
  uint32_t *lheads;               /* Its loop heads (fb positions) */
  uint32_t nlheads;
  uint32_t lhcap;
  /* Bounds proven in this straight-line code: [reg, reg + end) is in the
   * memory for a local's register (until a label or a write to it; the
   * memory only grows)
   */

  struct
  {
    int8_t reg;
    uint32_t end;
  } bc[4];
  uint32_t nbc;
  bool tbh_near;                  /* br_table entries only to its fragments */
  bool tbh_far;                   /* A br_table entry was out of reach */
  bool wide;                      /* 64-bit values: pairs of temporaries */
  bool regs_out;                  /* Out of registers: compiled again */
  uint8_t loc_max;                /* Core registers for locals at most */
  int hint;                       /* local.set/tee follows: its local, or -1 */
  int shint;                      /* ... of an f32 local in an S register */
  bool hint_used;

  /* Constants used in loops, kept in registers (loaded by the prologue) */

  struct
  {
    uint32_t v;
    uint32_t w;
    uint8_t f32;
  } kc[48];
  uint32_t nkc;
  struct
  {
    uint32_t v;
    int8_t r;
    uint8_t f32;
  } pin[16];
  uint32_t npin;

  /* Constants left in free temporaries (kvalid: which hold kval[r]) */

  uint32_t kval[16];
  uint32_t kvalid;

  /* Registers */

  uint32_t ctemp;                 /* Core registers that are temporaries */
  uint32_t cfree;                 /* ... free */
  uint32_t sfree;                 /* s0-s15 free */
  uint32_t cused;                 /* Callee-saved core registers used */
  uint32_t stemp;                 /* S registers that are temporaries */
  bool lr_used;                   /* lr held a temporary */
  bool leaf;                      /* No calls (nor helpers) */
  uint32_t sused;                 /* s16+ used: how many */

  int epilogue;                   /* Label */
  uint32_t peak;
} jc_t;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_alloc_order[] =
{
  0, 1, 2, 3, 14, 4, 5, 6, 7, 8
};

#define CALLER_SAVED  0x400f              /* r0-r3, lr */
#define LOCAL_HOT     16                  /* A local's weight in a loop */
#define CALLEE_TEMPS  0x01f0              /* r4-r8 */

static uint32_t g_jit_bytes;               /* The compiler's memory now */
static uint32_t g_jit_peak;

/****************************************************************************
 * Memory
 ****************************************************************************/

void *jit_realloc(void *p, uint32_t size)
{
  uint32_t *h = p != NULL ? (uint32_t *)p - 2 : NULL;
  uint32_t old = h != NULL ? h[0] : 0;
  uint32_t *n;

  if (size == 0)
    {
      if (h != NULL)
        {
          g_jit_bytes -= old;
          wasm_runtime_free(h);
        }

      return NULL;
    }

  n = wasm_runtime_realloc(h, size + 8);
  if (n == NULL)
    {
      return NULL;
    }

  g_jit_bytes += size - old;
  if (g_jit_bytes > g_jit_peak)
    {
      g_jit_peak = g_jit_bytes;
    }

  n[0] = size;
  return n + 2;
}

static void jit_free(void *p)
{
  jit_realloc(p, 0);
}

static void *jit_zalloc(uint32_t size)
{
  void *p = jit_realloc(NULL, size);

  if (p != NULL)
    {
      memset(p, 0, size);
    }

  return p;
}

#define GROW(c, arr, n, cap) \
  ((n) < (cap) || grow_arr((c), (void **)&(arr), &(cap), sizeof(*(arr))))

static bool grow_arr(jc_t *c, void **arr, uint32_t *cap, uint32_t elem)
{
  uint32_t ncap = *cap ? *cap * 2 : 32;
  void *p = jit_realloc(*arr, ncap * elem);

  if (p == NULL)
    {
      c->failed = true;
      snprintf(c->err, c->errlen, "out of memory");
      return false;
    }

  *arr = p;
  *cap = ncap;
  return true;
}

static void fail(jc_t *c, const char *fmt, uint32_t a)
{
  if (!c->failed)
    {
      char msg[96];

      snprintf(msg, sizeof(msg), fmt, (unsigned)a);
      snprintf(c->err, c->errlen, "function %u: %s",
               (unsigned)(c->fidx + c->mod->import_function_count), msg);
      c->failed = true;
    }
}

/****************************************************************************
 * Decoding
 ****************************************************************************/

static uint32_t rd_u32(const uint8_t **pp)
{
  const uint8_t *p = *pp;
  uint32_t v = 0;
  int sh = 0;
  uint8_t b;

  if (*p < 0x80)
    {
      *pp = p + 1;                /* (the common case: one byte) */
      return *p;
    }

  do
    {
      b = *p++;
      if (sh < 32)
        {
          v |= (uint32_t)(b & 0x7f) << sh;
        }

      sh += 7;
    }
  while (b & 0x80);

  *pp = p;
  return v;
}

static int64_t rd_s64(const uint8_t **pp, int bits)
{
  const uint8_t *p = *pp;
  uint64_t v = 0;
  int sh = 0;
  uint8_t b;

  do
    {
      b = *p++;
      if (sh < 64)
        {
          v |= (uint64_t)(b & 0x7f) << sh;
        }

      sh += 7;
    }
  while (b & 0x80);

  if (sh < 64 && (b & 0x40))
    {
      v |= ~(uint64_t)0 << sh;
    }

  *pp = p;
  (void)bits;
  return (int64_t)v;
}

static uint32_t rd_le32(const uint8_t **pp)
{
  const uint8_t *p = *pp;

  *pp = p + 4;
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int jtype(uint8_t t)
{
  switch (t)
    {
      case VALUE_TYPE_I64:
        return T_I64;

      case VALUE_TYPE_F32:
        return T_F32;

      case VALUE_TYPE_F64:
        return T_F64;

      default:
        return T_I32;           /* i32, funcref, externref */
    }
}

static WASMFuncType *func_type_of(WASMModule *m, uint32_t idx)
{
  if (idx < m->import_function_count)
    {
      return m->import_functions[idx].u.function.func_type;
    }

  return m->functions[idx - m->import_function_count]->func_type;
}

static uint32_t type_words(const uint8_t *t, uint32_t n)
{
  uint32_t w = 0;
  uint32_t i;

  for (i = 0; i < n; i++)
    {
      w += TWORDS(jtype(t[i]));
    }

  return w;
}

/* Skip an instruction's immediates (p after the opcode); false if the
 * compiler does not handle the instruction
 */

static bool skip_imm(uint8_t op, const uint8_t **pp)
{
  const uint8_t *p = *pp;
  uint32_t n;
  uint32_t sub;

  switch (op)
    {
      case WASM_OP_BLOCK:
      case WASM_OP_LOOP:
      case WASM_OP_IF:
        if (*p == 0x40 || (*p & 0xc0) == 0x40)
          {
            p++;                /* Empty, or a value type */
          }
        else
          {
            rd_s64(&p, 33);
          }
        break;

      case WASM_OP_BR:
      case WASM_OP_BR_IF:
      case WASM_OP_CALL:
      case WASM_OP_GET_LOCAL:
      case WASM_OP_SET_LOCAL:
      case WASM_OP_TEE_LOCAL:
      case WASM_OP_GET_GLOBAL:
      case WASM_OP_SET_GLOBAL:
      case WASM_OP_TABLE_GET:
      case WASM_OP_TABLE_SET:
      case WASM_OP_REF_FUNC:
      case WASM_OP_MEMORY_SIZE:
      case WASM_OP_MEMORY_GROW:
        rd_u32(&p);
        break;

      case WASM_OP_BR_TABLE:
        n = rd_u32(&p);
        while (n-- > 0)
          {
            rd_u32(&p);
          }

        rd_u32(&p);
        break;

      case WASM_OP_CALL_INDIRECT:
        rd_u32(&p);
        rd_u32(&p);
        break;

      case WASM_OP_SELECT_T:
        n = rd_u32(&p);
        p += n;
        break;

      case WASM_OP_REF_NULL:
        p++;
        break;

      case WASM_OP_I32_CONST:
      case WASM_OP_I64_CONST:
        rd_s64(&p, 64);
        break;

      case WASM_OP_F32_CONST:
        p += 4;
        break;

      case WASM_OP_F64_CONST:
        p += 8;
        break;

      case WASM_OP_MISC_PREFIX:
        sub = rd_u32(&p);
        switch (sub)
          {
            case WASM_OP_I32_TRUNC_SAT_S_F32:
            case WASM_OP_I32_TRUNC_SAT_U_F32:
            case WASM_OP_I32_TRUNC_SAT_S_F64:
            case WASM_OP_I32_TRUNC_SAT_U_F64:
            case WASM_OP_I64_TRUNC_SAT_S_F32:
            case WASM_OP_I64_TRUNC_SAT_U_F32:
            case WASM_OP_I64_TRUNC_SAT_S_F64:
            case WASM_OP_I64_TRUNC_SAT_U_F64:
              break;

            case WASM_OP_MEMORY_INIT:
              rd_u32(&p);
              rd_u32(&p);
              break;

            case WASM_OP_DATA_DROP:
            case WASM_OP_MEMORY_FILL:
            case WASM_OP_ELEM_DROP:
            case WASM_OP_TABLE_GROW:
            case WASM_OP_TABLE_SIZE:
            case WASM_OP_TABLE_FILL:
              rd_u32(&p);
              break;

            case WASM_OP_MEMORY_COPY:
            case WASM_OP_TABLE_INIT:
            case WASM_OP_TABLE_COPY:
              rd_u32(&p);
              rd_u32(&p);
              break;

            default:
              return false;
          }
        break;

      default:
        if (op >= WASM_OP_I32_LOAD && op <= WASM_OP_I64_STORE32)
          {
            rd_u32(&p);         /* align */
            rd_u32(&p);         /* offset */
            break;
          }

        if (op == WASM_OP_UNREACHABLE || op == WASM_OP_NOP ||
            op == WASM_OP_ELSE || op == WASM_OP_END ||
            op == WASM_OP_RETURN || op == WASM_OP_DROP ||
            op == WASM_OP_SELECT || op == WASM_OP_REF_IS_NULL ||
            (op >= WASM_OP_I32_EQZ && op <= WASM_OP_I64_EXTEND32_S))
          {
            break;
          }

        return false;
    }

  *pp = p;
  return true;
}

/****************************************************************************
 * Labels, branches
 ****************************************************************************/

static int new_label(jc_t *c)
{
  if (!GROW(c, c->labels, c->nlabels, c->lcap))
    {
      return 0;
    }

  c->labels[c->nlabels].pos = -1;
  c->labels[c->nlabels].fix = -1;
  return (int)c->nlabels++;
}

static void patch(jc_t *c, fixup_t *f, uint32_t target)
{
  int32_t off = (int32_t)target - (int32_t)f->pos;

  switch (f->kind)
    {
      case F_B:
        t_b_at(&c->fb, f->pos, off, false);
        break;

      case F_BCC:
        if (off > 1048574 || off < -1048576)
          {
            fail(c, "a branch too far", 0);
          }

        t_bcc_at(&c->fb, f->pos, f->cond, off);
        break;

      case F_TBH:
        if (target < f->base || target - f->base > 131070)
          {
            c->tbh_far = true;            /* (compiled again: tbh_near) */
            fail(c, "a br_table too far", 0);
          }

        jb_put16(&c->fb, f->pos, (target - f->base) >> 1);
        break;
    }
}

static void lbind(jc_t *c, int l)
{
  label_t *lb = &c->labels[l];
  int32_t i;

  c->nbc = 0;                   /* (other ways in: nothing proven) */
  c->kvalid = 0;                /* (nor known) */
  lb->pos = (int32_t)c->fb.len;
  for (i = lb->fix; i >= 0; i = c->fix[i].next)
    {
      if (!c->fb.oom)
        {
          patch(c, &c->fix[i], c->fb.len);
        }
    }

  lb->fix = -1;
}

static void add_fix(jc_t *c, int l, uint8_t kind, uint8_t cond,
                    uint32_t pos, uint32_t base)
{
  label_t *lb = &c->labels[l];
  fixup_t f;

  f.pos  = pos;
  f.base = base;
  f.kind = kind;
  f.cond = cond;
  f.next = lb->fix;
  if (lb->pos >= 0)
    {
      if (!c->fb.oom)
        {
          patch(c, &f, (uint32_t)lb->pos);
        }

      return;
    }

  if (!GROW(c, c->fix, c->nfix, c->fcap))
    {
      return;
    }

  c->fix[c->nfix] = f;
  lb->fix = (int32_t)c->nfix++;
}

static void br_to(jc_t *c, int l)            /* b.w label */
{
  uint32_t pos = c->fb.len;

  e32(&c->fb, 0xf000, 0x9000);
  add_fix(c, l, F_B, 0, pos, 0);
}

/* b.n to a label bound already, when it is near enough; else b.w */

static void br_back(jc_t *c, int l)
{
  int32_t off = c->labels[l].pos >= 0 ?
                c->labels[l].pos - (int32_t)(c->fb.len + 4) : 1 << 20;

  if (off >= -2048 && off <= 2046)
    {
      e16(&c->fb, 0xe000 | ((off >> 1) & 0x7ff));
      return;
    }

  br_to(c, l);
}

static void bcc_to(jc_t *c, int cond, int l)  /* b<cond>.w label */
{
  uint32_t pos = c->fb.len;

  if (cond == CC_AL)
    {
      br_to(c, l);
      return;
    }

  e32(&c->fb, 0xf000, 0x8000);
  add_fix(c, l, F_BCC, (uint8_t)cond, pos, 0);
}

/* A branch to the module's code (trap entries, unwinding) */

static void far_br(jc_t *c, int cond, uint32_t target)
{
  if (!GROW(c, c->fars, c->nfars, c->farcap))
    {
      return;
    }

  c->fars[c->nfars].pos    = c->fb.len;
  c->fars[c->nfars].target = target;
  c->fars[c->nfars].kind   = cond == CC_AL ? F_B : F_BCC;
  c->fars[c->nfars].cond   = (uint8_t)cond;
  c->nfars++;
  e32(&c->fb, 0xf000, cond == CC_AL ? 0x9000 : 0x8000);
}

static void trap_if(jc_t *c, int cond, int code)
{
  far_br(c, cond, c->trap_pos[code]);
}

static void call_func(jc_t *c, uint32_t idx)  /* bl function idx */
{
  if (!GROW(c, c->fars, c->nfars, c->farcap))
    {
      return;
    }

  c->fars[c->nfars].pos    = c->fb.len;
  c->fars[c->nfars].target = idx;
  c->fars[c->nfars].kind   = F_BL;
  c->fars[c->nfars].cond   = 0;
  c->nfars++;
  e32(&c->fb, 0xf000, 0xd000);
}

/****************************************************************************
 * Frame addressing
 ****************************************************************************/

static uint32_t slot_off(jc_t *c, uint32_t depth)
{
  if (depth >= c->nslots)
    {
      c->nslots = depth + 1;              /* (the frame's size so far) */
    }

  return c->slot_base + 8 * depth;
}

static void ld_sp(jc_t *c, int r, uint32_t off)
{
  t_ldst(&c->fb, LS_LDR, r, R_SP, (int32_t)off);
}

static void st_sp(jc_t *c, int r, uint32_t off)
{
  if (r == R_IP && off >= 4096)
    {
      /* ip is the value: the address through lr, saved around it */

      e32(&c->fb, 0xf84d, 0xed08);              /* str lr, [sp, #-8]! */
      t_movi(&c->fb, R_LR, off + 8);
      e16(&c->fb, 0x4400 | 0x80 | R_SP << 3 | (R_LR & 7));  /* add lr, sp */
      t_ldst(&c->fb, LS_STR, R_IP, R_LR, 0);
      e32(&c->fb, 0xf85d, 0xeb08);              /* ldr lr, [sp], #8 */
      return;
    }

  t_ldst(&c->fb, LS_STR, r, R_SP, (int32_t)off);
}

static void vldst_sp(jc_t *c, bool load, int s, uint32_t off)
{
  if (off <= 1020)
    {
      t_vldst(&c->fb, load, s, R_SP, (int32_t)off);
    }
  else
    {
      t_addi(&c->fb, R_IP, R_SP, (int32_t)off);
      t_vldst(&c->fb, load, s, R_IP, 0);
    }
}

/* r = word at [r11 + off] (instance data) */

static void ld_inst(jc_t *c, int op, int r, uint32_t off)
{
  if (off < 4096)
    {
      t_ldst(&c->fb, op, r, JR_INST, (int32_t)off);
    }
  else
    {
      t_movi(&c->fb, R_IP, off);
      t_ldst_reg(&c->fb, op, r, JR_INST, R_IP, 0);
    }
}

/* Reload the memory registers (after a call: the memory may have moved) */

static void reload_mem(jc_t *c, jbuf_t *b)
{
  if (c->has_mem)
    {
      t_ldst(b, LS_LDR, JR_MEM, JR_INST,
             c->mem_off + offsetof(WASMMemoryInstance, memory_data));
      t_ldst(b, LS_LDR, JR_LIM4, JR_INST, EXT_OFF(EXT_LIM4));
    }
}

/****************************************************************************
 * Registers
 ****************************************************************************/

static void spill_at(jc_t *c, uint32_t i);
static int pinned(jc_t *c, uint32_t v, bool f32);

static int take_core(jc_t *c, int r)
{
  c->cfree &= ~(1u << r);
  c->kvalid &= ~(1u << r);
  if (r == R_LR)
    {
      c->lr_used = true;
    }

  if (r >= 4 && r <= 8)
    {
      c->cused |= 1u << r;
    }

  return r;
}

static int alloc_core(jc_t *c)
{
  uint32_t i;
  int tries;

  for (tries = 0; tries < 64; tries++)
    {
      for (i = 0; i < sizeof(g_alloc_order); i++)
        {
          int r = g_alloc_order[i];

          if ((c->cfree & ~c->kvalid) & (1u << r))
            {
              return take_core(c, r);
            }
        }

      for (i = 0; i < sizeof(g_alloc_order); i++)
        {
          int r = g_alloc_order[i];

          if (c->cfree & (1u << r))
            {
              return take_core(c, r);
            }
        }

      /* Spill the deepest value in a core register */

      for (i = 0; i < c->vsp; i++)
        {
          val_t *v = &c->vs[i];

          if (v->kind == V_REG && v->type != T_F32)
            {
              spill_at(c, i);
              break;
            }
        }

      if (i == c->vsp)
        {
          break;
        }
    }

  c->regs_out = true;           /* (compiled again with fewer locals) */
  fail(c, "out of registers", 0);
  return R0;
}

static void free_core(jc_t *c, int r)
{
  if (c->ctemp & (1u << r))
    {
      c->cfree |= 1u << r;
    }
}

/* A register holding the i32 constant k, to read at once (before anything
 * else is allocated): its own (pinned), a free temporary that holds it
 * already, or one it is put in (and left in, known), else ip.  avoid: the
 * registers of operands freed already, still to be read.
 */

static int const_reg(jc_t *c, uint32_t k, uint32_t avoid)
{
  uint32_t m = c->cfree & c->kvalid;
  uint32_t i;
  int r = pinned(c, k, false);

  if (r >= 0)
    {
      return r;
    }

  for (; m != 0; m &= m - 1)
    {
      r = __builtin_ctz(m);
      if (c->kval[r] == k)
        {
          return r;
        }
    }

  for (i = 0; i < sizeof(g_alloc_order); i++)
    {
      r = g_alloc_order[i];
      if ((c->cfree & ~c->kvalid & ~avoid) & (1u << r))
        {
          break;
        }
    }

  if (i == sizeof(g_alloc_order))
    {
      t_movi(&c->fb, R_IP, k);
      return R_IP;
    }

  t_movi(&c->fb, r, k);
  c->kval[r] = k;
  c->kvalid |= 1u << r;
  if (r >= 4 && r <= 8)
    {
      c->cused |= 1u << r;          /* (saved by the prologue) */
    }

  if (r == R_LR)
    {
      c->lr_used = true;
    }

  return r;
}

static int alloc_s(jc_t *c)
{
  uint32_t i;
  int tries;

  for (tries = 0; tries < 64; tries++)
    {
      if ((c->sfree & c->stemp) != 0)
        {
          int s = __builtin_ctz(c->sfree & c->stemp);

          c->sfree &= ~(1u << s);
          return s;
        }

      for (i = 0; i < c->vsp; i++)
        {
          val_t *v = &c->vs[i];

          if (v->kind == V_REG && v->type == T_F32)
            {
              spill_at(c, i);
              break;
            }
        }

      if (i == c->vsp)
        {
          break;
        }
    }

  fail(c, "out of FPU registers", 0);
  return 0;
}

static void free_s(jc_t *c, int s)
{
  if (s < 16 && (c->stemp & (1u << s)))
    {
      c->sfree |= 1u << s;
    }
}

static void free_val(jc_t *c, val_t *v)
{
  if (v->kind != V_REG)
    {
      return;
    }

  if (v->type == T_F32)
    {
      free_s(c, v->r);
    }
  else
    {
      free_core(c, v->r);
      if (IS64(v->type))
        {
          free_core(c, v->r2);
        }
    }

  v->kind = V_SLOT;             /* (no longer holds anything) */
}

/* Whether a value is in a register a call clobbers */

static bool val_caller_saved(val_t *v)
{
  if (v->kind != V_REG)
    {
      return false;
    }

  if (v->type == T_F32)
    {
      return true;
    }

  return ((CALLER_SAVED >> v->r) & 1) ||
         (IS64(v->type) && ((CALLER_SAVED >> v->r2) & 1));
}

/****************************************************************************
 * The value stack
 ****************************************************************************/

static val_t *vpush(jc_t *c, int kind, int type)
{
  val_t *v;

  if (!GROW(c, c->vs, c->vsp, c->vcap))
    {
      static val_t dummy;

      return &dummy;
    }

  v = &c->vs[c->vsp];
  v->kind  = (uint8_t)kind;
  v->type  = (uint8_t)type;
  v->r     = 0;
  v->r2    = 0;
  v->local = 0;
  v->c     = 0;
  v->depth = c->vsp++;
  return v;
}

static void push_reg(jc_t *c, int type, int r, int r2)
{
  val_t *v = vpush(c, V_REG, type);

  v->r  = (int8_t)r;
  v->r2 = (int8_t)r2;
}

static void push_const(jc_t *c, int type, uint64_t k)
{
  val_t *v = vpush(c, V_CONST, type);

  v->c = k;
}

static val_t pop(jc_t *c)
{
  if (c->vsp == 0)
    {
      val_t z;

      fail(c, "stack underflow", 0);
      memset(&z, 0, sizeof(z));
      z.kind = V_CONST;
      return z;
    }

  return c->vs[--c->vsp];
}

static val_t *peek(jc_t *c, uint32_t n)        /* n = 0: the top */
{
  return &c->vs[c->vsp - 1 - n];
}

/* Store a value into the slot at off (the value is left as it is) */

static void store_slot(jc_t *c, val_t *v, uint32_t off)
{
  uint32_t w;

  if (v->kind == V_SLOT && slot_off(c, v->depth) == off)
    {
      return;
    }

  if (v->kind == V_REG)
    {
      if (v->type == T_F32)
        {
          vldst_sp(c, false, v->r, off);
        }
      else
        {
          st_sp(c, v->r, off);
          if (IS64(v->type))
            {
              st_sp(c, v->r2, off + 4);
            }
        }

      return;
    }

  if (v->kind == V_LOCAL && c->lkind[v->local] == L_CORE)
    {
      st_sp(c, c->lreg[v->local], off);
      if (IS64(v->type))
        {
          st_sp(c, c->lreg2[v->local], off + 4);
        }

      return;
    }

  if (v->kind == V_LOCAL && c->lkind[v->local] == L_SREG)
    {
      vldst_sp(c, false, c->lreg[v->local], off);
      return;
    }

  if (v->kind == V_LOCK)
    {
      t_addi(&c->fb, R_IP, c->lreg[v->local], (int32_t)v->c);
      st_sp(c, R_IP, off);
      return;
    }

  for (w = 0; w < (uint32_t)TWORDS(v->type); w++)
    {
      if (v->kind == V_CONST)
        {
          t_movi(&c->fb, R_IP, (uint32_t)(v->c >> (32 * w)));
        }
      else if (v->kind == V_LOCAL)
        {
          ld_sp(c, R_IP, c->loff[v->local] + 4 * w);
        }
      else
        {
          ld_sp(c, R_IP, slot_off(c, v->depth) + 4 * w);
        }

      st_sp(c, R_IP, off + 4 * w);
    }
}

static void spill_at(jc_t *c, uint32_t i)
{
  val_t *v = &c->vs[i];

  if (v->kind == V_REG || v->kind == V_LOCAL || v->kind == V_LOCK)
    {
      store_slot(c, v, slot_off(c, i));
      free_val(c, v);
      v->kind = V_SLOT;
    }
}

/* Every value in a register or a local into its slot (control flow
 * merges: block starts, branches)
 */

static void spill_all(jc_t *c)
{
  uint32_t i;

  for (i = 0; i < c->vsp; i++)
    {
      spill_at(c, i);
    }
}

/* A block's parameters (from its height up) into their slots, constants
 * too: its start and every branch back to it see them there
 */

static void spill_from(jc_t *c, uint32_t height)
{
  uint32_t i;

  for (i = height; i < c->vsp; i++)
    {
      val_t *v = &c->vs[i];

      if (v->kind != V_SLOT)
        {
          store_slot(c, v, slot_off(c, i));
          free_val(c, v);
          v->kind = V_SLOT;
        }
    }
}

/* The values a call clobbers (from start up) */

static void spill_caller_saved(jc_t *c, uint32_t upto)
{
  uint32_t i;

  for (i = 0; i < upto; i++)
    {
      if (val_caller_saved(&c->vs[i]))
        {
          spill_at(c, i);
        }
    }
}

/* A register a local lives in changes: what was proven of it goes */

static void bc_forget(jc_t *c, int r)
{
  uint32_t i;

  for (i = 0; i < c->nbc; i++)
    {
      if (c->bc[i].reg == r)
        {
          c->bc[i] = c->bc[--c->nbc];
          i--;
        }
    }
}

/* Before a local changes: the stack's references to it get its value (a
 * free register, or their slot; only their slot when temps is false: an
 * instruction's operands, freed already, may still be in registers that
 * look free)
 */

static void local_changing(jc_t *c, uint32_t l, bool temps)
{
  uint32_t i;

  for (i = 0; i < c->vsp; i++)
    {
      val_t *v = &c->vs[i];

      if ((v->kind != V_LOCAL && v->kind != V_LOCK) || v->local != l)
        {
          continue;
        }

      if (!temps)
        {
          spill_at(c, i);
        }
      else if (v->kind == V_LOCK && (c->cfree & c->ctemp) != 0)
        {
          int r = alloc_core(c);

          t_addi(&c->fb, r, c->lreg[l], (int32_t)v->c);
          v->kind = V_REG;
          v->r = (int8_t)r;
        }
      else if (v->kind == V_LOCK)
        {
          spill_at(c, i);
        }
      else if (c->lkind[l] == L_CORE && IS64(c->ltype[l]) &&
          __builtin_popcount(c->cfree & c->ctemp) >= 2)
        {
          int r = alloc_core(c);
          int r2 = alloc_core(c);

          t_mov(&c->fb, r, c->lreg[l]);
          t_mov(&c->fb, r2, c->lreg2[l]);
          v->kind = V_REG;
          v->r = (int8_t)r;
          v->r2 = (int8_t)r2;
        }
      else if (c->lkind[l] == L_CORE && !IS64(c->ltype[l]) &&
               (c->cfree & c->ctemp) != 0)
        {
          int r = alloc_core(c);

          t_mov(&c->fb, r, c->lreg[l]);
          v->kind = V_REG;
          v->r = (int8_t)r;
        }
      else if (c->lkind[l] == L_SREG && (c->sfree & c->stemp) != 0)
        {
          int s = alloc_s(c);

          t_vop2(&c->fb, VOP_MOV, s, c->lreg[l]);
          v->kind = V_REG;
          v->r = (int8_t)s;
        }
      else
        {
          spill_at(c, i);
        }
    }
}

/* The register for an i32 result: the local's when a local.set or tee of
 * a local in a register follows (the result computed into it), else a
 * temporary
 */

static int alloc_sdest(jc_t *c)
{
  if (c->shint >= 0 && !c->hint_used)
    {
      local_changing(c, (uint32_t)c->shint, false);
      c->hint_used = true;
      return c->lreg[c->shint];
    }

  return alloc_s(c);
}

static int alloc_dest(jc_t *c)
{
  if (c->hint >= 0 && !c->hint_used)
    {
      local_changing(c, (uint32_t)c->hint, false);
      bc_forget(c, c->lreg[c->hint]);
      c->hint_used = true;
      return c->lreg[c->hint];
    }

  return alloc_core(c);
}

/* Whether the next instruction is a local.set or tee of a local in a core
 * register
 */

static void set_hint(jc_t *c, const uint8_t *next)
{
  c->hint = -1;
  c->shint = -1;
  c->hint_used = false;
  if (next < c->code_end &&
      (*next == WASM_OP_SET_LOCAL || *next == WASM_OP_TEE_LOCAL))
    {
      const uint8_t *q = next + 1;
      uint32_t l = rd_u32(&q);

      if (l < c->nlocals && c->lkind[l] == L_CORE)
        {
          c->hint = (int)l;
        }
      else if (l < c->nlocals && c->lkind[l] == L_SREG)
        {
          c->shint = (int)l;
        }
    }
}

/* After an instruction that took the hint: its local.set (or tee) done */

static const uint8_t *finish_hint(jc_t *c, const uint8_t *p)
{
  if (c->hint_used)
    {
      uint8_t op = *p++;
      uint32_t l = rd_u32(&p);
      val_t v = pop(c);

      if (v.kind != V_REG || v.r != c->lreg[l])
        {
          fail(c, "hint", 0);
        }

      (void)v;

      if (op == WASM_OP_TEE_LOCAL)
        {
          vpush(c, V_LOCAL, c->ltype[l])->local = l;
        }
    }

  c->hint = -1;
  c->shint = -1;
  c->hint_used = false;
  return p;
}

/****************************************************************************
 * Values into registers
 ****************************************************************************/

/* A 32-bit integer value in a core register (a local's register is used
 * as it is: read only)
 */

static int vreg(jc_t *c, val_t *v)
{
  int r;

  switch (v->kind)
    {
      case V_REG:
        return v->r;

      case V_LOCAL:
        if (c->lkind[v->local] == L_CORE)
          {
            return c->lreg[v->local];
          }

        r = alloc_core(c);
        ld_sp(c, r, c->loff[v->local]);
        break;

      case V_CONST:
        r = pinned(c, (uint32_t)v->c, false);
        if (r >= 0)
          {
            return r;           /* (read only, like a local's) */
          }

        {
          uint32_t m = c->cfree & c->kvalid;

          for (; m != 0; m &= m - 1)
            {
              if (c->kval[__builtin_ctz(m)] == (uint32_t)v->c)
                {
                  break;
                }
            }

          if (m != 0)
            {
              r = take_core(c, __builtin_ctz(m));      /* (holds it) */
              break;
            }
        }

        r = alloc_core(c);
        t_movi(&c->fb, r, (uint32_t)v->c);
        break;

      case V_LOCK:
        r = alloc_core(c);
        t_addi(&c->fb, r, c->lreg[v->local], (int32_t)v->c);
        break;

      default:
        r = alloc_core(c);
        ld_sp(c, r, slot_off(c, v->depth));
        break;
    }

  v->kind = V_REG;
  v->r = (int8_t)r;
  return r;
}

/* A 32-bit integer value in a temporary register it owns */

static int vreg_own(jc_t *c, val_t *v)
{
  int r;

  if (v->kind == V_CONST)
    {
      r = alloc_core(c);
      t_movi(&c->fb, r, (uint32_t)v->c);
      v->kind = V_REG;
      v->r = (int8_t)r;
      return r;
    }

  if (v->kind == V_LOCAL && c->lkind[v->local] == L_CORE)
    {
      r = alloc_core(c);
      t_mov(&c->fb, r, c->lreg[v->local]);
      v->kind = V_REG;
      v->r = (int8_t)r;
      return r;
    }

  return vreg(c, v);
}

/* A 64-bit value in a pair of temporaries */

static void vreg64(jc_t *c, val_t *v, int *lo, int *hi)
{
  int a;
  int b;

  if (v->kind != V_REG)
    {
      a = alloc_core(c);
      b = alloc_core(c);
      if (v->kind == V_CONST)
        {
          t_movi(&c->fb, a, (uint32_t)v->c);
          t_movi(&c->fb, b, (uint32_t)(v->c >> 32));
        }
      else if (v->kind == V_LOCAL && c->lkind[v->local] == L_CORE)
        {
          t_mov(&c->fb, a, c->lreg[v->local]);
          t_mov(&c->fb, b, c->lreg2[v->local]);
        }
      else
        {
          uint32_t off = v->kind == V_LOCAL ? c->loff[v->local] :
                         slot_off(c, v->depth);

          ld_sp(c, a, off);
          ld_sp(c, b, off + 4);
        }

      v->kind = V_REG;
      v->r  = (int8_t)a;
      v->r2 = (int8_t)b;
    }

  *lo = v->r;
  *hi = v->r2;
}

/* An f32 value in an S register (a local's: read only) */

static int vsreg(jc_t *c, val_t *v)
{
  int s;

  switch (v->kind)
    {
      case V_REG:
        return v->r;

      case V_LOCAL:
        if (c->lkind[v->local] == L_SREG)
          {
            return c->lreg[v->local];
          }

        s = alloc_s(c);
        vldst_sp(c, true, s, c->loff[v->local]);
        break;

      case V_CONST:
        s = pinned(c, (uint32_t)v->c, true);
        if (s >= 0)
          {
            return s;
          }

        s = alloc_s(c);
        t_movi(&c->fb, R_IP, (uint32_t)v->c);
        t_vmov_sr(&c->fb, s, R_IP);
        break;

      default:
        s = alloc_s(c);
        vldst_sp(c, true, s, slot_off(c, v->depth));
        break;
    }

  v->kind = V_REG;
  v->r = (int8_t)s;
  return s;
}

/* A word (half 0/1 of a 64-bit value) of a value into a core register */

static void word_to(jc_t *c, int dst, val_t *v, int half)
{
  switch (v->kind)
    {
      case V_REG:
        if (v->type == T_F32)
          {
            t_vmov_rs(&c->fb, dst, v->r);
          }
        else
          {
            t_mov(&c->fb, dst, half ? v->r2 : v->r);
          }
        break;

      case V_CONST:
        t_movi(&c->fb, dst, (uint32_t)(v->c >> (32 * half)));
        break;

      case V_LOCK:
        t_addi(&c->fb, dst, c->lreg[v->local], (int32_t)v->c);
        break;

      case V_LOCAL:
        if (c->lkind[v->local] == L_CORE)
          {
            t_mov(&c->fb, dst, half ? c->lreg2[v->local] :
                  c->lreg[v->local]);
          }
        else if (c->lkind[v->local] == L_SREG)
          {
            t_vmov_rs(&c->fb, dst, c->lreg[v->local]);
          }
        else
          {
            ld_sp(c, dst, c->loff[v->local] + 4 * half);
          }
        break;

      default:
        ld_sp(c, dst, slot_off(c, v->depth) + 4 * half);
        break;
    }
}

/* The core register a word of a value is in, or -1 */

static int word_reg(jc_t *c, val_t *v, int half)
{
  if (v->kind == V_REG && v->type != T_F32)
    {
      return half ? v->r2 : v->r;
    }

  if (v->kind == V_LOCAL && c->lkind[v->local] == L_CORE)
    {
      return half ? c->lreg2[v->local] : c->lreg[v->local];
    }

  return -1;
}

/* Moves of value words into core registers, all at once */

typedef struct pmove_s
{
  int dst;
  int src;                      /* Core register, or -1: from v */
  val_t *v;
  int half;
} pmove_t;

static void pmoves(jc_t *c, pmove_t *m, int n)
{
  bool done[8];
  int left = 0;
  int i;
  int j;

  for (i = 0; i < n; i++)
    {
      done[i] = m[i].src < 0;
      left += !done[i];
    }

  while (left > 0)
    {
      bool progress = false;

      for (i = 0; i < n; i++)
        {
          bool blocked = false;

          if (done[i])
            {
              continue;
            }

          if (m[i].src == m[i].dst)
            {
              done[i] = true;
              left--;
              progress = true;
              continue;
            }

          for (j = 0; j < n; j++)
            {
              if (j != i && !done[j] && m[j].src == m[i].dst)
                {
                  blocked = true;
                  break;
                }
            }

          if (!blocked)
            {
              t_mov(&c->fb, m[i].dst, m[i].src);
              done[i] = true;
              left--;
              progress = true;
            }
        }

      if (!progress)
        {
          /* A cycle: one source to ip */

          for (i = 0; i < n && done[i]; i++);
          t_mov(&c->fb, R_IP, m[i].src);
          for (j = n - 1; j >= 0; j--)
            {
              if (!done[j] && m[j].src == m[i].src && j != i)
                {
                  m[j].src = R_IP;
                }
            }

          m[i].src = R_IP;
        }
    }

  for (i = 0; i < n; i++)
    {
      if (m[i].v != NULL && m[i].src < 0)
        {
          word_to(c, m[i].dst, m[i].v, m[i].half);
        }
    }
}

static void pm_add(jc_t *c, pmove_t *m, int *n, int dst, val_t *v, int half)
{
  m[*n].dst  = dst;
  m[*n].v    = v;
  m[*n].half = half;
  m[*n].src  = word_reg(c, v, half);
  (*n)++;
}

/****************************************************************************
 * Calls
 ****************************************************************************/

/* The locals in caller-saved registers to their homes before a call, and
 * back after it
 */

static void save_csl(jc_t *c)
{
  uint32_t i;

  for (i = 0; i < c->ncsl; i++)
    {
      st_sp(c, c->lreg[c->csl[i]], c->loff[c->csl[i]]);
    }
}

static void load_csl(jc_t *c)
{
  uint32_t i;

  for (i = 0; i < c->ncsl; i++)
    {
      ld_sp(c, c->lreg[c->csl[i]], c->loff[c->csl[i]]);
    }
}

/* A C helper: args are words of values ('w'), 64-bit values ('l', an even
 * register pair) or the instance ('I'); the result in r0 (and r1)
 */

static void ccall(jc_t *c, void *fn, const char *sig, val_t *args, int rtype,
                  bool has_result)
{
  pmove_t m[8];
  val_t inst;
  int n = 0;
  int reg = 0;
  int ai = 0;
  int i;

  spill_caller_saved(c, c->vsp);
  memset(&inst, 0, sizeof(inst));
  inst.kind = V_REG;
  inst.type = T_I32;
  inst.r    = JR_INST;

  for (i = 0; sig[i] != '\0'; i++)
    {
      if (sig[i] == 'I')
        {
          pm_add(c, m, &n, reg++, &inst, 0);
        }
      else if (sig[i] == 'w' && reg >= 4)
        {
          /* On the stack (the outgoing area) */

          val_t *v = &args[ai++];
          int r = word_reg(c, v, 0);

          if (r < 0)
            {
              word_to(c, R_IP, v, 0);
              r = R_IP;
            }

          st_sp(c, r, 4 * (reg++ - 4));
        }
      else if (sig[i] == 'w')
        {
          pm_add(c, m, &n, reg++, &args[ai++], 0);
        }
      else
        {
          reg = (reg + 1) & ~1;
          pm_add(c, m, &n, reg, &args[ai], 0);
          pm_add(c, m, &n, reg + 1, &args[ai], 1);
          reg += 2;
          ai++;
        }
    }

  save_csl(c);
  pmoves(c, m, n);
  c->kvalid = 0;
  for (i = 0; i < ai; i++)
    {
      free_val(c, &args[i]);
    }

  c->makes_calls = true;
  t_movi(&c->fb, R_IP, (uint32_t)(uintptr_t)fn);
  t_blx(&c->fb, R_IP);
  load_csl(c);

  if (has_result)
    {
      if (rtype == T_F32)
        {
          int s = alloc_s(c);

          t_vmov_sr(&c->fb, s, R0);
          push_reg(c, T_F32, s, -1);
        }
      else if (IS64(rtype))
        {
          take_core(c, R0);
          take_core(c, R1);
          push_reg(c, rtype, R0, R1);
        }
      else
        {
          take_core(c, R0);
          push_reg(c, T_I32, R0, -1);
        }
    }
}

/* Words of a function type's values */

static void push_results(jc_t *c, WASMFuncType *t)
{
  const uint8_t *rt = t->types + t->param_count;
  uint32_t rw = type_words(rt, t->result_count);
  uint32_t i;
  uint32_t w = 0;

  if (rw <= 2)
    {
      for (i = 0; i < t->result_count; i++)
        {
          int ty = jtype(rt[i]);

          if (ty == T_F32)
            {
              int s = alloc_s(c);

              t_vmov_sr(&c->fb, s, R0 + w);
              push_reg(c, T_F32, s, -1);
            }
          else if (IS64(ty))
            {
              take_core(c, R0);
              take_core(c, R1);
              push_reg(c, ty, R0, R1);
            }
          else
            {
              take_core(c, R0 + w);
              push_reg(c, T_I32, R0 + w, -1);
            }

          w += TWORDS(ty);
        }

      return;
    }

  /* In the outgoing area: into their slots */

  for (i = 0; i < t->result_count; i++)
    {
      int ty = jtype(rt[i]);
      val_t *v = vpush(c, V_SLOT, ty);
      uint32_t off = slot_off(c, v->depth);
      uint32_t k;

      for (k = 0; k < (uint32_t)TWORDS(ty); k++)
        {
          ld_sp(c, R_IP, 4 * (w + k));
          st_sp(c, R_IP, off + 4 * k);
        }

      w += TWORDS(ty);
    }
}

/* The arguments of a call (the top of the stack) to where the callee
 * wants them: r0-r3, then the outgoing area.  Pops them.
 */

static void call_args(jc_t *c, WASMFuncType *t)
{
  uint32_t na = t->param_count;
  uint32_t base = c->vsp - na;
  uint32_t aw = type_words(t->types, na);
  uint32_t rw = type_words(t->types + na, t->result_count);
  pmove_t m[8];
  int n = 0;
  uint32_t w = 0;
  uint32_t i;
  uint32_t k;

  if ((aw > 4 ? aw - 4 : 0) > c->out_words || (rw > 2 && rw > c->out_words))
    {
      fail(c, "outgoing area", 0);
      return;
    }

  spill_caller_saved(c, base);

  for (i = 0; i < na; i++)
    {
      val_t *v = &c->vs[base + i];

      for (k = 0; k < (uint32_t)TWORDS(v->type); k++, w++)
        {
          if (w < 4)
            {
              pm_add(c, m, &n, R0 + w, v, k);
            }
          else
            {
              int r = word_reg(c, v, k);

              if (r < 0 && v->kind == V_REG && v->type == T_F32)
                {
                  vldst_sp(c, false, v->r, 4 * (w - 4));
                  continue;
                }

              if (r < 0)
                {
                  word_to(c, R_IP, v, k);
                  r = R_IP;
                }

              st_sp(c, r, 4 * (w - 4));
            }
        }
    }

  save_csl(c);
  pmoves(c, m, n);
  c->kvalid = 0;
  for (i = 0; i < na; i++)
    {
      val_t v = pop(c);

      free_val(c, &v);
    }

  c->makes_calls = true;
}

static void op_call(jc_t *c, uint32_t idx)
{
  WASMFuncType *t = func_type_of(c->mod, idx);

  call_args(c, t);
  call_func(c, idx);
  load_csl(c);
  if (c->may_grow == NULL || c->may_grow[idx])
    {
      reload_mem(c, &c->fb);      /* (the memory may have moved) */
    }

  push_results(c, t);
}

static uint32_t table_off(jc_t *c, uint32_t tbl)
{
  return (uint32_t)((uint8_t *)c->inst->tables[tbl] - (uint8_t *)c->inst);
}

static void op_call_indirect(jc_t *c, uint32_t tidx, uint32_t tbl)
{
  WASMFuncType *t = (WASMFuncType *)c->mod->types[tidx];
  uint32_t toff = table_off(c, tbl);
  uint32_t i;
  val_t vi;
  int ri;
  int enc;

  /* lr will hold the callee's address: nothing else may be there */

  for (i = 0; i < c->vsp; i++)
    {
      val_t *v = &c->vs[i];

      if (v->kind == V_REG && v->type != T_F32 &&
          (v->r == R_LR || (IS64(v->type) && v->r2 == R_LR)))
        {
          spill_at(c, i);
        }
    }

  vi = pop(c);
  ri = vreg(c, &vi);
  if (ri == R_LR)
    {
      ri = alloc_core(c);
      t_mov(&c->fb, ri, R_LR);
      free_core(c, R_LR);
      vi.r = (int8_t)ri;
    }

  c->cfree &= ~(1u << R_LR);    /* Held below */
  c->kvalid = 0;

  /* idx < size, the entry set, a function of this module, the type */

  ld_inst(c, LS_LDR, R_IP, toff + offsetof(WASMTableInstance, cur_size));
  t_cmp(&c->fb, ri, R_IP);
  trap_if(c, CC_HS, JT_UNDEF_ELEM);
  t_addi(&c->fb, R_IP, JR_INST, (int32_t)(toff +
                                  offsetof(WASMTableInstance, elems)));
  t_ldst_reg(&c->fb, LS_LDR, R_LR, R_IP, ri, 2);
  t_cmni_enc(&c->fb, R_LR, 1);
  trap_if(c, CC_EQ, JT_UNINIT_ELEM);
  t_movi(&c->fb, R_IP, c->jm->nfuncs);
  t_cmp(&c->fb, R_LR, R_IP);
  trap_if(c, CC_HS, JT_UNKNOWN_FUNC);
  free_val(c, &vi);
  ld_inst(c, LS_LDR, R_IP, EXT_OFF(EXT_FUNCS));
  t_dp_reg(&c->fb, DP_ADD, 0, R_LR, R_IP, R_LR, SH_LSL, 3);
  t_ldst(&c->fb, LS_LDR, R_IP, R_LR, 4);
  enc = t_modimm(c->typeid[tidx]);
  if (enc >= 0)
    {
      t_cmpi_enc(&c->fb, R_IP, enc);
      trap_if(c, CC_NE, JT_TYPE);
      t_ldst(&c->fb, LS_LDR, R_LR, R_LR, 0);
    }
  else
    {
      t_ldst(&c->fb, LS_LDR, R_LR, R_LR, 0);
      e32(&c->fb, 0xf84d, 0xed04);            /* push {lr} (str lr, [sp,#-4]!) */
      t_movi(&c->fb, R_LR, c->typeid[tidx]);
      t_cmp(&c->fb, R_IP, R_LR);
      e32(&c->fb, 0xf85d, 0xeb04);            /* pop {lr} (ldr lr, [sp],#4) */
      trap_if(c, CC_NE, JT_TYPE);
    }

  call_args(c, t);
  t_blx(&c->fb, R_LR);
  load_csl(c);
  if (c->ctemp & (1u << R_LR))
    {
      c->cfree |= 1u << R_LR;
    }

  reload_mem(c, &c->fb);
  push_results(c, t);
}

/****************************************************************************
 * Control flow
 ****************************************************************************/

static uint8_t ctrl_rtype(ctrl_t *ct, uint32_t i)
{
  return ct->rt[i];
}

static uint32_t br_arity(ctrl_t *ct)
{
  return ct->kind == CK_LOOP ? ct->np : ct->nr;
}

/* A block's one i32 result in a register (rreg): chosen at its start,
 * a free temporary; every branch to its end (and the fall through) puts
 * the result there, what else is in it spilled first
 */

static void res_free(jc_t *c, ctrl_t *ct)
{
  uint32_t i;

  for (i = 0; i + 1 < c->vsp; i++)
    {
      val_t *v = &c->vs[i];

      if (v->kind == V_REG && v->type != T_F32 &&
          (v->r == ct->rreg || (IS64(v->type) && v->r2 == ct->rreg)))
        {
          spill_at(c, i);
        }
    }
}

static void res_move(jc_t *c, ctrl_t *ct)
{
  val_t *v = &c->vs[c->vsp - 1];

  if (v->kind == V_REG && v->r == ct->rreg)
    {
      return;
    }

  res_free(c, ct);
  word_to(c, ct->rreg, v, 0);
  c->kvalid &= ~(1u << ct->rreg);
}

static void res_choose(jc_t *c, ctrl_t *ct)
{
  static const int8_t order[] =
  {
    3, 2, 1, 0, 14, 8, 7, 6, 5, 4
  };

  uint32_t i;

  ct->rreg = -1;
  if ((ct->kind != CK_BLOCK && ct->kind != CK_IF) || ct->nr != 1 ||
      ct->np != 0 || jtype(ct->rt[0]) != T_I32)
    {
      return;
    }

  for (i = 0; i < sizeof(order); i++)
    {
      int r = order[i];

      if (c->ctemp & c->cfree & (1u << r))
        {
          ct->rreg = (int8_t)r;
          if (r >= 4 && r <= 8)
            {
              c->cused |= 1u << r;          /* (the prologue saves it) */
            }

          if (r == R_LR)
            {
              c->lr_used = true;
            }

          return;
        }
    }
}

/* Whether a branch to ct must move values */

static bool br_needs_moves(jc_t *c, ctrl_t *ct)
{
  uint32_t n = br_arity(ct);
  uint32_t i;

  if (ct->rreg >= 0 && ct->kind != CK_LOOP)
    {
      val_t *v = &c->vs[c->vsp - 1];

      return v->kind != V_REG || v->r != ct->rreg;
    }

  for (i = 0; i < n; i++)
    {
      val_t *v = &c->vs[c->vsp - n + i];

      if (v->kind != V_SLOT || v->depth != ct->height + i)
        {
          return true;
        }
    }

  return false;
}

/* The branch's values into the target's slots */

static void br_moves(jc_t *c, ctrl_t *ct)
{
  uint32_t n = br_arity(ct);
  uint32_t i;

  if (ct->rreg >= 0 && ct->kind != CK_LOOP)
    {
      res_move(c, ct);
      return;
    }

  for (i = 0; i < n; i++)
    {
      store_slot(c, &c->vs[c->vsp - n + i], slot_off(c, ct->height + i));
    }
}

/* The results into r0/r1 (or the slots for more than 2 words) */

static void ret_moves(jc_t *c)
{
  WASMFuncType *t = c->ftype;
  uint32_t nr = t->result_count;
  const uint8_t *rt = t->types + t->param_count;
  pmove_t m[8];
  int n = 0;
  uint32_t w = 0;
  uint32_t i;
  uint32_t k;

  if (type_words(rt, nr) > 2)
    {
      for (i = 0; i < nr; i++)
        {
          store_slot(c, &c->vs[c->vsp - nr + i], slot_off(c, i));
        }

      return;
    }

  for (i = 0; i < nr; i++)
    {
      val_t *v = &c->vs[c->vsp - nr + i];

      for (k = 0; k < (uint32_t)TWORDS(v->type); k++)
        {
          pm_add(c, m, &n, R0 + w++, v, k);
        }
    }

  pmoves(c, m, n);
}

static ctrl_t *ctrl_at(jc_t *c, uint32_t depth)
{
  return &c->cs[c->csp - 1 - depth];
}

static void op_br(jc_t *c, uint32_t depth)
{
  ctrl_t *ct = ctrl_at(c, depth);

  if (ct->kind == CK_FUNC)
    {
      ret_moves(c);
      br_to(c, c->epilogue);
    }
  else
    {
      br_moves(c, ct);
      br_to(c, ct->label);
    }

  c->cs[c->csp - 1].unreach = true;
}

/* br_if on a condition (flags set) */

static void br_cond(jc_t *c, uint32_t depth, int cond)
{
  ctrl_t *ct = ctrl_at(c, depth);
  int skip;

  if (ct->kind != CK_FUNC && !br_needs_moves(c, ct))
    {
      bcc_to(c, cond, ct->label);
      return;
    }

  if (ct->kind != CK_FUNC && ct->rreg >= 0)
    {
      res_free(c, ct);            /* (both ways on: stores keep the flags) */
    }

  skip = new_label(c);
  bcc_to(c, CC_INV(cond), skip);
  if (ct->kind == CK_FUNC)
    {
      ret_moves(c);
      br_to(c, c->epilogue);
    }
  else
    {
      br_moves(c, ct);
      br_to(c, ct->label);
    }

  lbind(c, skip);
}

/* Set the flags from a value: true is NE */

static int test_val(jc_t *c, val_t *v)
{
  int r = vreg(c, v);

  t_cmpi_enc(&c->fb, r, 0);
  free_val(c, v);
  return CC_NE;
}

static void push_ctrl(jc_t *c, int kind, const uint8_t **pp)
{
  const uint8_t *p = *pp;
  ctrl_t *ct;

  if (!GROW(c, c->cs, c->csp, c->ccap))
    {
      return;
    }

  ct = &c->cs[c->csp++];
  memset(ct, 0, sizeof(*ct));
  ct->kind = (uint8_t)kind;
  ct->label = -1;
  ct->else_label = -1;
  ct->rreg = -1;
  ct->rt = &ct->rt1;

  if (*p == 0x40)
    {
      p++;
    }
  else if ((*p & 0xc0) == 0x40)
    {
      ct->rt1 = *p++;
      ct->nr = 1;
    }
  else
    {
      uint32_t ti = (uint32_t)rd_s64(&p, 33);
      WASMFuncType *t = (WASMFuncType *)c->mod->types[ti];

      ct->np = t->param_count;
      ct->nr = t->result_count;
      ct->pt = t->types;
      ct->rt = t->types + t->param_count;
    }

  ct->height = c->vsp >= ct->np ? c->vsp - ct->np : 0;
  *pp = p;
}

/* The stack back to a block's start, its parameters (or results) in their
 * slots
 */

static void reset_stack(jc_t *c, ctrl_t *ct, bool results)
{
  uint32_t n = results ? ct->nr : ct->np;
  uint32_t i;

  while (c->vsp > ct->height)
    {
      val_t v = pop(c);

      free_val(c, &v);
    }

  if (results && ct->rreg >= 0 && ct->kind != CK_LOOP)
    {
      push_reg(c, T_I32, take_core(c, ct->rreg), -1);
      return;
    }

  for (i = 0; i < n; i++)
    {
      vpush(c, V_SLOT, jtype(results ? ctrl_rtype(ct, i) : ct->pt[i]));
    }
}

static void op_end(jc_t *c)
{
  ctrl_t *ct = &c->cs[c->csp - 1];
  label_t *lb;

  if (ct->dead)
    {
      c->csp--;
      return;
    }

  if (ct->kind == CK_FUNC)
    {
      if (!ct->unreach)
        {
          ret_moves(c);
        }

      while (c->vsp > 0)
        {
          val_t v = pop(c);

          free_val(c, &v);
        }

      c->csp--;
      return;
    }

  if (ct->kind == CK_LOOP)
    {
      /* Nothing branches to a loop's end: its results stay as they are */

      c->csp--;
      if (ct->unreach && c->csp > 0)
        {
          c->cs[c->csp - 1].unreach = true;
        }

      return;
    }

  lb = &c->labels[ct->label];
  if (ct->kind == CK_BLOCK && lb->fix < 0 && !ct->unreach)
    {
      /* Nothing branched out: the values stay where they are */

      c->csp--;
      return;
    }

  if (!ct->unreach)
    {
      br_moves(c, ct);
    }

  if (ct->kind == CK_IF)
    {
      lbind(c, ct->else_label);    /* No else: the parameters pass */
    }

  lbind(c, ct->label);
  reset_stack(c, ct, true);
  {
    bool reach = !ct->unreach || ct->kind == CK_IF ||
                 c->labels[ct->label].pos >= 0;

    /* (a bound label: something branched here, or fell through) */

    c->csp--;
    if (c->csp > 0)
      {
        c->cs[c->csp - 1].unreach = !reach;
      }
  }
}

/****************************************************************************
 * Memory
 ****************************************************************************/

/* Check [addr + offset, + size) against the memory; the address to use:
 * [*base, *reg] (*reg >= 0), or [*base, #*imm] (and #*imm + 4 for the
 * second word of 8 bytes).  r9 is the memory's size - 3: a 4-byte access
 * below it is in; 8 bytes check their second word against it; 1 and 2
 * bytes, the last 3 of the memory, are checked exactly out of line.
 */

/* A branch (flags set: HS) to an exact check out of line, which comes
 * back having changed nothing
 */

static void add_cold(jc_t *c, int mode, int ra, uint32_t k, uint32_t size)
{
  cold_t *cs;
  uint32_t n;
  uint32_t kv;
  __typeof__(c->bc) bc;

  if (!GROW(c, c->cold, c->ncold, c->coldcap))
    {
      return;
    }

  cs = &c->cold[c->ncold++];
  cs->entry = new_label(c);
  cs->back  = new_label(c);
  cs->ra    = (int8_t)ra;
  cs->size  = (uint8_t)size;
  cs->k     = k;
  cs->mode  = (uint8_t)mode;
  bcc_to(c, CC_HS, cs->entry);

  n  = c->nbc;
  kv = c->kvalid;
  memcpy(bc, c->bc, sizeof(bc));
  lbind(c, cs->back);
  memcpy(c->bc, bc, sizeof(bc));
  c->nbc = n;
  c->kvalid = kv;
}

static void mem_addr(jc_t *c, val_t *addr, uint32_t offset, uint32_t size,
                     int *base, int *reg, int *imm)
{
  uint64_t k = (uint64_t)offset + (size == 8 ? 4 : 0);
  int ra;
  int enc;

  if (!c->has_mem)
    {
      fail(c, "memory access without a memory", 0);
      return;
    }

  ra = vreg(c, addr);
  *base = JR_MEM;
  *imm  = 0;
  if (k > 0xffffffffu)
    {
      trap_if(c, CC_AL, JT_OOB);
      *reg = ra;
      return;
    }

  /* A local's register already checked far enough: no check */

  if (addr->kind == V_LOCAL && c->lkind[addr->local] == L_CORE &&
      (uint64_t)offset + size <= 0xffffffffu)
    {
      uint32_t i;

      for (i = 0; i < c->nbc; i++)
        {
          if (c->bc[i].reg == ra && offset + size <= c->bc[i].end)
            {
              if (offset == 0 && size != 8)
                {
                  *reg = ra;
                }
              else if (offset <= 4091)
                {
                  t_dp_reg(&c->fb, DP_ADD, 0, R_IP, JR_MEM, ra, SH_LSL, 0);
                  *base = R_IP;
                  *reg  = -1;
                  *imm  = (int)offset;
                }
              else
                {
                  /* ip = ra + offset (t_addi cannot add to ip itself an
                   * offset that takes ip to build) */

                  t_movi(&c->fb, R_IP, offset);
                  t_dp_reg(&c->fb, DP_ADD, 0, R_IP, R_IP, ra, SH_LSL, 0);
                  *reg = R_IP;
                  if (size == 8)
                    {
                      e16(&c->fb, 0x4400 | 0x80 | JR_MEM << 3 | (R_IP & 7));
                      *base = R_IP;                    /* (add ip, r10) */
                      *reg  = -1;
                    }
                }

              return;
            }
        }
    }

  if (c->guard != 0 && (uint64_t)offset + size <= c->guard)
    {
      /* ra < r9: [ra, ra + JIT_GUARD) is in */

      t_cmp(&c->fb, ra, JR_LIM4);
      add_cold(c, COLD_GUARD, ra, offset, size);
      if (offset == 0 && size != 8)
        {
          *reg = ra;
        }
      else
        {
          t_dp_reg(&c->fb, DP_ADD, 0, R_IP, JR_MEM, ra, SH_LSL, 0);
          *base = R_IP;
          *reg  = -1;
          *imm  = (int)offset;
        }

      goto proved;
    }

  if (c->guard != 0)
    {
      /* A large offset: ip = ra + offset (past 4 GB: out), then ip < r9 */

      if ((uint64_t)offset + size > 0xffffffffu)
        {
          trap_if(c, CC_AL, JT_OOB);
          *reg = ra;
          return;
        }

      enc = t_modimm(offset);
      if (enc >= 0)
        {
          t_dp_imm(&c->fb, DP_ADD, 1, R_IP, ra, enc);       /* adds */
        }
      else
        {
          t_movi(&c->fb, R_IP, offset);
          t_dp_reg(&c->fb, DP_ADD, 1, R_IP, ra, R_IP, SH_LSL, 0);
        }

      trap_if(c, CC_HS, JT_OOB);
      t_cmp(&c->fb, R_IP, JR_LIM4);
      add_cold(c, COLD_GUARD_IP, R_IP, 0, size);
      *reg = R_IP;
      if (size == 8)
        {
          e16(&c->fb, 0x4400 | 0x80 | JR_MEM << 3 | (R_IP & 7));
          *base = R_IP;                         /* (add ip, r10) */
          *reg  = -1;
        }

      goto proved;
    }

  if (k == 0)
    {
      t_cmp(&c->fb, ra, JR_LIM4);
      *reg = ra;
    }
  else
    {
      enc = t_modimm((uint32_t)k);
      if (enc >= 0)
        {
          t_dp_imm(&c->fb, DP_ADD, 1, R_IP, ra, enc);       /* adds */
        }
      else
        {
          t_movi(&c->fb, R_IP, (uint32_t)k);
          t_dp_reg(&c->fb, DP_ADD, 1, R_IP, ra, R_IP, SH_LSL, 0);
        }

      t_it(&c->fb, CC_LO, "t");
      t_cmp(&c->fb, R_IP, JR_LIM4);                       /* cmpcc */
      *reg = R_IP;
    }

  if (size >= 4)
    {
      trap_if(c, CC_HS, JT_OOB);
    }
  else
    {
      add_cold(c, COLD_EXACT, ra, (uint32_t)k, size);
    }

  if (size == 8)
    {
      e16(&c->fb, 0x4400 | 0x80 | JR_MEM << 3 | (R_IP & 7));  /* add ip, r10 */
      *base = R_IP;
      *reg  = -1;
      *imm  = -4;
    }

  /* What this proved, of a local's register */

proved:
  if (addr->kind == V_LOCAL && c->lkind[addr->local] == L_CORE)
    {
      uint32_t end = offset + size;     /* (no wrap: k <= 2^32 - 1) */
      uint32_t i;

      for (i = 0; i < c->nbc && c->bc[i].reg != ra; i++);
      if (i == c->nbc && c->nbc < sizeof(c->bc) / sizeof(c->bc[0]))
        {
          c->bc[c->nbc].reg = (int8_t)ra;
          c->bc[c->nbc].end = 0;
          c->nbc++;
        }

      if (i < c->nbc && end > c->bc[i].end)
        {
          c->bc[i].end = end;
        }
    }
}

/* The exact checks of 1 and 2-byte accesses near the memory's end */

static void emit_cold(jc_t *c)
{
  uint32_t i;

  for (i = 0; i < c->ncold; i++)
    {
      cold_t *cs = &c->cold[i];
      int ea = cs->k ? R_IP : cs->ra;

      if (cs->mode == COLD_GUARD)
        {
          /* ra + k + size <= the size: ra <= size - k - size (the size is
           * at least 64 KB: no borrow)
           */

          lbind(c, cs->entry);
          t_addi(&c->fb, R_IP, JR_LIM4, (int32_t)(c->guard - cs->k -
                                                   cs->size));
          t_cmp(&c->fb, cs->ra, R_IP);      /* (r9 + guard: the size) */
          trap_if(c, CC_HI, JT_OOB);
          br_back(c, cs->back);
          continue;
        }

      if (cs->mode == COLD_GUARD_IP)
        {
          lbind(c, cs->entry);
          e32(&c->fb, 0xf84d, 0xed08);             /* str lr, [sp, #-8]! */
          t_ldst(&c->fb, LS_LDR, R_LR, JR_INST, c->mem_off +
                 offsetof(WASMMemoryInstance, memory_data_size));
          t_addi(&c->fb, R_LR, R_LR, -(int32_t)cs->size);
          t_cmp(&c->fb, R_IP, R_LR);
          e32(&c->fb, 0xf85d, 0xeb08);             /* ldr lr, [sp], #8 */
          trap_if(c, CC_HI, JT_OOB);
          br_to(c, cs->back);
          continue;
        }

      lbind(c, cs->entry);
      if (cs->k)
        {
          int enc = t_modimm(cs->k);

          if (enc >= 0)
            {
              t_dp_imm(&c->fb, DP_ADD, 1, R_IP, cs->ra, enc);
            }
          else
            {
              t_movi(&c->fb, R_IP, cs->k);
              t_dp_reg(&c->fb, DP_ADD, 1, R_IP, cs->ra, R_IP, SH_LSL, 0);
            }

          trap_if(c, CC_HS, JT_OOB);                       /* (wrapped) */
        }

      /* The memory's size into a scratch register: ip when the address
       * is in lr (k = 0), else lr (saved around it)
       */

      {
        int sc = ea == R_LR ? R_IP : R_LR;

        if (sc == R_LR)
          {
            e32(&c->fb, 0xf84d, 0xed08);           /* str lr, [sp, #-8]! */
          }

        t_ldst(&c->fb, LS_LDR, sc, JR_INST, c->mem_off +
               offsetof(WASMMemoryInstance, memory_data_size));
        if (cs->size == 2)
          {
            t_dp_imm(&c->fb, DP_SUB, 1, sc, sc, 1);         /* subs */
            trap_if(c, CC_LO, JT_OOB);    /* (a trap restores sp) */
          }

        t_cmp(&c->fb, ea, sc);
        if (sc == R_LR)
          {
            e32(&c->fb, 0xf85d, 0xeb08);           /* ldr lr, [sp], #8 */
          }
      }

      trap_if(c, CC_HS, JT_OOB);
      br_to(c, cs->back);
    }
}

static void op_load(jc_t *c, uint8_t op, uint32_t offset)
{
  static const struct
  {
    uint8_t type;
    uint8_t size;
    uint8_t ls;
  }
  ld[] =
  {
    { T_I32, 4, LS_LDR },       /* i32.load */
    { T_I64, 8, LS_LDR },       /* i64.load */
    { T_F32, 4, LS_LDR },       /* f32.load */
    { T_F64, 8, LS_LDR },       /* f64.load */
    { T_I32, 1, LS_LDRSB },     /* i32.load8_s */
    { T_I32, 1, LS_LDRB },
    { T_I32, 2, LS_LDRSH },
    { T_I32, 2, LS_LDRH },
    { T_I64, 1, LS_LDRSB },     /* i64.load8_s */
    { T_I64, 1, LS_LDRB },
    { T_I64, 2, LS_LDRSH },
    { T_I64, 2, LS_LDRH },
    { T_I64, 4, LS_LDR },       /* i64.load32_s */
    { T_I64, 4, LS_LDR },       /* i64.load32_u */
  };

  int i = op - WASM_OP_I32_LOAD;
  int ty = ld[i].type;
  int size = ld[i].size;
  val_t a = pop(c);
  int base;
  int reg;
  int imm;
  int rd;
  int rh;

  mem_addr(c, &a, offset, size, &base, &reg, &imm);
  if (size == 8)
    {
      free_val(c, &a);
      rd = alloc_core(c);
      rh = alloc_core(c);
      t_ldst(&c->fb, LS_LDR, rd, base, imm);
      t_ldst(&c->fb, LS_LDR, rh, base, imm + 4);
      push_reg(c, ty, rd, rh);
      return;
    }

  free_val(c, &a);
  rd = ty == T_I32 ? alloc_dest(c) : alloc_core(c);
  if (reg >= 0)
    {
      t_ldst_reg(&c->fb, ld[i].ls, rd, base, reg, 0);
    }
  else
    {
      t_ldst(&c->fb, ld[i].ls, rd, base, imm);
    }

  if (ty == T_F32)
    {
      int s = alloc_s(c);

      t_vmov_sr(&c->fb, s, rd);
      free_core(c, rd);
      push_reg(c, T_F32, s, -1);
    }
  else if (ty == T_I64)
    {
      rh = alloc_core(c);
      if (op == WASM_OP_I64_LOAD8_U || op == WASM_OP_I64_LOAD16_U ||
          op == WASM_OP_I64_LOAD32_U)
        {
          t_movi(&c->fb, rh, 0);
        }
      else
        {
          t_shi(&c->fb, SH_ASR, rh, rd, 31);
        }

      push_reg(c, T_I64, rd, rh);
    }
  else
    {
      push_reg(c, T_I32, rd, -1);
    }
}

static void op_store(jc_t *c, uint8_t op, uint32_t offset)
{
  static const uint8_t sz[] =
  {
    4, 8, 4, 8, 1, 2, 1, 2, 4
  };

  static const uint8_t ls[] =
  {
    LS_STR, LS_STR, LS_STR, LS_STR, LS_STRB, LS_STRH, LS_STRB, LS_STRH, LS_STR
  };

  int i = op - WASM_OP_I32_STORE;
  int size = sz[i];
  val_t v = pop(c);
  val_t a = pop(c);
  int base;
  int reg;
  int imm;
  int rv;
  int rh = -1;

  if (v.type == T_F32)
    {
      /* Through a core register: VSTR faults on an unaligned address */

      if (v.kind == V_REG || (v.kind == V_LOCAL &&
                              c->lkind[v.local] == L_SREG))
        {
          int s = vsreg(c, &v);

          rv = alloc_core(c);
          t_vmov_rs(&c->fb, rv, s);
          free_val(c, &v);
          v.kind = V_REG;
          v.type = T_I32;
          v.r = (int8_t)rv;
        }
      else
        {
          v.type = T_I32;       /* The bits */
          rv = vreg(c, &v);
        }
    }
  else if (IS64(v.type))
    {
      if (size == 8)
        {
          vreg64(c, &v, &rv, &rh);
        }
      else
        {
          /* The low word */

          if (v.kind == V_REG)
            {
              free_core(c, v.r2);
              v.type = T_I32;
            }
          else if (v.kind == V_CONST)
            {
              v.type = T_I32;
              v.c &= 0xffffffffu;
            }
          else
            {
              int r = alloc_core(c);

              word_to(c, r, &v, 0);
              v.kind = V_REG;
              v.type = T_I32;
              v.r = (int8_t)r;
            }

          rv = vreg(c, &v);
        }
    }
  else
    {
      rv = vreg(c, &v);
    }

  mem_addr(c, &a, offset, size, &base, &reg, &imm);
  if (size == 8)
    {
      t_ldst(&c->fb, LS_STR, rv, base, imm);
      t_ldst(&c->fb, LS_STR, rh, base, imm + 4);
    }
  else if (reg >= 0)
    {
      t_ldst_reg(&c->fb, ls[i], rv, base, reg, 0);
    }
  else
    {
      t_ldst(&c->fb, ls[i], rv, base, imm);
    }

  free_val(c, &a);
  free_val(c, &v);
}

/****************************************************************************
 * Integer arithmetic
 ****************************************************************************/

/* cmp ra, #k (or cmn, or through ip) */

static void cmp_imm(jc_t *c, int ra, uint32_t k)
{
  int enc = t_modimm(k);

  if (enc >= 0)
    {
      t_cmpi_enc(&c->fb, ra, enc);
      return;
    }

  enc = t_modimm(-k);
  if (enc >= 0)
    {
      t_cmni_enc(&c->fb, ra, enc);
      return;
    }

  t_cmp(&c->fb, ra, const_reg(c, k, 1u << ra));
}

/* Materialize a condition as 0/1 */

static void push_cond(jc_t *c, int cond)
{
  int rd = alloc_dest(c);

  t_it(&c->fb, cond, "te");
  t_movi_it(&c->fb, rd, 1);
  t_movi_it(&c->fb, rd, 0);
  push_reg(c, T_I32, rd, -1);
}

/* What follows a comparison: a fused br_if, if or select, or a value */

static void use_cond(jc_t *c, int cond, const uint8_t **pp);

/* i32 comparisons: the condition the flags give */

static int cmp_i32(jc_t *c, uint8_t op)
{
  static const uint8_t cc[] =
  {
    CC_EQ, CC_NE, CC_LT, CC_LO, CC_GT, CC_HI, CC_LE, CC_LS, CC_GE, CC_HS
  };

  static const uint8_t swapped[] =
  {
    CC_EQ, CC_NE, CC_GT, CC_HI, CC_LT, CC_LO, CC_GE, CC_HS, CC_LE, CC_LS
  };

  val_t b = pop(c);
  val_t a = pop(c);
  int i = op - WASM_OP_I32_EQ;

  if (b.kind == V_CONST)
    {
      int ra = vreg(c, &a);

      cmp_imm(c, ra, (uint32_t)b.c);
      free_val(c, &a);
      return cc[i];
    }

  if (a.kind == V_CONST)
    {
      int rb = vreg(c, &b);

      cmp_imm(c, rb, (uint32_t)a.c);
      free_val(c, &b);
      return swapped[i];
    }

  {
    int ra = vreg(c, &a);
    int rb = vreg(c, &b);

    t_cmp(&c->fb, ra, rb);
    free_val(c, &a);
    free_val(c, &b);
  }

  return cc[i];
}

static int cmp_i64(jc_t *c, uint8_t op)
{
  val_t b = pop(c);
  val_t a = pop(c);
  int alo;
  int ahi;
  int blo;
  int bhi;
  int cond;

  vreg64(c, &a, &alo, &ahi);
  vreg64(c, &b, &blo, &bhi);

  switch (op)
    {
      case WASM_OP_I64_EQ:
      case WASM_OP_I64_NE:
        t_cmp(&c->fb, alo, blo);
        t_it(&c->fb, CC_EQ, "t");
        t_cmp(&c->fb, ahi, bhi);
        cond = op == WASM_OP_I64_EQ ? CC_EQ : CC_NE;
        break;

      case WASM_OP_I64_LT_S:
      case WASM_OP_I64_GE_S:
      case WASM_OP_I64_LT_U:
      case WASM_OP_I64_GE_U:
        t_cmp(&c->fb, alo, blo);
        t_dp_reg(&c->fb, DP_SBC, 1, R_IP, ahi, bhi, SH_LSL, 0);
        cond = op == WASM_OP_I64_LT_S ? CC_LT : op == WASM_OP_I64_GE_S ?
               CC_GE : op == WASM_OP_I64_LT_U ? CC_LO : CC_HS;
        break;

      default:                  /* gt, le: swapped */
        t_cmp(&c->fb, blo, alo);
        t_dp_reg(&c->fb, DP_SBC, 1, R_IP, bhi, ahi, SH_LSL, 0);
        cond = op == WASM_OP_I64_GT_S ? CC_LT : op == WASM_OP_I64_LE_S ?
               CC_GE : op == WASM_OP_I64_GT_U ? CC_LO : CC_HS;
        break;
    }

  free_val(c, &a);
  free_val(c, &b);
  return cond;
}

static int cmp_f32(jc_t *c, uint8_t op)
{
  static const uint8_t cc[] =
  {
    CC_EQ, CC_NE, CC_MI, CC_GT, CC_LS, CC_GE
  };

  val_t b = pop(c);
  val_t a = pop(c);
  int sa = vsreg(c, &a);
  int sb = vsreg(c, &b);

  t_vcmp(&c->fb, sa, sb);
  free_val(c, &a);
  free_val(c, &b);
  return cc[op - WASM_OP_F32_EQ];
}

static uint32_t fold_i32(uint8_t op, uint32_t a, uint32_t b, bool *ok)
{
  *ok = true;
  switch (op)
    {
      case WASM_OP_I32_ADD: return a + b;
      case WASM_OP_I32_SUB: return a - b;
      case WASM_OP_I32_MUL: return a * b;
      case WASM_OP_I32_AND: return a & b;
      case WASM_OP_I32_OR:  return a | b;
      case WASM_OP_I32_XOR: return a ^ b;
      case WASM_OP_I32_SHL: return a << (b & 31);
      case WASM_OP_I32_SHR_S: return (uint32_t)((int32_t)a >> (b & 31));
      case WASM_OP_I32_SHR_U: return a >> (b & 31);
      case WASM_OP_I32_ROTL:
        b &= 31;
        return b ? (a << b) | (a >> (32 - b)) : a;
      case WASM_OP_I32_ROTR:
        b &= 31;
        return b ? (a >> b) | (a << (32 - b)) : a;
      default:
        *ok = false;
        return 0;
    }
}

static void op_i32_bin(jc_t *c, uint8_t op)
{
  val_t b = pop(c);
  val_t a = pop(c);
  int ra;
  int rb;
  int rd;
  int enc;

  if (a.kind == V_CONST && b.kind == V_CONST)
    {
      bool ok;
      uint32_t r = fold_i32(op, (uint32_t)a.c, (uint32_t)b.c, &ok);

      if (ok)
        {
          push_const(c, T_I32, r);
          return;
        }
    }

  /* Constant first operand of a commutative operation: swap */

  if (a.kind == V_CONST && b.kind != V_CONST &&
      (op == WASM_OP_I32_ADD || op == WASM_OP_I32_MUL ||
       op == WASM_OP_I32_AND || op == WASM_OP_I32_OR ||
       op == WASM_OP_I32_XOR))
    {
      val_t t = a;

      a = b;
      b = t;
    }

  /* A local's register plus a constant: lazy (computed where it is used,
   * at no cost where it is not; not spilled around a call)
   */

  if (b.kind == V_CONST && (op == WASM_OP_I32_ADD || op == WASM_OP_I32_SUB) &&
      ((a.kind == V_LOCAL && c->lkind[a.local] == L_CORE &&
        c->ltype[a.local] == T_I32) || a.kind == V_LOCK))
    {
      uint32_t k = (op == WASM_OP_I32_ADD ? (uint32_t)b.c : -(uint32_t)b.c) +
                   (a.kind == V_LOCK ? (uint32_t)a.c : 0);

      if ((int32_t)k > -4096 && (int32_t)k < 4096)
        {
          val_t *v = vpush(c, k != 0 ? V_LOCK : V_LOCAL, T_I32);

          v->local = a.local;
          v->c = k;
          return;
        }
    }

  if (b.kind == V_CONST)
    {
      uint32_t k = (uint32_t)b.c;

      ra = vreg(c, &a);
      free_val(c, &a);
      switch (op)
        {
          case WASM_OP_I32_ADD:
          case WASM_OP_I32_SUB:
            rd = alloc_dest(c);
            t_addi(&c->fb, rd, ra,
                   op == WASM_OP_I32_ADD ? (int32_t)k : -(int32_t)k);
            push_reg(c, T_I32, rd, -1);
            return;

          case WASM_OP_I32_AND:
          case WASM_OP_I32_OR:
          case WASM_OP_I32_XOR:
            {
              int dp = op == WASM_OP_I32_AND ? DP_AND :
                       op == WASM_OP_I32_OR ? DP_ORR : DP_EOR;

              enc = t_modimm(k);
              if (enc < 0 && op != WASM_OP_I32_XOR)
                {
                  enc = t_modimm(~k);
                  dp = op == WASM_OP_I32_AND ? DP_BIC : DP_ORN;
                }

              if (enc >= 0)
                {
                  rd = alloc_dest(c);
                  t_dp_imm(&c->fb, dp, 0, rd, ra, enc);
                  push_reg(c, T_I32, rd, -1);
                  return;
                }
            }
            break;

          case WASM_OP_I32_SHL:
          case WASM_OP_I32_SHR_S:
          case WASM_OP_I32_SHR_U:
          case WASM_OP_I32_ROTL:
          case WASM_OP_I32_ROTR:
            {
              int sh = op == WASM_OP_I32_SHL ? SH_LSL :
                       op == WASM_OP_I32_SHR_S ? SH_ASR :
                       op == WASM_OP_I32_SHR_U ? SH_LSR : SH_ROR;

              k &= 31;
              if (op == WASM_OP_I32_ROTL)
                {
                  k = (32 - k) & 31;
                }

              rd = alloc_dest(c);
              if (k == 0)
                {
                  t_mov(&c->fb, rd, ra);
                }
              else
                {
                  t_shi(&c->fb, sh, rd, ra, (int)k);
                }

              push_reg(c, T_I32, rd, -1);
              return;
            }

          case WASM_OP_I32_MUL:
            if (k != 0 && (k & (k - 1)) == 0)
              {
                rd = alloc_dest(c);
                if (k == 1)
                  {
                    t_mov(&c->fb, rd, ra);
                  }
                else
                  {
                    t_shi(&c->fb, SH_LSL, rd, ra, __builtin_ctz(k));
                  }

                push_reg(c, T_I32, rd, -1);
                return;
              }
            break;

          default:
            break;
        }

      /* The constant in a register: its own, or ip */

      rd = alloc_dest(c);
      rb = const_reg(c, k, 1u << ra);
    }
  else if (a.kind == V_CONST && op == WASM_OP_I32_SUB &&
           t_modimm((uint32_t)a.c) >= 0)
    {
      rb = vreg(c, &b);
      free_val(c, &b);
      rd = alloc_dest(c);
      t_dp_imm(&c->fb, DP_RSB, 0, rd, rb, t_modimm((uint32_t)a.c));
      push_reg(c, T_I32, rd, -1);
      return;
    }
  else
    {
      ra = vreg(c, &a);
      rb = vreg(c, &b);
      free_val(c, &a);
      free_val(c, &b);
      rd = alloc_dest(c);
    }

  switch (op)
    {
      case WASM_OP_I32_ADD:
        t_dp_reg(&c->fb, DP_ADD, 0, rd, ra, rb, SH_LSL, 0);
        break;

      case WASM_OP_I32_SUB:
        t_dp_reg(&c->fb, DP_SUB, 0, rd, ra, rb, SH_LSL, 0);
        break;

      case WASM_OP_I32_MUL:
        t_mul(&c->fb, rd, ra, rb);
        break;

      case WASM_OP_I32_AND:
        t_dp_reg(&c->fb, DP_AND, 0, rd, ra, rb, SH_LSL, 0);
        break;

      case WASM_OP_I32_OR:
        t_dp_reg(&c->fb, DP_ORR, 0, rd, ra, rb, SH_LSL, 0);
        break;

      case WASM_OP_I32_XOR:
        t_dp_reg(&c->fb, DP_EOR, 0, rd, ra, rb, SH_LSL, 0);
        break;

      case WASM_OP_I32_SHL:
      case WASM_OP_I32_SHR_S:
      case WASM_OP_I32_SHR_U:
        t_dp_imm(&c->fb, DP_AND, 0, R_IP, rb, 31);
        t_shr(&c->fb, op == WASM_OP_I32_SHL ? SH_LSL :
              op == WASM_OP_I32_SHR_S ? SH_ASR : SH_LSR, rd, ra, R_IP);
        break;

      case WASM_OP_I32_ROTL:
        t_dp_imm(&c->fb, DP_RSB, 0, R_IP, rb, 0);
        t_shr(&c->fb, SH_ROR, rd, ra, R_IP);
        break;

      case WASM_OP_I32_ROTR:
        t_shr(&c->fb, SH_ROR, rd, ra, rb);
        break;
    }

  push_reg(c, T_I32, rd, -1);
}

static void op_i32_div(jc_t *c, uint8_t op)
{
  val_t b = pop(c);
  val_t a = pop(c);
  bool sgn = op == WASM_OP_I32_DIV_S || op == WASM_OP_I32_REM_S;
  bool rem = op == WASM_OP_I32_REM_S || op == WASM_OP_I32_REM_U;
  int ra;
  int rb;
  int rd;

  if (b.kind == V_CONST && (uint32_t)b.c != 0 && a.kind == V_CONST &&
      !(sgn && (uint32_t)b.c == 0xffffffffu &&
        (uint32_t)a.c == 0x80000000u))
    {
      uint32_t x = (uint32_t)a.c;
      uint32_t y = (uint32_t)b.c;
      uint32_t r;

      if (sgn)
        {
          if (y == 0xffffffffu)
            {
              r = rem ? 0 : -x;
            }
          else
            {
              r = rem ? (uint32_t)((int32_t)x % (int32_t)y) :
                        (uint32_t)((int32_t)x / (int32_t)y);
            }
        }
      else
        {
          r = rem ? x % y : x / y;
        }

      push_const(c, T_I32, r);
      return;
    }

  /* A constant divisor: no checks it makes needless; powers of two */

  if (b.kind == V_CONST && (uint32_t)b.c != 0)
    {
      uint32_t k = (uint32_t)b.c;
      bool pow2 = (k & (k - 1)) == 0;
      int sh = __builtin_ctz(k);

      if (pow2 && (!sgn || (k > 1 && k < 0x80000000u)))
        {
          ra = vreg(c, &a);
          free_val(c, &a);
          rd = alloc_dest(c);
          if (k == 1)
            {
              if (rem)
                {
                  t_movi(&c->fb, rd, 0);
                }
              else
                {
                  t_mov(&c->fb, rd, ra);
                }
            }
          else if (!sgn)
            {
              if (rem && t_modimm(k - 1) >= 0)
                {
                  t_dp_imm(&c->fb, DP_AND, 0, rd, ra, t_modimm(k - 1));
                }
              else if (rem)
                {
                  /* (k - 1 not an immediate: two shifts) */

                  t_shi(&c->fb, SH_LSL, rd, ra, 32 - sh);
                  t_shi(&c->fb, SH_LSR, rd, rd, 32 - sh);
                }
              else
                {
                  t_shi(&c->fb, SH_LSR, rd, ra, sh);
                }
            }
          else
            {
              /* ip = a + (a < 0 ? k - 1 : 0) */

              t_shi(&c->fb, SH_ASR, R_IP, ra, 31);
              t_dp_reg(&c->fb, DP_ADD, 0, R_IP, ra, R_IP, SH_LSR, 32 - sh);
              if (rem)
                {
                  t_shi(&c->fb, SH_LSR, R_IP, R_IP, sh);
                  t_dp_reg(&c->fb, DP_SUB, 0, rd, ra, R_IP, SH_LSL, sh);
                }
              else
                {
                  t_shi(&c->fb, SH_ASR, rd, R_IP, sh);
                }
            }

          push_reg(c, T_I32, rd, -1);
          return;
        }

      ra = vreg(c, &a);
      rb = vreg(c, &b);
      rd = alloc_dest(c);
      if (sgn && !rem && k == 0xffffffffu)
        {
          cmp_imm(c, ra, 0x80000000u);
          trap_if(c, CC_EQ, JT_OVERFLOW);
        }
    }
  else
    {
      ra = vreg(c, &a);
      rb = vreg(c, &b);

      /* b == 0; INT_MIN / -1 */

      t_cmpi_enc(&c->fb, rb, 0);
      trap_if(c, CC_EQ, JT_DIV0);
      if (sgn && !rem)
        {
          int ok = new_label(c);

          t_cmni_enc(&c->fb, rb, 1);
          bcc_to(c, CC_NE, ok);
          cmp_imm(c, ra, 0x80000000u);
          trap_if(c, CC_EQ, JT_OVERFLOW);
          lbind(c, ok);
        }

      rd = alloc_dest(c);
    }

  if (rem)
    {
      if (sgn)
        {
          t_sdiv(&c->fb, R_IP, ra, rb);
        }
      else
        {
          t_udiv(&c->fb, R_IP, ra, rb);
        }

      t_mls(&c->fb, rd, R_IP, rb, ra);
    }
  else if (sgn)
    {
      t_sdiv(&c->fb, rd, ra, rb);
    }
  else
    {
      t_udiv(&c->fb, rd, ra, rb);
    }

  free_val(c, &a);
  free_val(c, &b);
  push_reg(c, T_I32, rd, -1);
}

static void op_i32_unary(jc_t *c, uint8_t op)
{
  val_t a = pop(c);
  int ra;
  int rd;

  if (op == WASM_OP_I32_POPCNT)
    {
      ccall(c, (void *)jit_i32_popcnt, "w", &a, T_I32, true);
      return;
    }

  if (a.kind == V_CONST)
    {
      uint32_t x = (uint32_t)a.c;
      uint32_t r;

      switch (op)
        {
          case WASM_OP_I32_CLZ:
            r = x ? __builtin_clz(x) : 32;
            break;

          case WASM_OP_I32_CTZ:
            r = x ? __builtin_ctz(x) : 32;
            break;

          case WASM_OP_I32_EXTEND8_S:
            r = (uint32_t)(int32_t)(int8_t)x;
            break;

          default:
            r = (uint32_t)(int32_t)(int16_t)x;
            break;
        }

      push_const(c, T_I32, r);
      return;
    }

  ra = vreg(c, &a);
  free_val(c, &a);
  rd = alloc_dest(c);
  switch (op)
    {
      case WASM_OP_I32_CLZ:
        t_clz(&c->fb, rd, ra);
        break;

      case WASM_OP_I32_CTZ:
        t_rbit(&c->fb, R_IP, ra);
        t_clz(&c->fb, rd, R_IP);
        break;

      case WASM_OP_I32_EXTEND8_S:
        t_sxtb(&c->fb, rd, ra);
        break;

      default:
        t_sxth(&c->fb, rd, ra);
        break;
    }

  push_reg(c, T_I32, rd, -1);
}

static void op_i64_bin(jc_t *c, uint8_t op)
{
  val_t b = pop(c);
  val_t a = pop(c);
  int alo;
  int ahi;
  int blo;
  int bhi;

  if (a.kind == V_CONST && b.kind == V_CONST)
    {
      uint64_t x = a.c;
      uint64_t y = b.c;
      uint64_t r;
      bool ok = true;

      switch (op)
        {
          case WASM_OP_I64_ADD: r = x + y; break;
          case WASM_OP_I64_SUB: r = x - y; break;
          case WASM_OP_I64_MUL: r = x * y; break;
          case WASM_OP_I64_AND: r = x & y; break;
          case WASM_OP_I64_OR:  r = x | y; break;
          case WASM_OP_I64_XOR: r = x ^ y; break;
          case WASM_OP_I64_SHL: r = x << (y & 63); break;
          case WASM_OP_I64_SHR_S: r = (uint64_t)((int64_t)x >> (y & 63));
            break;
          case WASM_OP_I64_SHR_U: r = x >> (y & 63); break;
          default: ok = false; r = 0; break;
        }

      if (ok)
        {
          push_const(c, T_I64, r);
          return;
        }
    }

  /* Shifts and rotations by a variable: helpers; by a constant: inline */

  if (op >= WASM_OP_I64_SHL && op <= WASM_OP_I64_ROTR)
    {
      static void *const fn[] =
      {
        (void *)jit_i64_shl, (void *)jit_i64_shr_s, (void *)jit_i64_shr_u,
        (void *)jit_i64_rotl, (void *)jit_i64_rotr
      };

      if (b.kind == V_CONST && op <= WASM_OP_I64_SHR_U)
        {
          uint32_t k = (uint32_t)b.c & 63;

          vreg64(c, &a, &alo, &ahi);
          if (k == 0)
            {
            }
          else if (op == WASM_OP_I64_SHL)
            {
              if (k < 32)
                {
                  t_shi(&c->fb, SH_LSL, ahi, ahi, k);
                  t_dp_reg(&c->fb, DP_ORR, 0, ahi, ahi, alo, SH_LSR, 32 - k);
                  t_shi(&c->fb, SH_LSL, alo, alo, k);
                }
              else
                {
                  if (k == 32)
                    {
                      t_mov(&c->fb, ahi, alo);
                    }
                  else
                    {
                      t_shi(&c->fb, SH_LSL, ahi, alo, k - 32);
                    }

                  t_movi(&c->fb, alo, 0);
                }
            }
          else
            {
              bool s = op == WASM_OP_I64_SHR_S;

              if (k < 32)
                {
                  t_shi(&c->fb, SH_LSR, alo, alo, k);
                  t_dp_reg(&c->fb, DP_ORR, 0, alo, alo, ahi, SH_LSL, 32 - k);
                  t_shi(&c->fb, s ? SH_ASR : SH_LSR, ahi, ahi, k);
                }
              else
                {
                  if (k == 32)
                    {
                      t_mov(&c->fb, alo, ahi);
                    }
                  else
                    {
                      t_shi(&c->fb, s ? SH_ASR : SH_LSR, alo, ahi, k - 32);
                    }

                  if (s)
                    {
                      t_shi(&c->fb, SH_ASR, ahi, ahi, 31);
                    }
                  else
                    {
                      t_movi(&c->fb, ahi, 0);
                    }
                }
            }

          push_reg(c, T_I64, alo, ahi);
          return;
        }

      {
        val_t args[2];

        /* The count's low word */

        if (b.kind == V_CONST)
          {
            b.type = T_I32;
            b.c &= 63;
          }
        else
          {
            int r = alloc_core(c);

            word_to(c, r, &b, 0);
            free_val(c, &b);
            b.kind = V_REG;
            b.type = T_I32;
            b.r = (int8_t)r;
          }

        args[0] = a;
        args[1] = b;
        ccall(c, fn[op - WASM_OP_I64_SHL], "lw", args, T_I64, true);
        return;
      }
    }

  /* a: a local's registers read as they are (the result into a new
   * pair), or its own pair (the result there); b: read only, a local's
   * registers, a constant's halves (immediates where the instruction has
   * them, else registers that keep them known), or a pair of its own
   */

  {
    bool ro = a.kind == V_LOCAL && c->lkind[a.local] == L_CORE;
    int dp = op == WASM_OP_I64_ADD ? DP_ADD : op == WASM_OP_I64_SUB ? DP_SUB :
             op == WASM_OP_I64_AND ? DP_AND : op == WASM_OP_I64_OR ? DP_ORR :
             op == WASM_OP_I64_XOR ? DP_EOR : -1;
    int dp2 = op == WASM_OP_I64_ADD ? DP_ADC : op == WASM_OP_I64_SUB ? DP_SBC :
              dp;
    int elo = -1;
    int ehi = -1;
    int dlo;
    int dhi;
    uint32_t avoid;

    if (ro)
      {
        alo = c->lreg[a.local];
        ahi = c->lreg2[a.local];
      }
    else
      {
        vreg64(c, &a, &alo, &ahi);
      }

    if (b.kind == V_LOCAL && c->lkind[b.local] == L_CORE)
      {
        blo = c->lreg[b.local];
        bhi = c->lreg2[b.local];
      }
    else if (b.kind != V_CONST)
      {
        vreg64(c, &b, &blo, &bhi);
      }

    if (ro)
      {
        dlo = alloc_core(c);
        dhi = alloc_core(c);
      }
    else
      {
        dlo = alo;
        dhi = ahi;
      }

    if (b.kind == V_CONST)
      {
        uint32_t klo = (uint32_t)b.c;
        uint32_t khi = (uint32_t)(b.c >> 32);

        avoid = 1u << alo | 1u << ahi | 1u << dlo | 1u << dhi;
        if (dp >= 0)
          {
            elo = t_modimm(klo);
            ehi = t_modimm(khi);
          }

        if ((dp == DP_ADD || dp == DP_SUB) && (elo < 0 || ehi < 0))
          {
            elo = -1;               /* (the carry: both, or neither) */
            ehi = -1;
          }

        blo = elo >= 0 ? -1 : const_reg(c, klo, avoid);
        if (blo >= 0)
          {
            avoid |= 1u << blo;
          }

        bhi = ehi >= 0 ? -1 : const_reg(c, khi, avoid);
        if (op == WASM_OP_I64_MUL && (blo == R_IP || bhi == R_IP))
          {
            /* (ip is the multiplication's own: the constant elsewhere) */

            int t = alloc_core(c);

            t_mov(&c->fb, t, R_IP);
            if (blo == R_IP)
              {
                blo = t;
              }
            else
              {
                bhi = t;
              }

            b.kind = V_REG;             /* (freed below) */
            b.type = T_I32;
            b.r = (int8_t)t;
          }
        else if (blo == R_IP && bhi == R_IP)
          {
            fail(c, "out of registers", 0);
            c->regs_out = true;
          }
      }

    switch (op)
      {
        case WASM_OP_I64_ADD:
        case WASM_OP_I64_SUB:
        case WASM_OP_I64_AND:
        case WASM_OP_I64_OR:
        case WASM_OP_I64_XOR:
          {
            int s = dp == DP_ADD || dp == DP_SUB;

            if (elo >= 0)
              {
                t_dp_imm(&c->fb, dp, s, dlo, alo, elo);
              }
            else
              {
                t_dp_reg(&c->fb, dp, s, dlo, alo, blo, SH_LSL, 0);
              }

            if (ehi >= 0)
              {
                t_dp_imm(&c->fb, dp2, 0, dhi, ahi, ehi);
              }
            else
              {
                t_dp_reg(&c->fb, dp2, 0, dhi, ahi, bhi, SH_LSL, 0);
              }
          }
          break;

        case WASM_OP_I64_MUL:
          t_mul(&c->fb, R_IP, alo, bhi);
          t_mla(&c->fb, R_IP, ahi, blo, R_IP);
          t_umull(&c->fb, dlo, dhi, alo, blo);
          t_dp_reg(&c->fb, DP_ADD, 0, dhi, dhi, R_IP, SH_LSL, 0);
          break;
      }

    if (b.kind == V_REG)
      {
        free_val(c, &b);
      }

    push_reg(c, T_I64, dlo, dhi);
  }
}

static void op_i64_div(jc_t *c, uint8_t op)
{
  static void *const fn[] =
  {
    (void *)jit_i64_div_s, (void *)jit_i64_div_u,
    (void *)jit_i64_rem_s, (void *)jit_i64_rem_u
  };

  val_t args[2];
  int alo;
  int ahi;
  int blo;
  int bhi;

  args[1] = pop(c);
  args[0] = pop(c);
  vreg64(c, &args[0], &alo, &ahi);
  vreg64(c, &args[1], &blo, &bhi);

  t_dp_reg(&c->fb, DP_ORR, 1, R_IP, blo, bhi, SH_LSL, 0);    /* orrs */
  trap_if(c, CC_EQ, JT_DIV0);

  if (op == WASM_OP_I64_DIV_S)
    {
      int ok = new_label(c);

      t_dp_reg(&c->fb, DP_AND, 0, R_IP, blo, bhi, SH_LSL, 0);
      t_cmni_enc(&c->fb, R_IP, 1);
      bcc_to(c, CC_NE, ok);
      cmp_imm(c, ahi, 0x80000000u);
      t_it(&c->fb, CC_EQ, "t");
      t_cmpi_enc(&c->fb, alo, 0);
      trap_if(c, CC_EQ, JT_OVERFLOW);
      lbind(c, ok);
    }

  ccall(c, fn[op - WASM_OP_I64_DIV_S], "ll", args, T_I64, true);
}

static void op_i64_unary(jc_t *c, uint8_t op)
{
  val_t a = pop(c);
  int alo;
  int ahi;

  switch (op)
    {
      case WASM_OP_I64_CTZ:
        ccall(c, (void *)jit_i64_ctz, "l", &a, T_I64, true);
        return;

      case WASM_OP_I64_POPCNT:
        ccall(c, (void *)jit_i64_popcnt, "l", &a, T_I64, true);
        return;

      case WASM_OP_I64_CLZ:
        vreg64(c, &a, &alo, &ahi);
        t_clz(&c->fb, R_IP, alo);
        t_addi(&c->fb, R_IP, R_IP, 32);
        t_cmpi_enc(&c->fb, ahi, 0);
        t_it(&c->fb, CC_NE, "t");
        t_clz(&c->fb, R_IP, ahi);
        t_mov(&c->fb, alo, R_IP);
        t_movi(&c->fb, ahi, 0);
        push_reg(c, T_I64, alo, ahi);
        return;

      default:                  /* extend8_s, extend16_s, extend32_s */
        vreg64(c, &a, &alo, &ahi);
        if (op == WASM_OP_I64_EXTEND8_S)
          {
            t_sxtb(&c->fb, alo, alo);
          }
        else if (op == WASM_OP_I64_EXTEND16_S)
          {
            t_sxth(&c->fb, alo, alo);
          }

        t_shi(&c->fb, SH_ASR, ahi, alo, 31);
        push_reg(c, T_I64, alo, ahi);
        return;
    }
}

/****************************************************************************
 * Floating point
 ****************************************************************************/

static void op_f32_bin(jc_t *c, uint8_t op)
{
  val_t b = pop(c);
  val_t a = pop(c);
  int sa = vsreg(c, &a);
  int sb = vsreg(c, &b);
  int sd;

  if (op == WASM_OP_F32_MIN || op == WASM_OP_F32_MAX)
    {
      sd = alloc_s(c);
      t_vcmp(&c->fb, sa, sb);
      t_vop3(&c->fb, op == WASM_OP_F32_MIN ? VOP_MINNM : VOP_MAXNM,
             sd, sa, sb);
      t_it(&c->fb, CC_VS, "t");
      t_vop3(&c->fb, VOP_ADD, sd, sa, sb);           /* NaN: a NaN */
      free_val(c, &a);
      free_val(c, &b);
      push_reg(c, T_F32, sd, -1);
      return;
    }

  if (op == WASM_OP_F32_COPYSIGN)
    {
      int r = alloc_core(c);

      t_vmov_rs(&c->fb, R_IP, sb);
      t_vmov_rs(&c->fb, r, sa);
      free_val(c, &a);
      free_val(c, &b);
      t_dp_imm(&c->fb, DP_AND, 0, R_IP, R_IP, t_modimm(0x80000000u));
      t_dp_imm(&c->fb, DP_BIC, 0, r, r, t_modimm(0x80000000u));
      t_dp_reg(&c->fb, DP_ORR, 0, r, r, R_IP, SH_LSL, 0);
      sd = alloc_s(c);
      t_vmov_sr(&c->fb, sd, r);
      free_core(c, r);
      push_reg(c, T_F32, sd, -1);
      return;
    }

  free_val(c, &a);
  free_val(c, &b);
  sd = alloc_sdest(c);
  t_vop3(&c->fb, op == WASM_OP_F32_ADD ? VOP_ADD : op == WASM_OP_F32_SUB ?
         VOP_SUB : op == WASM_OP_F32_MUL ? VOP_MUL : VOP_DIV, sd, sa, sb);
  push_reg(c, T_F32, sd, -1);
}

static void op_f32_unary(jc_t *c, uint8_t op)
{
  static const uint8_t vop[] =
  {
    VOP_ABS, VOP_NEG, VOP_RINTP, VOP_RINTM, VOP_RINTZ, VOP_RINTN, VOP_SQRT
  };

  val_t a = pop(c);
  int sa = vsreg(c, &a);
  int sd;

  free_val(c, &a);
  sd = alloc_sdest(c);
  t_vop2(&c->fb, vop[op - WASM_OP_F32_ABS], sd, sa);
  push_reg(c, T_F32, sd, -1);
}

/* f64: helpers on the bit patterns (abs, neg, copysign inline) */

static void op_f64(jc_t *c, uint8_t op)
{
  static void *const bin[] =
  {
    (void *)jit_f64_add, (void *)jit_f64_sub, (void *)jit_f64_mul,
    (void *)jit_f64_div, (void *)jit_f64_min, (void *)jit_f64_max
  };

  static void *const un[] =
  {
    NULL, NULL, (void *)jit_f64_ceil, (void *)jit_f64_floor,
    (void *)jit_f64_trunc, (void *)jit_f64_nearest, (void *)jit_f64_sqrt
  };

  static void *const cmp[] =
  {
    (void *)jit_f64_eq, (void *)jit_f64_ne, (void *)jit_f64_lt,
    (void *)jit_f64_gt, (void *)jit_f64_le, (void *)jit_f64_ge
  };

  val_t args[2];
  int lo;
  int hi;

  if (op >= WASM_OP_F64_EQ && op <= WASM_OP_F64_GE)
    {
      args[1] = pop(c);
      args[0] = pop(c);
      ccall(c, cmp[op - WASM_OP_F64_EQ], "ll", args, T_I32, true);
      return;
    }

  if (op >= WASM_OP_F64_ADD && op <= WASM_OP_F64_MAX)
    {
      args[1] = pop(c);
      args[0] = pop(c);
      ccall(c, bin[op - WASM_OP_F64_ADD], "ll", args, T_F64, true);
      return;
    }

  if (op == WASM_OP_F64_COPYSIGN)
    {
      args[1] = pop(c);
      args[0] = pop(c);
      vreg64(c, &args[0], &lo, &hi);
      word_to(c, R_IP, &args[1], 1);
      free_val(c, &args[1]);
      t_dp_imm(&c->fb, DP_AND, 0, R_IP, R_IP, t_modimm(0x80000000u));
      t_dp_imm(&c->fb, DP_BIC, 0, hi, hi, t_modimm(0x80000000u));
      t_dp_reg(&c->fb, DP_ORR, 0, hi, hi, R_IP, SH_LSL, 0);
      push_reg(c, T_F64, lo, hi);
      return;
    }

  args[0] = pop(c);
  if (op == WASM_OP_F64_ABS || op == WASM_OP_F64_NEG)
    {
      vreg64(c, &args[0], &lo, &hi);
      t_dp_imm(&c->fb, op == WASM_OP_F64_ABS ? DP_BIC : DP_EOR, 0, hi, hi,
               t_modimm(0x80000000u));
      push_reg(c, T_F64, lo, hi);
      return;
    }

  ccall(c, un[op - WASM_OP_F64_ABS], "l", args, T_F64, true);
}

/* Check an f32 before truncating it to an integer (lo < x < hi, given as
 * the bits of exclusive bounds); NaN is an invalid conversion
 */

static void f32_range(jc_t *c, int sa, uint32_t lo_bits, uint32_t hi_bits)
{
  int st = alloc_s(c);

  t_vcmp(&c->fb, sa, sa);
  trap_if(c, CC_VS, JT_CONV);
  t_movi(&c->fb, R_IP, hi_bits);
  t_vmov_sr(&c->fb, st, R_IP);
  t_vcmp(&c->fb, sa, st);
  trap_if(c, CC_GE, JT_OVERFLOW);
  t_movi(&c->fb, R_IP, lo_bits);
  t_vmov_sr(&c->fb, st, R_IP);
  t_vcmp(&c->fb, sa, st);
  trap_if(c, CC_LS, JT_OVERFLOW);
  free_s(c, st);
}

static void op_convert(jc_t *c, uint8_t op)
{
  val_t a = pop(c);
  int ra;
  int sa;
  int rd;
  int sd;
  int lo;
  int hi;

  switch (op)
    {
      case WASM_OP_I32_WRAP_I64:
        if (a.kind == V_CONST)
          {
            push_const(c, T_I32, a.c & 0xffffffffu);
          }
        else if (a.kind == V_REG)
          {
            free_core(c, a.r2);
            push_reg(c, T_I32, a.r, -1);
          }
        else
          {
            rd = alloc_core(c);
            word_to(c, rd, &a, 0);
            push_reg(c, T_I32, rd, -1);
          }
        return;

      case WASM_OP_I64_EXTEND_S_I32:
      case WASM_OP_I64_EXTEND_U_I32:
        if (a.kind == V_CONST)
          {
            push_const(c, T_I64, op == WASM_OP_I64_EXTEND_S_I32 ?
                       (uint64_t)(int64_t)(int32_t)a.c :
                       (uint64_t)(uint32_t)a.c);
            return;
          }

        lo = vreg_own(c, &a);
        hi = alloc_core(c);
        if (op == WASM_OP_I64_EXTEND_S_I32)
          {
            t_shi(&c->fb, SH_ASR, hi, lo, 31);
          }
        else
          {
            t_movi(&c->fb, hi, 0);
          }

        push_reg(c, T_I64, lo, hi);
        return;

      case WASM_OP_I32_REINTERPRET_F32:
        if (a.kind == V_CONST)
          {
            push_const(c, T_I32, a.c);
            return;
          }

        if (a.kind == V_SLOT || (a.kind == V_LOCAL &&
                                 c->lkind[a.local] == L_FRAME))
          {
            a.type = T_I32;
            rd = vreg(c, &a);
            push_reg(c, T_I32, rd, -1);
            return;
          }

        sa = vsreg(c, &a);
        free_val(c, &a);
        rd = alloc_core(c);
        t_vmov_rs(&c->fb, rd, sa);
        push_reg(c, T_I32, rd, -1);
        return;

      case WASM_OP_F32_REINTERPRET_I32:
        if (a.kind == V_CONST)
          {
            push_const(c, T_F32, a.c);
            return;
          }

        if (a.kind == V_SLOT)
          {
            a.type = T_F32;
            sd = vsreg(c, &a);
            push_reg(c, T_F32, sd, -1);
            return;
          }

        ra = vreg(c, &a);
        free_val(c, &a);
        sd = alloc_s(c);
        t_vmov_sr(&c->fb, sd, ra);
        push_reg(c, T_F32, sd, -1);
        return;

      case WASM_OP_I64_REINTERPRET_F64:
      case WASM_OP_F64_REINTERPRET_I64:
        {
          val_t *v;

          if (a.kind == V_LOCAL)
            {
              vreg64(c, &a, &lo, &hi);
            }

          v = vpush(c, a.kind, op == WASM_OP_I64_REINTERPRET_F64 ?
                    T_I64 : T_F64);
          v->r  = a.r;
          v->r2 = a.r2;
          v->c  = a.c;
          if (a.kind == V_SLOT && a.depth != v->depth)
            {
              /* (a slot is the stack index's: the same here) */
            }

          return;
        }

      case WASM_OP_F32_CONVERT_S_I32:
      case WASM_OP_F32_CONVERT_U_I32:
        ra = vreg(c, &a);
        free_val(c, &a);
        sd = alloc_sdest(c);
        t_vmov_sr(&c->fb, sd, ra);
        t_vop2(&c->fb, op == WASM_OP_F32_CONVERT_S_I32 ? VOP_CVT_F32_S32 :
               VOP_CVT_F32_U32, sd, sd);
        push_reg(c, T_F32, sd, -1);
        return;

      case WASM_OP_I32_TRUNC_S_F32:
      case WASM_OP_I32_TRUNC_U_F32:
        sa = vsreg(c, &a);
        if (op == WASM_OP_I32_TRUNC_S_F32)
          {
            f32_range(c, sa, 0xcf000001u, 0x4f000000u);   /* -2^31-, 2^31 */
          }
        else
          {
            f32_range(c, sa, 0xbf800000u, 0x4f800000u);   /* -1, 2^32 */
          }

        free_val(c, &a);
        sd = alloc_s(c);
        t_vop2(&c->fb, op == WASM_OP_I32_TRUNC_S_F32 ? VOP_CVT_S32_F32 :
               VOP_CVT_U32_F32, sd, sa);
        rd = alloc_core(c);
        t_vmov_rs(&c->fb, rd, sd);
        free_s(c, sd);
        push_reg(c, T_I32, rd, -1);
        return;

      case WASM_OP_I32_TRUNC_S_F64:
        ccall(c, (void *)jit_i32_trunc_f64_s, "Il", &a, T_I32, true);
        return;

      case WASM_OP_I32_TRUNC_U_F64:
        ccall(c, (void *)jit_i32_trunc_f64_u, "Il", &a, T_I32, true);
        return;

      case WASM_OP_I64_TRUNC_S_F32:
        ccall(c, (void *)jit_i64_trunc_f32_s, "Iw", &a, T_I64, true);
        return;

      case WASM_OP_I64_TRUNC_U_F32:
        ccall(c, (void *)jit_i64_trunc_f32_u, "Iw", &a, T_I64, true);
        return;

      case WASM_OP_I64_TRUNC_S_F64:
        ccall(c, (void *)jit_i64_trunc_f64_s, "Il", &a, T_I64, true);
        return;

      case WASM_OP_I64_TRUNC_U_F64:
        ccall(c, (void *)jit_i64_trunc_f64_u, "Il", &a, T_I64, true);
        return;

      case WASM_OP_F32_CONVERT_S_I64:
        ccall(c, (void *)jit_f32_convert_i64_s, "l", &a, T_F32, true);
        return;

      case WASM_OP_F32_CONVERT_U_I64:
        ccall(c, (void *)jit_f32_convert_i64_u, "l", &a, T_F32, true);
        return;

      case WASM_OP_F32_DEMOTE_F64:
        ccall(c, (void *)jit_f32_demote_f64, "l", &a, T_F32, true);
        return;

      case WASM_OP_F64_CONVERT_S_I32:
        ccall(c, (void *)jit_f64_convert_i32_s, "w", &a, T_F64, true);
        return;

      case WASM_OP_F64_CONVERT_U_I32:
        ccall(c, (void *)jit_f64_convert_i32_u, "w", &a, T_F64, true);
        return;

      case WASM_OP_F64_CONVERT_S_I64:
        ccall(c, (void *)jit_f64_convert_i64_s, "l", &a, T_F64, true);
        return;

      case WASM_OP_F64_CONVERT_U_I64:
        ccall(c, (void *)jit_f64_convert_i64_u, "l", &a, T_F64, true);
        return;

      case WASM_OP_F64_PROMOTE_F32:
        ccall(c, (void *)jit_f64_promote_f32, "w", &a, T_F64, true);
        return;
    }
}

static void op_trunc_sat(jc_t *c, uint32_t sub)
{
  val_t a = pop(c);
  int sa;
  int sd;
  int rd;

  switch (sub)
    {
      case WASM_OP_I32_TRUNC_SAT_S_F32:
      case WASM_OP_I32_TRUNC_SAT_U_F32:
        sa = vsreg(c, &a);
        free_val(c, &a);
        sd = alloc_s(c);
        t_vop2(&c->fb, sub == WASM_OP_I32_TRUNC_SAT_S_F32 ?
               VOP_CVT_S32_F32 : VOP_CVT_U32_F32, sd, sa);
        rd = alloc_core(c);
        t_vmov_rs(&c->fb, rd, sd);
        free_s(c, sd);
        push_reg(c, T_I32, rd, -1);
        return;

      case WASM_OP_I32_TRUNC_SAT_S_F64:
        ccall(c, (void *)jit_i32_trunc_sat_f64_s, "l", &a, T_I32, true);
        return;

      case WASM_OP_I32_TRUNC_SAT_U_F64:
        ccall(c, (void *)jit_i32_trunc_sat_f64_u, "l", &a, T_I32, true);
        return;

      case WASM_OP_I64_TRUNC_SAT_S_F32:
        ccall(c, (void *)jit_i64_trunc_sat_f32_s, "w", &a, T_I64, true);
        return;

      case WASM_OP_I64_TRUNC_SAT_U_F32:
        ccall(c, (void *)jit_i64_trunc_sat_f32_u, "w", &a, T_I64, true);
        return;

      case WASM_OP_I64_TRUNC_SAT_S_F64:
        ccall(c, (void *)jit_i64_trunc_sat_f64_s, "l", &a, T_I64, true);
        return;

      default:
        ccall(c, (void *)jit_i64_trunc_sat_f64_u, "l", &a, T_I64, true);
        return;
    }
}

/****************************************************************************
 * Fused conditions, select
 ****************************************************************************/

static void op_select_cond(jc_t *c, int cond)
{
  val_t b = pop(c);
  val_t a = pop(c);

  /* (the flags are live: nothing below may set them) */

  if (a.type == T_F32)
    {
      int sa;
      int sb = vsreg(c, &b);

      if (a.kind != V_REG)
        {
          int s = alloc_s(c);

          sa = vsreg(c, &a);
          t_vop2(&c->fb, VOP_MOV, s, sa);
          free_val(c, &a);
          a.kind = V_REG;
          a.r = (int8_t)s;
        }

      t_it(&c->fb, CC_INV(cond), "t");
      t_vop2(&c->fb, VOP_MOV, a.r, sb);
      free_val(c, &b);
      push_reg(c, T_F32, a.r, -1);
      return;
    }

  if (IS64(a.type))
    {
      int alo;
      int ahi;
      int blo;
      int bhi;

      vreg64(c, &a, &alo, &ahi);
      vreg64(c, &b, &blo, &bhi);
      t_it(&c->fb, CC_INV(cond), "tt");
      t_mov(&c->fb, alo, blo);
      t_mov(&c->fb, ahi, bhi);
      free_val(c, &b);
      push_reg(c, a.type, alo, ahi);
      return;
    }

  {
    int ra = vreg_own(c, &a);
    int rb = vreg(c, &b);

    t_it(&c->fb, CC_INV(cond), "t");
    t_mov(&c->fb, ra, rb);
    free_val(c, &b);
    push_reg(c, T_I32, ra, -1);
  }
}

/* Whether the compare's result goes straight to br_if, if or select */

static int next_use(const uint8_t *p)
{
  if (*p == WASM_OP_BR_IF || *p == WASM_OP_IF || *p == WASM_OP_SELECT)
    {
      return *p;
    }

  return 0;
}

static void op_if(jc_t *c, const uint8_t **pp, int cond, val_t *condv);

static void use_cond(jc_t *c, int cond, const uint8_t **pp)
{
  const uint8_t *p = *pp;

  switch (next_use(p))
    {
      case WASM_OP_BR_IF:
        p++;
        br_cond(c, rd_u32(&p), cond);
        break;

      case WASM_OP_IF:
        p++;
        op_if(c, &p, cond, NULL);
        break;

      case WASM_OP_SELECT:
        p++;
        op_select_cond(c, cond);
        break;

      default:
        push_cond(c, cond);
        break;
    }

  *pp = p;
}

/* if: a fused condition (flags set: true is cond), or a value */

static void op_if(jc_t *c, const uint8_t **pp, int cond, val_t *condv)
{
  ctrl_t *ct;
  int rc = -1;

  if (condv != NULL)
    {
      rc = vreg(c, condv);
    }

  /* (the flags, if live, survive the spills: loads and stores only) */

  push_ctrl(c, CK_IF, pp);
  spill_all(c);
  ct = &c->cs[c->csp - 1];
  spill_from(c, ct->height);
  ct->label = new_label(c);
  ct->else_label = new_label(c);
  if (condv != NULL)
    {
      t_cmpi_enc(&c->fb, rc, 0);
      free_val(c, condv);
      cond = CC_NE;
    }

  res_choose(c, ct);
  bcc_to(c, CC_INV(cond), ct->else_label);
}

/* i32.and, or, xor, sub whose result is only tested (an i32.eqz, br_if,
 * if or select follows): the flags of tst, orrs, teq, cmp, no result
 * (true: done, *pp after what was fused)
 */

static bool fuse_test(jc_t *c, uint8_t op, const uint8_t **pp)
{
  const uint8_t *q = *pp;
  int cond = CC_NE;
  val_t b;
  val_t a;
  int ra;
  int enc;
  int dp;

  if (*q == WASM_OP_I32_EQZ)
    {
      cond = CC_EQ;
      q++;
    }
  else if (next_use(q) == 0)
    {
      return false;
    }

  if (c->vsp < 2 || (peek(c, 0)->kind == V_CONST &&
                     peek(c, 1)->kind == V_CONST))
    {
      return false;                     /* (folded instead) */
    }

  b = pop(c);
  a = pop(c);
  if (a.kind == V_CONST)
    {
      val_t t = a;                      /* (each is commutative here) */

      a = b;
      b = t;
    }

  ra = vreg(c, &a);
  dp = op == WASM_OP_I32_AND ? DP_AND : op == WASM_OP_I32_OR ? DP_ORR :
       op == WASM_OP_I32_XOR ? DP_EOR : DP_SUB;
  if (b.kind == V_CONST && dp == DP_SUB)
    {
      cmp_imm(c, ra, (uint32_t)b.c);
    }
  else if (b.kind == V_CONST && (enc = t_modimm((uint32_t)b.c)) >= 0)
    {
      t_dp_imm(&c->fb, dp, 1, dp == DP_ORR ? R_IP : 15, ra, enc);
    }
  else
    {
      int rb;

      if (b.kind == V_CONST)
        {
          rb = const_reg(c, (uint32_t)b.c, 1u << ra);
        }
      else
        {
          rb = vreg(c, &b);
        }

      t_dp_reg(&c->fb, dp, 1, dp == DP_ORR ? R_IP : 15, ra, rb, SH_LSL, 0);
    }

  free_val(c, &a);
  free_val(c, &b);
  use_cond(c, cond, &q);
  *pp = q;
  return true;
}

/****************************************************************************
 * Functions
 ****************************************************************************/

/* Whether an i32 constant costs nothing much as an operand */

static bool cheap_imm(uint32_t v)
{
  return v < 4096 || -v < 4096 || t_modimm(v) >= 0 || t_modimm(~v) >= 0;
}

static void count_const(jc_t *c, uint32_t v, bool f32, uint32_t w)
{
  uint32_t i;

  for (i = 0; i < c->nkc; i++)
    {
      if (c->kc[i].v == v && c->kc[i].f32 == f32)
        {
          c->kc[i].w += w;
          return;
        }
    }

  if (c->nkc < sizeof(c->kc) / sizeof(c->kc[0]))
    {
      c->kc[c->nkc].v = v;
      c->kc[c->nkc].w = w;
      c->kc[c->nkc].f32 = f32;
      c->nkc++;
    }
}

/* A constant's register, or -1 */

static int pinned(jc_t *c, uint32_t v, bool f32)
{
  uint32_t i;

  for (i = 0; i < c->npin; i++)
    {
      if (c->pin[i].v == v && c->pin[i].f32 == f32)
        {
          return c->pin[i].r;
        }
    }

  return -1;
}

/* Whether an instruction calls one of the runtime's helpers (p after
 * the opcode; prev the instruction before it)
 */

static bool calls_helper(uint8_t op, const uint8_t *p, uint8_t prev)
{
  uint32_t sub;

  switch (op)
    {
      case WASM_OP_I32_POPCNT:
      case WASM_OP_I64_CTZ:
      case WASM_OP_I64_POPCNT:
      case WASM_OP_I64_DIV_S:
      case WASM_OP_I64_DIV_U:
      case WASM_OP_I64_REM_S:
      case WASM_OP_I64_REM_U:
      case WASM_OP_I64_ROTL:
      case WASM_OP_I64_ROTR:
      case WASM_OP_MEMORY_GROW:
      case WASM_OP_TABLE_GET:
      case WASM_OP_TABLE_SET:
      case WASM_OP_I32_TRUNC_S_F64:
      case WASM_OP_I32_TRUNC_U_F64:
      case WASM_OP_F32_DEMOTE_F64:
      case WASM_OP_F64_PROMOTE_F32:
        return true;

      case WASM_OP_I64_SHL:
      case WASM_OP_I64_SHR_S:
      case WASM_OP_I64_SHR_U:
        return prev != WASM_OP_I64_CONST;   /* (by a constant: inline) */

      case WASM_OP_MISC_PREFIX:
        sub = rd_u32(&p);
        return sub != WASM_OP_I32_TRUNC_SAT_S_F32 &&
               sub != WASM_OP_I32_TRUNC_SAT_U_F32 &&
               sub != WASM_OP_TABLE_SIZE;

      default:
        break;
    }

  return (op >= WASM_OP_F64_EQ && op <= WASM_OP_F64_GE) ||
         (op >= WASM_OP_F64_CEIL && op <= WASM_OP_F64_MAX) ||
         (op >= WASM_OP_I64_TRUNC_S_F32 && op <= WASM_OP_I64_TRUNC_U_F64) ||
         (op >= WASM_OP_F32_CONVERT_S_I64 && op <= WASM_OP_F32_CONVERT_U_I64) ||
         (op >= WASM_OP_F64_CONVERT_S_I32 && op <= WASM_OP_F64_CONVERT_U_I64);
}

/* Count the uses of each local (weighted by loop depth) and of the
 * constants in loops, find the largest outgoing area, whether the
 * function is a leaf; false if the function uses what the compiler does
 * not support
 */

/* Which locals are set on every path before they are read (definite
 * assignment over the structured control flow, forward): those need no
 * starting value (c->lfirst[l] = 2).  The state is a set of the locals
 * assigned; a block's end gets the intersection of the states of the
 * branches to it and of its fall through; a loop's body starts with its
 * entry's (its back edges only add to that); unreachable code has every
 * local (neutral).
 */

struct da_s
{
  uint8_t kind;                   /* CK_* */
  bool any;                       /* Something reached its end */
  uint32_t *acc;                  /* The end's state (intersection) */
  uint32_t *cond;                 /* An if's state at its condition */
};

static void defassign(jc_t *c)
{
  const uint8_t *p = c->code;
  uint32_t nw = (c->nlocals + 31) / 32;
  uint32_t depth = 0;
  uint32_t cap = 16;
  struct da_s *st;
  uint32_t *pool;
  uint32_t *a;
  uint32_t i;
  bool unreach = false;

  /* (the state, then two sets a level: cap levels, more when nested) */

  st   = jit_zalloc(cap * sizeof(*st));
  pool = jit_zalloc((1 + 2 * cap) * nw * 4);
  if (st == NULL || pool == NULL)
    {
      jit_free(st);
      jit_free(pool);
      return;                       /* (everything zeroed, as before) */
    }

  a = pool;
  for (i = 0; i < nw; i++)
    {
      a[i] = 0;
    }

  for (i = 0; i < c->nparams; i++)
    {
      a[i / 32] |= 1u << (i % 32);
    }

#define DA_SET(x, l)  ((x)[(l) / 32] |= 1u << ((l) % 32))
#define DA_HAS(x, l)  (((x)[(l) / 32] >> ((l) % 32)) & 1)

  while (p < c->code_end)
    {
      uint8_t op = *p++;
      const uint8_t *q = p;
      struct da_s *t;
      uint32_t l;
      uint32_t k;

      switch (op)
        {
          case WASM_OP_GET_LOCAL:
            l = rd_u32(&q);
            if (l < c->nlocals && !DA_HAS(a, l))
              {
                c->lfirst[l] = 1;
              }
            break;

          case WASM_OP_SET_LOCAL:
          case WASM_OP_TEE_LOCAL:
            l = rd_u32(&q);
            if (l < c->nlocals)
              {
                DA_SET(a, l);
              }
            break;

          case WASM_OP_BLOCK:
          case WASM_OP_LOOP:
          case WASM_OP_IF:
            if (depth == cap)
              {
                struct da_s *ns = jit_realloc(st, 2 * cap * sizeof(*st));
                uint32_t *np = ns != NULL ?
                               jit_realloc(pool, (1 + 4 * cap) * nw * 4) :
                               NULL;

                if (np == NULL)
                  {
                    jit_free(ns != NULL ? ns : st);
                    jit_free(pool);
                    for (i = c->nparams; i < c->nlocals; i++)
                      {
                        c->lfirst[i] = 1;     /* (all zeroed) */
                      }

                    return;
                  }

                st   = ns;
                pool = np;
                cap *= 2;
                a = pool;
                for (k = 0; k < depth; k++)
                  {
                    st[k].acc  = pool + (1 + 2 * k) * nw;
                    st[k].cond = pool + (2 + 2 * k) * nw;
                  }
              }

            t = &st[depth++];
            t->kind = op == WASM_OP_BLOCK ? CK_BLOCK :
                      op == WASM_OP_LOOP ? CK_LOOP : CK_IF;
            t->any  = false;
            t->acc  = pool + (1 + 2 * (depth - 1)) * nw;
            t->cond = pool + (2 + 2 * (depth - 1)) * nw;
            for (i = 0; i < nw; i++)
              {
                t->acc[i]  = 0xffffffffu;
                t->cond[i] = a[i];
              }
            break;

          case WASM_OP_ELSE:
            if (depth > 0)
              {
                t = &st[depth - 1];
                for (i = 0; i < nw; i++)
                  {
                    t->acc[i] &= a[i];          /* (the then's end) */
                    a[i] = t->cond[i];
                  }

                t->any = t->any || !unreach;
                t->kind = CK_ELSE;
                unreach = false;
              }
            break;

          case WASM_OP_END:
            if (depth == 0)
              {
                break;                          /* (the function's) */
              }

            t = &st[--depth];
            if (t->kind == CK_LOOP)
              {
                break;                          /* (its body's end) */
              }

            for (i = 0; i < nw; i++)
              {
                a[i] &= t->acc[i];
                if (t->kind == CK_IF)
                  {
                    a[i] &= t->cond[i];         /* (no else: skipped) */
                  }
              }

            unreach = unreach && !t->any && t->kind != CK_IF;
            if (unreach)
              {
                for (i = 0; i < nw; i++)
                  {
                    a[i] = 0xffffffffu;
                  }
              }
            break;

          case WASM_OP_BR:
          case WASM_OP_BR_IF:
            k = rd_u32(&q);
            if (k < depth && st[depth - 1 - k].kind != CK_LOOP)
              {
                t = &st[depth - 1 - k];
                for (i = 0; i < nw; i++)
                  {
                    t->acc[i] &= a[i];
                  }

                t->any = t->any || !unreach;
              }

            if (op == WASM_OP_BR)
              {
                unreach = true;
              }
            break;

          case WASM_OP_BR_TABLE:
            {
              uint32_t n = rd_u32(&q);
              uint32_t j;

              for (j = 0; j <= n; j++)
                {
                  k = rd_u32(&q);
                  if (k < depth && st[depth - 1 - k].kind != CK_LOOP)
                    {
                      t = &st[depth - 1 - k];
                      for (i = 0; i < nw; i++)
                        {
                          t->acc[i] &= a[i];
                        }

                      t->any = t->any || !unreach;
                    }
                }

              unreach = true;
            }
            break;

          case WASM_OP_RETURN:
          case WASM_OP_UNREACHABLE:
            unreach = true;
            break;

          default:
            break;
        }

      if (unreach && op != WASM_OP_END)
        {
          for (i = 0; i < nw; i++)
            {
              a[i] = 0xffffffffu;           /* (neutral) */
            }
        }

      if (!skip_imm(op, &p))
        {
          break;
        }
    }

#undef DA_SET
#undef DA_HAS

  for (i = c->nparams; i < c->nlocals; i++)
    {
      c->lfirst[i] = c->lfirst[i] == 1 ? 1 : 2;
    }

  jit_free(st);
  jit_free(pool);
}

static bool prescan(jc_t *c)
{
  const uint8_t *p = c->code;
  uint8_t loops[64];
  uint32_t depth = 0;
  uint32_t ld = 0;
  uint8_t prev = WASM_OP_NOP;
  bool helpers = false;

  while (p < c->code_end)
    {
      uint8_t op = *p++;
      const uint8_t *q = p;
      uint32_t w = 1u << (2 * (ld < 5 ? ld : 5));

      if (calls_helper(op, p, prev))
        {
          helpers = true;
          c->callw += w;
        }

      if (op == WASM_OP_I64_CONST || op == WASM_OP_F64_CONST ||
          (op >= WASM_OP_I64_LOAD && op <= WASM_OP_F64_LOAD) ||
          (op >= WASM_OP_I64_LOAD8_S && op <= WASM_OP_I64_LOAD32_U) ||
          op == WASM_OP_I64_STORE || op == WASM_OP_F64_STORE ||
          (op >= WASM_OP_I64_STORE8 && op <= WASM_OP_I64_STORE32) ||
          (op >= WASM_OP_I64_EQZ && op <= WASM_OP_I64_GE_U) ||
          (op >= WASM_OP_F64_EQ && op <= WASM_OP_F64_GE) ||
          (op >= WASM_OP_I64_CLZ && op <= WASM_OP_I64_ROTR) ||
          (op >= WASM_OP_F64_ABS && op <= WASM_OP_F64_COPYSIGN) ||
          (op >= WASM_OP_I32_WRAP_I64 && op <= WASM_OP_F64_REINTERPRET_I64) ||
          (op >= WASM_OP_I64_EXTEND8_S && op <= WASM_OP_I64_EXTEND32_S))
        {
          c->wide = true;
        }

      prev = op;

      if (op == WASM_OP_BLOCK || op == WASM_OP_LOOP || op == WASM_OP_IF)
        {
          if (depth < sizeof(loops))
            {
              loops[depth] = op == WASM_OP_LOOP;
            }

          depth++;
          ld += op == WASM_OP_LOOP;
        }
      else if (op == WASM_OP_END && depth > 0)
        {
          depth--;
          if (depth < sizeof(loops) && loops[depth])
            {
              ld--;
            }
        }
      else if (op == WASM_OP_GET_LOCAL || op == WASM_OP_SET_LOCAL ||
               op == WASM_OP_TEE_LOCAL)
        {
          uint32_t l = rd_u32(&q);

          if (l < c->nlocals)
            {
              c->lweight[l] += w;
            }
        }
      else if (ld > 0 && (op == WASM_OP_I32_CONST || op == WASM_OP_F32_CONST))
        {
          uint32_t v = op == WASM_OP_I32_CONST ? (uint32_t)rd_s64(&q, 32) :
                       rd_le32(&q);

          if (op == WASM_OP_F32_CONST || !cheap_imm(v))
            {
              count_const(c, v, op == WASM_OP_F32_CONST, w);
            }
        }
      else if (op == WASM_OP_CALL || op == WASM_OP_CALL_INDIRECT)
        {
          uint32_t idx = rd_u32(&q);
          WASMFuncType *t = op == WASM_OP_CALL ? func_type_of(c->mod, idx) :
                            (WASMFuncType *)c->mod->types[idx];
          uint32_t aw = type_words(t->types, t->param_count);
          uint32_t rw = type_words(t->types + t->param_count,
                                   t->result_count);
          uint32_t need = aw > 4 ? aw - 4 : 0;

          if (rw > 2 && rw > need)
            {
              need = rw;
            }

          if (need > c->out_words)
            {
              c->out_words = need;
            }

          c->makes_calls = true;
          c->callw += w;
        }

      else if (op == WASM_OP_MISC_PREFIX)
        {
          uint32_t sub = rd_u32(&q);

          if (sub == WASM_OP_MEMORY_INIT || sub == WASM_OP_TABLE_INIT ||
              sub == WASM_OP_TABLE_COPY || sub == WASM_OP_TABLE_FILL)
            {
              if (c->out_words < 1)
                {
                  c->out_words = 1;
                }
            }
        }

      if (!skip_imm(op, &p))
        {
          if (op == WASM_OP_MISC_PREFIX)
            {
              q = p;
              fail(c, "instruction 0xfc %u not supported", rd_u32(&q));
            }
          else
            {
              fail(c, "instruction 0x%02x not supported", op);
            }

          return false;
        }
    }

  c->leaf = !c->makes_calls && !helpers;
  return true;
}

/* Which locals live in registers; the frame's layout.  A function that
 * calls keeps them in callee-saved registers (r4-r8, s16-s31: calls leave
 * them); a leaf (no calls, not even of the runtime's helpers) first in
 * the caller-saved ones (nothing to save), its parameters in the
 * registers they come in.  At most 5 core registers go to locals and
 * constants: the rest are the temporaries.
 */

static int free_reg(const int8_t *order, int n, uint32_t taken)
{
  int i;

  for (i = 0; i < n; i++)
    {
      if (!(taken & (1u << order[i])))
        {
          return order[i];
        }
    }

  return -1;
}

static void assign_locals(jc_t *c)
{
  static const int8_t core_call[] =
  {
    4, 5, 6, 7, 8
  };

  static const int8_t core_leaf[] =
  {
    0, 1, 2, 3, 4, 5, 6, 7, 8
  };

  static const int8_t s_call[] =
  {
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
  };

  static const int8_t s_leaf[] =
  {
    15, 14, 13, 12, 11, 10, 9, 8,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
  };

  const int8_t *cr = c->leaf ? core_leaf : core_call;
  const int8_t *sr = c->leaf ? s_leaf : s_call;
  int ncr = c->leaf ? 9 : 5;
  int nsr = c->leaf ? 24 : 16;
  uint32_t taken = 0;           /* Core registers of locals, constants */
  uint32_t reserved = 0;        /* ... and parameters' until the prologue */
  uint32_t staken = 0;
  uint32_t ncore = 0;
  uint32_t lim5 = c->loc_max < 5 ? c->loc_max : 5;
  uint32_t nframe = 0;
  uint32_t i;
  uint32_t w = 0;

  for (i = 0; i < c->nlocals; i++)
    {
      c->lkind[i] = L_FRAME;
    }

  /* A leaf's parameters where they come */

  for (i = 0; c->leaf && i < c->nparams; i++)
    {
      int ty = c->ltype[i];

      if (c->lweight[i] > 0 && ty == T_I32 && w < 4 && ncore < lim5)
        {
          c->lkind[i] = L_CORE;
          c->lreg[i] = (int8_t)w;
          taken |= 1u << w;
          ncore++;
        }
      else if (c->lweight[i] > 0 && ty == T_I64 && w + 1 < 4 &&
               ncore + 1 < lim5)
        {
          c->lkind[i] = L_CORE;
          c->lreg[i] = (int8_t)w;
          c->lreg2[i] = (int8_t)(w + 1);
          taken |= 3u << w;
          ncore += 2;
        }
      else if (c->lweight[i] > 0)
        {
          /* (its registers hold it until the prologue has moved it) */

          reserved |= (w < 4 ? 1u << w : 0) |
                      (IS64(ty) && w + 1 < 4 ? 1u << (w + 1) : 0);
        }

      w += TWORDS(ty);
    }

  /* The heaviest first: 5 core registers for any, 2 more for those used
   * in loops (LOCAL_HOT: 3 temporaries left); a function that calls has
   * them in r3, r2, saved and loaded again around each call, for a local
   * used more than its calls cost
   */

  for (;;)
    {
      uint32_t best = 0;
      int bi = -1;

      for (i = 0; i < c->nlocals; i++)
        {
          uint32_t cap = c->lweight[i] >= LOCAL_HOT || c->loc_max < 5 ?
                         c->loc_max : 5;

          if (c->lkind[i] == L_FRAME && c->lweight[i] > best &&
              ((c->ltype[i] == T_I32 && ncore < cap) ||
               (c->ltype[i] == T_I64 && ncore + 1 < cap) ||
               c->ltype[i] == T_F32))
            {
              best = c->lweight[i];
              bi = (int)i;
            }
        }

      if (bi < 0)
        {
          break;
        }

      if (c->ltype[bi] == T_F32)
        {
          int r = free_reg(sr, nsr, staken);

          if (r < 0)
            {
              c->lkind[bi] = L_KEPT;        /* (stays in the frame) */
              continue;
            }

          c->lkind[bi] = L_SREG;
          c->lreg[bi] = (int8_t)r;
          staken |= 1u << r;
        }
      else
        {
          int r = free_reg(cr, ncr, taken | reserved);
          int r2 = -1;

          if (r >= 0 && c->ltype[bi] == T_I64)
            {
              r2 = free_reg(cr, ncr, taken | reserved | 1u << r);
            }

          if (r < 0 && !c->leaf && c->ltype[bi] == T_I32 &&
              (uint32_t)bi >= c->nparams && c->ncsl < 2 &&
              c->lweight[bi] > 2 * c->callw)
            {
              static const int8_t core_csl[] =
              {
                3, 2
              };

              r = free_reg(core_csl, 2, taken | reserved);
              if (r >= 0)
                {
                  c->csl[c->ncsl++] = (int16_t)bi;
                }
            }

          if (r < 0 || (c->ltype[bi] == T_I64 && r2 < 0))
            {
              c->lkind[bi] = L_KEPT;
              continue;
            }

          c->lkind[bi] = L_CORE;
          c->lreg[bi] = (int8_t)r;
          taken |= 1u << r;
          ncore++;
          if (r2 >= 0)
            {
              c->lreg2[bi] = (int8_t)r2;
              taken |= 1u << r2;
              ncore++;
            }
        }
    }

  /* The heaviest constants of loops into the registers left (a constant
   * used twice in a loop, or once in a nested one)
   */

  c->npin = 0;
  for (;;)
    {
      uint32_t best = 7;
      int bi = -1;
      int r;

      for (i = 0; i < c->nkc; i++)
        {
          if (c->kc[i].w > best && (c->kc[i].f32 || ncore < lim5) &&
              c->npin < sizeof(c->pin) / sizeof(c->pin[0]))
            {
              best = c->kc[i].w;
              bi = (int)i;
            }
        }

      if (bi < 0)
        {
          break;
        }

      r = c->kc[bi].f32 ? free_reg(sr, nsr, staken) :
                          free_reg(cr, ncr, taken | reserved);
      c->kc[bi].w = 0;
      if (r < 0)
        {
          continue;
        }

      c->pin[c->npin].v = c->kc[bi].v;
      c->pin[c->npin].f32 = c->kc[bi].f32;
      c->pin[c->npin].r = (int8_t)r;
      if (c->kc[bi].f32)
        {
          staken |= 1u << r;
        }
      else
        {
          taken |= 1u << r;
          ncore++;
        }

      c->npin++;
    }

  /* Locals the code never reads or writes: nowhere */

  for (i = 0; i < c->nlocals; i++)
    {
      if (c->lkind[i] == L_KEPT)
        {
          c->lkind[i] = L_FRAME;
        }
      else if (c->lkind[i] == L_FRAME && c->lweight[i] == 0)
        {
          c->lkind[i] = L_NONE;
        }
    }

  c->cused = taken & CALLEE_TEMPS;
  c->sused = (uint32_t)__builtin_popcount(staken >> 16);
  for (i = 0; i < c->nlocals; i++)
    {
      if (c->lkind[i] == L_FRAME)
        {
          nframe++;
        }
    }

  /* sp: the outgoing area, the locals in the frame, the value stack's
   * slots (as many as the body uses: the frame's size is known at its end)
   */

  c->local_base = (4 * c->out_words + 7) & ~7u;
  nframe = 0;
  for (i = 0; i < c->nlocals; i++)
    {
      if (c->lkind[i] == L_FRAME)
        {
          c->loff[i] = c->local_base + 8 * nframe++;
        }
    }

  for (i = 0; i < c->ncsl; i++)
    {
      c->loff[c->csl[i]] = c->local_base + 8 * nframe++;    /* (homes) */
    }

  c->slot_base = c->local_base + 8 * nframe;
  c->nslots = 0;

  /* The temporaries: what no local or constant holds */

  c->ctemp = (CALLER_SAVED | CALLEE_TEMPS) & ~taken;
  c->cfree = c->ctemp;
  c->stemp = 0xffffu & ~staken;
  c->sfree = c->stemp;
  c->lr_used = false;
}

/* The prologue (into mb, before the body is placed) */

static uint32_t emit_prologue(jc_t *c, uint32_t push)
{
  jbuf_t *b = &c->mb;
  uint32_t start = b->len;
  uint32_t pushed = 4 * __builtin_popcount(push) + 4 * c->sused;
  uint32_t in_base;
  uint32_t w = 0;
  uint32_t i;
  bool zero = false;
  int scr = (push & (1u << R_LR)) ? R_LR : R_IP;   /* (lr when it is saved) */

  if (push != 0)
    {
      t_push(b, push);
    }

  if (c->sused > 0)
    {
      e32(b, 0xed2d | (16 & 1) << 6, (16 >> 1) << 12 | 0x0a00 | c->sused);
    }

  in_base = c->frame_size + pushed;

  if (c->frame_size > 0 || c->makes_calls)
    {
      if (c->makes_calls || c->frame_size > 256)
        {
          uint32_t pos;

          t_addi(b, R_IP, R_SP, -(int32_t)c->frame_size);
          t_ldst(b, LS_LDR, R_LR, JR_INST, EXT_OFF(EXT_LIMIT));
          t_cmp(b, R_IP, R_LR);
          pos = b->len;
          t_bcc_at(b, (e32(b, 0, 0), pos), CC_LO,
                   (int32_t)c->trap_pos[JT_STACK] - (int32_t)pos);
          t_mov(b, R_SP, R_IP);
        }
      else
        {
          t_sp_add(b, -(int32_t)c->frame_size);
        }
    }

  /* The parameters to their homes */

  for (i = 0; i < c->nparams; i++)
    {
      int ty = c->ltype[i];
      uint32_t k;

      for (k = 0; k < (uint32_t)TWORDS(ty); k++, w++)
        {
          int src = w < 4 ? (int)(R0 + w) : -1;

          if (c->lkind[i] == L_NONE)
            {
              continue;
            }

          if (c->lkind[i] == L_CORE)
            {
              int dst = k ? c->lreg2[i] : c->lreg[i];

              if (src >= 0)
                {
                  t_mov(b, dst, src);
                }
              else
                {
                  t_ldst(b, LS_LDR, dst, R_SP, in_base + 4 * (w - 4));
                }
            }
          else if (c->lkind[i] == L_SREG)
            {
              if (src < 0)
                {
                  t_ldst(b, LS_LDR, scr, R_SP, in_base + 4 * (w - 4));
                  src = scr;
                }

              t_vmov_sr(b, c->lreg[i], src);
            }
          else
            {
              if (src < 0)
                {
                  t_ldst(b, LS_LDR, scr, R_SP, in_base + 4 * (w - 4));
                  src = scr;
                }

              t_ldst(b, LS_STR, src, R_SP, c->loff[i] + 4 * k);
            }
        }
    }

  /* The other locals start at 0 (references at null: WAMR's NULL_REF) */

  for (i = c->nparams; i < c->nlocals; i++)
    {
      uint8_t wt = c->func->local_types[i - c->nparams];

      if (c->lkind[i] == L_NONE || c->lfirst[i] == 2)
        {
          continue;                 /* (set before it is read) */
        }

      if (wt == VALUE_TYPE_FUNCREF || wt == VALUE_TYPE_EXTERNREF)
        {
          if (c->lkind[i] == L_CORE)
            {
              t_movi(b, c->lreg[i], NULL_REF);
            }
          else
            {
              t_movi(b, scr, NULL_REF);
              t_ldst(b, LS_STR, scr, R_SP, (int32_t)c->loff[i]);
              zero = false;
            }

          continue;
        }

      if (!zero && c->lkind[i] != L_CORE)
        {
          t_movi(b, scr, 0);
          zero = true;
        }

      if (c->lkind[i] == L_CORE)
        {
          t_movi(b, c->lreg[i], 0);
          if (IS64(c->ltype[i]))
            {
              t_movi(b, c->lreg2[i], 0);
            }
        }
      else if (c->lkind[i] == L_SREG)
        {
          t_vmov_sr(b, c->lreg[i], scr);
        }
      else
        {
          t_ldst(b, LS_STR, scr, R_SP, c->loff[i]);
          if (IS64(c->ltype[i]))
            {
              t_ldst(b, LS_STR, scr, R_SP, c->loff[i] + 4);
            }
        }
    }

  /* The constants kept in registers */

  for (i = 0; i < c->npin; i++)
    {
      if (c->pin[i].f32)
        {
          t_movi(b, scr, c->pin[i].v);
          t_vmov_sr(b, c->pin[i].r, scr);
        }
      else
        {
          t_movi(b, c->pin[i].r, c->pin[i].v);
        }
    }

  /* The body word-aligned (its loops' first instructions are: a 32-bit
   * instruction across two words takes the Cortex-M33 another fetch at
   * every branch to it)
   */

  if ((b->len - start) & 2)
    {
      e16(b, 0xbf00);
    }

  return b->len - start;
}

static void emit_epilogue(jc_t *c, uint32_t push)
{
  WASMFuncType *t = c->ftype;
  const uint8_t *rt = t->types + t->param_count;
  uint32_t pushed = 4 * __builtin_popcount(push) + 4 * c->sused;

  lbind(c, c->epilogue);
  if (type_words(rt, t->result_count) > 2)
    {
      uint32_t w = 0;
      uint32_t i;
      uint32_t k;

      for (i = 0; i < t->result_count; i++)
        {
          int ty = jtype(rt[i]);

          for (k = 0; k < (uint32_t)TWORDS(ty); k++, w++)
            {
              ld_sp(c, R_LR, slot_off(c, i) + 4 * k);
              st_sp(c, R_LR, c->frame_size + pushed + 4 * w);
            }
        }
    }

  t_sp_add(&c->fb, (int32_t)c->frame_size);
  if (c->sused > 0)
    {
      e32(&c->fb, 0xecbd | (16 & 1) << 6,
          (16 >> 1) << 12 | 0x0a00 | c->sused);
    }

  if (push & (1u << R_LR))
    {
      t_pop(&c->fb, (push & ~(1u << R_LR)) | (1u << R_PC));
    }
  else
    {
      if (push != 0)
        {
          t_pop(&c->fb, push);
        }

      t_bx(&c->fb, R_LR);
    }
}

static void op_local_set(jc_t *c, uint32_t l, bool tee)
{
  val_t v = pop(c);
  int k = c->lkind[l];

  local_changing(c, l, true);
  if (k == L_CORE)
    {
      bc_forget(c, c->lreg[l]);
      if (IS64(c->ltype[l]))
        {
          bc_forget(c, c->lreg2[l]);
        }
    }

  if (k == L_CORE && IS64(c->ltype[l]))
    {
      int w;

      for (w = 0; w < 2; w++)
        {
          int dst = w ? c->lreg2[l] : c->lreg[l];
          int src = word_reg(c, &v, w);

          if (src >= 0)
            {
              t_mov(&c->fb, dst, src);
            }
          else
            {
              word_to(c, dst, &v, w);
            }
        }
    }
  else if (k == L_CORE)
    {
      int dst = c->lreg[l];

      if (v.kind == V_CONST)
        {
          t_movi(&c->fb, dst, (uint32_t)v.c);
        }
      else if (v.kind == V_LOCK)
        {
          t_addi(&c->fb, dst, c->lreg[v.local], (int32_t)v.c);
        }
      else if (v.kind == V_SLOT)
        {
          ld_sp(c, dst, slot_off(c, v.depth));
        }
      else if (v.kind == V_LOCAL && c->lkind[v.local] == L_FRAME)
        {
          ld_sp(c, dst, c->loff[v.local]);
        }
      else
        {
          t_mov(&c->fb, dst, vreg(c, &v));
        }
    }
  else if (k == L_SREG)
    {
      int dst = c->lreg[l];

      if (v.kind == V_SLOT)
        {
          vldst_sp(c, true, dst, slot_off(c, v.depth));
        }
      else if (v.kind == V_CONST)
        {
          t_movi(&c->fb, R_IP, (uint32_t)v.c);
          t_vmov_sr(&c->fb, dst, R_IP);
        }
      else
        {
          int s = vsreg(c, &v);

          if (s != dst)
            {
              t_vop2(&c->fb, VOP_MOV, dst, s);
            }
        }
    }
  else
    {
      store_slot(c, &v, c->loff[l]);
      if (tee && (v.kind == V_REG || v.kind == V_CONST))
        {
          /* The value stays where it is (no load back) */

          val_t *t = vpush(c, v.kind, v.type);

          t->r  = v.r;
          t->r2 = v.r2;
          t->c  = v.c;
          return;
        }
    }

  if (tee && k == L_CORE && IS64(c->ltype[l]) && v.kind == V_REG)
    {
      /* The pair it came in stays the tee's value: what uses it next
       * (in place: i64 operations write their first operand) copies
       * nothing
       */

      push_reg(c, v.type, v.r, v.r2);
      return;
    }

  free_val(c, &v);
  if (tee)
    {
      val_t *t = vpush(c, V_LOCAL, c->ltype[l]);

      t->local = l;
    }
}

static void op_global_get(jc_t *c, uint32_t g)
{
  WASMGlobalInstance *gi = c->inst->e->globals + g;
  uint8_t *addr = c->inst->global_data + gi->data_offset;
  uint32_t off = (uint32_t)(addr - (uint8_t *)c->inst);
  int ty = jtype(gi->type);
  int r;
  int r2;

  if (!gi->is_mutable)
    {
      uint64_t v = 0;

      memcpy(&v, addr, TWORDS(ty) * 4);
      push_const(c, ty, v);
      return;
    }

  if (ty == T_F32)
    {
      int s = alloc_s(c);

      if (off <= 1020)
        {
          t_vldst(&c->fb, true, s, JR_INST, (int32_t)off);
        }
      else
        {
          ld_inst(c, LS_LDR, R_IP, off);
          t_vmov_sr(&c->fb, s, R_IP);
        }

      push_reg(c, T_F32, s, -1);
      return;
    }

  r = IS64(ty) ? alloc_core(c) : alloc_dest(c);
  ld_inst(c, LS_LDR, r, off);
  if (IS64(ty))
    {
      r2 = alloc_core(c);
      ld_inst(c, LS_LDR, r2, off + 4);
      push_reg(c, ty, r, r2);
      return;
    }

  push_reg(c, T_I32, r, -1);
}

static void st_inst(jc_t *c, int r, uint32_t off)
{
  if (off < 4096)
    {
      t_ldst(&c->fb, LS_STR, r, JR_INST, (int32_t)off);
    }
  else
    {
      t_movi(&c->fb, R_IP, off);
      t_ldst_reg(&c->fb, LS_STR, r, JR_INST, R_IP, 0);
    }
}

static void op_global_set(jc_t *c, uint32_t g)
{
  WASMGlobalInstance *gi = c->inst->e->globals + g;
  uint32_t off = (uint32_t)(c->inst->global_data + gi->data_offset -
                            (uint8_t *)c->inst);
  val_t v = pop(c);
  int ty = jtype(gi->type);

  if (ty == T_F32)
    {
      int s = vsreg(c, &v);
      int r = alloc_core(c);

      t_vmov_rs(&c->fb, r, s);
      st_inst(c, r, off);
      free_core(c, r);
    }
  else if (IS64(ty))
    {
      int lo;
      int hi;

      vreg64(c, &v, &lo, &hi);
      st_inst(c, lo, off);
      st_inst(c, hi, off + 4);
    }
  else
    {
      int r = vreg(c, &v);

      if (c->mod->aux_stack_size > 0 &&
          g == c->mod->aux_stack_top_global_index)
        {
          /* The shadow stack: as the interpreter, its bounds checked */

          int t = alloc_core(c);

          ld_inst(c, LS_LDR, t, EXT_OFF(EXT_AUXB));
          t_cmp(&c->fb, r, t);
          trap_if(c, CC_LS, JT_AUX_OVERFLOW);
          ld_inst(c, LS_LDR, t, EXT_OFF(EXT_AUXT));
          t_cmp(&c->fb, r, t);
          trap_if(c, CC_HI, JT_AUX_UNDERFLOW);
          free_core(c, t);
        }

      st_inst(c, r, off);
    }

  free_val(c, &v);
}

static void op_br_table(jc_t *c, const uint8_t **pp)
{
  const uint8_t *p = *pp;
  uint32_t n = rd_u32(&p);
  const uint8_t *targets = p;
  uint32_t i;
  int dflt;
  int *frag;
  uint32_t base;
  val_t vi = pop(c);
  int ri;

  ri = vreg(c, &vi);
  spill_all(c);

  frag = jit_zalloc((n + 1) * sizeof(int));
  if (frag == NULL)
    {
      fail(c, "out of memory", 0);
      return;
    }

  /* A target that needs no moves is branched to directly (the table's
   * entries only go forward: a loop's start needs a branch of its own);
   * the others through a fragment of moves and a branch
   */

  p = targets;
  for (i = 0; i <= n; i++)
    {
      ctrl_t *ct = ctrl_at(c, rd_u32(&p));

      if (ct->kind != CK_FUNC && !br_needs_moves(c, ct) &&
          (i == n || (!c->tbh_near && c->labels[ct->label].pos < 0)))
        {
          frag[i] = -1 - ct->label;
        }
      else
        {
          frag[i] = new_label(c);
        }
    }

  dflt = frag[n] < 0 ? -1 - frag[n] : frag[n];
  cmp_imm(c, ri, n);
  bcc_to(c, CC_HS, dflt);
  free_val(c, &vi);

  /* tbh [pc, ri, lsl #1] and the table */

  e32(&c->fb, 0xe8d0 | R_PC, 0xf010 | ri);
  base = c->fb.len;
  for (i = 0; i < n; i++)
    {
      uint32_t pos = c->fb.len;

      e16(&c->fb, 0);
      add_fix(c, frag[i] < 0 ? -1 - frag[i] : frag[i], F_TBH, 0, pos, base);
    }

  if (c->fb.len & 2)
    {
      e16(&c->fb, 0xbf00);
    }

  /* Each other target's moves and branch */

  p = targets;
  for (i = 0; i <= n; i++)
    {
      uint32_t depth = rd_u32(&p);

      if (frag[i] >= 0)
        {
          lbind(c, frag[i]);
          op_br(c, depth);
        }
    }

  jit_free(frag);
  c->cs[c->csp - 1].unreach = true;
  *pp = p;
}

/* The instructions of the misc prefix (bulk memory, tables) */

static void kval(val_t *v, uint32_t k)
{
  memset(v, 0, sizeof(*v));
  v->kind = V_CONST;
  v->type = T_I32;
  v->c = k;
}

static void op_misc(jc_t *c, const uint8_t **pp)
{
  const uint8_t *p = *pp;
  uint32_t sub = rd_u32(&p);
  val_t a[4];
  uint32_t x;
  uint32_t y;

  if (sub <= WASM_OP_I64_TRUNC_SAT_U_F64)
    {
      op_trunc_sat(c, sub);
      *pp = p;
      return;
    }

  switch (sub)
    {
      case WASM_OP_MEMORY_INIT:             /* (inst, seg, d, s, n) */
        x = rd_u32(&p);
        rd_u32(&p);
        a[3] = pop(c);
        a[2] = pop(c);
        a[1] = pop(c);
        kval(&a[0], x);
        ccall(c, (void *)jit_mem_init, "Iwwww", a, T_I32, false);
        break;

      case WASM_OP_DATA_DROP:
        kval(&a[0], rd_u32(&p));
        ccall(c, (void *)jit_data_drop, "Iw", a, T_I32, false);
        break;

      case WASM_OP_MEMORY_COPY:             /* (inst, d, s, n) */
        rd_u32(&p);
        rd_u32(&p);
        a[2] = pop(c);
        a[1] = pop(c);
        a[0] = pop(c);
        ccall(c, (void *)jit_mem_copy, "Iwww", a, T_I32, false);
        break;

      case WASM_OP_MEMORY_FILL:             /* (inst, d, val, n) */
        rd_u32(&p);
        a[2] = pop(c);
        a[1] = pop(c);
        a[0] = pop(c);
        ccall(c, (void *)jit_mem_fill, "Iwww", a, T_I32, false);
        break;

      case WASM_OP_TABLE_INIT:              /* (inst, seg << 16 | tbl, d, s, n) */
        x = rd_u32(&p);
        y = rd_u32(&p);
        a[3] = pop(c);
        a[2] = pop(c);
        a[1] = pop(c);
        kval(&a[0], x << 16 | y);
        ccall(c, (void *)jit_table_init, "Iwwww", a, T_I32, false);
        break;

      case WASM_OP_ELEM_DROP:
        kval(&a[0], rd_u32(&p));
        ccall(c, (void *)jit_elem_drop, "Iw", a, T_I32, false);
        break;

      case WASM_OP_TABLE_COPY:              /* (inst, dt << 16 | st, d, s, n) */
        x = rd_u32(&p);
        y = rd_u32(&p);
        a[3] = pop(c);
        a[2] = pop(c);
        a[1] = pop(c);
        kval(&a[0], x << 16 | y);
        ccall(c, (void *)jit_table_copy, "Iwwww", a, T_I32, false);
        break;

      case WASM_OP_TABLE_GROW:              /* (inst, tbl, init, n) */
        x = rd_u32(&p);
        a[2] = pop(c);
        a[1] = pop(c);
        kval(&a[0], x);
        ccall(c, (void *)jit_table_grow, "Iwww", a, T_I32, true);
        break;

      case WASM_OP_TABLE_SIZE:
        x = rd_u32(&p);
        {
          int r = alloc_core(c);

          ld_inst(c, LS_LDR, r, table_off(c, x) +
                  offsetof(WASMTableInstance, cur_size));
          push_reg(c, T_I32, r, -1);
        }
        break;

      case WASM_OP_TABLE_FILL:              /* (inst, tbl, i, val, n) */
        x = rd_u32(&p);
        a[3] = pop(c);
        a[2] = pop(c);
        a[1] = pop(c);
        kval(&a[0], x);
        ccall(c, (void *)jit_table_fill, "Iwwww", a, T_I32, false);
        break;
    }

  *pp = p;
}

/****************************************************************************
 * The body
 ****************************************************************************/

static void op_else(jc_t *c)
{
  ctrl_t *ct = &c->cs[c->csp - 1];

  if (ct->dead)
    {
      return;
    }

  if (!ct->unreach)
    {
      br_moves(c, ct);
      br_to(c, ct->label);
    }

  lbind(c, ct->else_label);
  reset_stack(c, ct, false);
  ct->kind = CK_ELSE;
  ct->unreach = false;
}

/* A loop's head (word-aligned): a nop.w that wasm_jit_interrupt() makes a
 * branch to the unwinding (WAMR's terminate request: the watchdog,
 * Ctrl-C), so that a loop that never ends is stopped at no cost
 * otherwise; written as one aligned word, a core running the loop sees
 * either instruction whole
 */

static void loop_head(jc_t *c)
{
  if (!GROW(c, c->lheads, c->nlheads, c->lhcap))
    {
      return;
    }

  c->lheads[c->nlheads++] = c->fb.len;
  e32(&c->fb, 0xf3af, 0x8000);                       /* nop.w */
}

static void compile_body(jc_t *c)
{
  const uint8_t *p = c->code;
  ctrl_t *ct;

  if (!GROW(c, c->cs, c->csp, c->ccap))
    {
      return;
    }

  ct = &c->cs[c->csp++];
  memset(ct, 0, sizeof(*ct));
  ct->kind = CK_FUNC;
  ct->nr = c->ftype->result_count;
  ct->rt = c->ftype->types + c->ftype->param_count;
  ct->label = c->epilogue;

  while (p < c->code_end && c->csp > 0 && !c->failed && !c->fb.oom)
    {
      uint8_t op = *p++;
      ctrl_t *top = &c->cs[c->csp - 1];
      val_t a;
      uint32_t x;
      uint32_t y;

      if (top->unreach || top->dead)
        {
          /* Unreachable code: only its structure */

          if (op == WASM_OP_BLOCK || op == WASM_OP_LOOP || op == WASM_OP_IF)
            {
              skip_imm(op, &p);
              if (!GROW(c, c->cs, c->csp, c->ccap))
                {
                  return;
                }

              ct = &c->cs[c->csp++];
              memset(ct, 0, sizeof(*ct));
              ct->kind = CK_BLOCK;
              ct->dead = true;
            }
          else if (op == WASM_OP_ELSE)
            {
              op_else(c);
            }
          else if (op == WASM_OP_END)
            {
              op_end(c);
            }
          else
            {
              skip_imm(op, &p);
            }

          continue;
        }

      c->hint = -1;
      c->shint = -1;
      c->hint_used = false;
      if ((op >= WASM_OP_I32_EQZ && op <= WASM_OP_I32_GE_U) ||
          (op >= WASM_OP_I32_CLZ && op <= WASM_OP_I32_ROTR) ||
          op == WASM_OP_I32_EXTEND8_S || op == WASM_OP_I32_EXTEND16_S ||
          (op >= WASM_OP_F32_ABS && op <= WASM_OP_F32_DIV) ||
          op == WASM_OP_F32_CONVERT_S_I32 || op == WASM_OP_F32_CONVERT_U_I32)
        {
          set_hint(c, p);
        }

      switch (op)
        {
          case WASM_OP_UNREACHABLE:
            trap_if(c, CC_AL, JT_UNREACHABLE);
            top->unreach = true;
            break;

          case WASM_OP_NOP:
            break;

          case WASM_OP_BLOCK:
            spill_all(c);
            push_ctrl(c, CK_BLOCK, &p);
            spill_from(c, c->cs[c->csp - 1].height);
            c->cs[c->csp - 1].label = new_label(c);
            res_choose(c, &c->cs[c->csp - 1]);
            break;

          case WASM_OP_LOOP:
            spill_all(c);
            push_ctrl(c, CK_LOOP, &p);
            spill_from(c, c->cs[c->csp - 1].height);
            ct = &c->cs[c->csp - 1];
            ct->label = new_label(c);
            if (c->fb.len & 2)
              {
                e16(&c->fb, 0xbf00);          /* (the loop's start aligned) */
              }

            lbind(c, ct->label);
            loop_head(c);
            break;

          case WASM_OP_IF:
            a = pop(c);
            op_if(c, &p, CC_NE, &a);
            break;

          case WASM_OP_ELSE:
            op_else(c);
            break;

          case WASM_OP_END:
            op_end(c);
            break;

          case WASM_OP_BR:
            op_br(c, rd_u32(&p));
            break;

          case WASM_OP_BR_IF:
            a = pop(c);
            x = rd_u32(&p);
            if (a.kind == V_CONST)
              {
                if ((uint32_t)a.c != 0)
                  {
                    op_br(c, x);
                  }

                break;
              }

            br_cond(c, x, test_val(c, &a));
            break;

          case WASM_OP_BR_TABLE:
            op_br_table(c, &p);
            break;

          case WASM_OP_RETURN:
            ret_moves(c);
            br_to(c, c->epilogue);
            top->unreach = true;
            break;

          case WASM_OP_CALL:
            op_call(c, rd_u32(&p));
            break;

          case WASM_OP_CALL_INDIRECT:
            x = rd_u32(&p);
            y = rd_u32(&p);
            op_call_indirect(c, x, y);
            break;

          case WASM_OP_DROP:
            a = pop(c);
            free_val(c, &a);
            break;

          case WASM_OP_SELECT_T:
            x = rd_u32(&p);
            p += x;

            /* fall through */

          case WASM_OP_SELECT:
            a = pop(c);
            if (a.kind == V_CONST)
              {
                val_t b2 = pop(c);
                val_t a2 = pop(c);
                val_t *v;

                if ((uint32_t)a.c != 0)
                  {
                    free_val(c, &b2);
                    v = vpush(c, a2.kind, a2.type);
                    x = v->depth;
                    *v = a2;
                    v->depth = x;
                    if (a2.kind == V_SLOT && a2.depth != x)
                      {
                        fail(c, "select slot", 0);
                      }
                  }
                else
                  {
                    free_val(c, &a2);
                    if (b2.kind == V_SLOT)
                      {
                        /* Its slot is one up: into a register */

                        if (IS64(b2.type))
                          {
                            int lo;
                            int hi;

                            vreg64(c, &b2, &lo, &hi);
                          }
                        else if (b2.type == T_F32)
                          {
                            vsreg(c, &b2);
                          }
                        else
                          {
                            vreg(c, &b2);
                          }
                      }

                    v = vpush(c, b2.kind, b2.type);
                    x = v->depth;
                    *v = b2;
                    v->depth = x;
                  }

                break;
              }

            op_select_cond(c, test_val(c, &a));
            break;

          case WASM_OP_GET_LOCAL:
            x = rd_u32(&p);
            vpush(c, V_LOCAL, c->ltype[x])->local = x;
            break;

          case WASM_OP_SET_LOCAL:
            op_local_set(c, rd_u32(&p), false);
            break;

          case WASM_OP_TEE_LOCAL:
            op_local_set(c, rd_u32(&p), true);
            break;

          case WASM_OP_GET_GLOBAL:
            x = rd_u32(&p);
            set_hint(c, p);
            op_global_get(c, x);
            break;

          case WASM_OP_SET_GLOBAL:
            op_global_set(c, rd_u32(&p));
            break;

          case WASM_OP_TABLE_GET:
            {
              val_t t[2];

              kval(&t[0], rd_u32(&p));
              t[1] = pop(c);
              ccall(c, (void *)jit_table_get, "Iww", t, T_I32, true);
            }
            break;

          case WASM_OP_TABLE_SET:
            {
              val_t t[3];

              kval(&t[0], rd_u32(&p));
              t[2] = pop(c);
              t[1] = pop(c);
              ccall(c, (void *)jit_table_set, "Iwww", t, T_I32, false);
            }
            break;

          case WASM_OP_MEMORY_SIZE:
            rd_u32(&p);
            {
              int r = alloc_core(c);

              ld_inst(c, LS_LDR, r, c->mem_off +
                      offsetof(WASMMemoryInstance, cur_page_count));
              push_reg(c, T_I32, r, -1);
            }
            break;

          case WASM_OP_MEMORY_GROW:
            rd_u32(&p);
            a = pop(c);
            ccall(c, (void *)jit_mem_grow, "Iw", &a, T_I32, true);
            reload_mem(c, &c->fb);
            break;

          case WASM_OP_I32_CONST:
            push_const(c, T_I32, (uint32_t)rd_s64(&p, 32));
            break;

          case WASM_OP_I64_CONST:
            push_const(c, T_I64, (uint64_t)rd_s64(&p, 64));
            break;

          case WASM_OP_F32_CONST:
            push_const(c, T_F32, rd_le32(&p));
            break;

          case WASM_OP_F64_CONST:
            x = rd_le32(&p);
            y = rd_le32(&p);
            push_const(c, T_F64, (uint64_t)y << 32 | x);
            break;

          case WASM_OP_I32_EQZ:
            a = pop(c);
            if (a.kind == V_CONST)
              {
                push_const(c, T_I32, (uint32_t)a.c == 0);
                break;
              }

            {
              int r = vreg(c, &a);

              t_cmpi_enc(&c->fb, r, 0);
              free_val(c, &a);
            }

            use_cond(c, CC_EQ, &p);
            break;

          case WASM_OP_I64_EQZ:
            a = pop(c);
            if (a.kind == V_CONST)
              {
                push_const(c, T_I32, a.c == 0);
                break;
              }

            {
              int lo;
              int hi;

              vreg64(c, &a, &lo, &hi);
              t_dp_reg(&c->fb, DP_ORR, 1, R_IP, lo, hi, SH_LSL, 0);
              free_val(c, &a);
            }

            use_cond(c, CC_EQ, &p);
            break;

          case WASM_OP_REF_NULL:
            p++;
            push_const(c, T_I32, 0xffffffffu);
            break;

          case WASM_OP_REF_IS_NULL:
            a = pop(c);
            {
              int r = vreg(c, &a);

              t_cmni_enc(&c->fb, r, 1);
              free_val(c, &a);
            }

            use_cond(c, CC_EQ, &p);
            break;

          case WASM_OP_REF_FUNC:
            push_const(c, T_I32, rd_u32(&p));
            break;

          case WASM_OP_MISC_PREFIX:
            op_misc(c, &p);
            break;

          default:
            if (op >= WASM_OP_I32_LOAD && op <= WASM_OP_I64_LOAD32_U)
              {
                rd_u32(&p);
                x = rd_u32(&p);
                set_hint(c, p);
                op_load(c, op, x);
              }
            else if (op >= WASM_OP_I32_STORE && op <= WASM_OP_I64_STORE32)
              {
                rd_u32(&p);
                op_store(c, op, rd_u32(&p));
              }
            else if (op >= WASM_OP_I32_EQ && op <= WASM_OP_I32_GE_U)
              {
                val_t *pb = peek(c, 0);
                val_t *pa = peek(c, 1);

                if (pa->kind == V_CONST && pb->kind == V_CONST)
                  {
                    int32_t va = (int32_t)pa->c;
                    int32_t vb = (int32_t)pb->c;
                    uint32_t ua = (uint32_t)va;
                    uint32_t ub = (uint32_t)vb;
                    uint32_t r;

                    switch (op)
                      {
                        case WASM_OP_I32_EQ: r = ua == ub; break;
                        case WASM_OP_I32_NE: r = ua != ub; break;
                        case WASM_OP_I32_LT_S: r = va < vb; break;
                        case WASM_OP_I32_LT_U: r = ua < ub; break;
                        case WASM_OP_I32_GT_S: r = va > vb; break;
                        case WASM_OP_I32_GT_U: r = ua > ub; break;
                        case WASM_OP_I32_LE_S: r = va <= vb; break;
                        case WASM_OP_I32_LE_U: r = ua <= ub; break;
                        case WASM_OP_I32_GE_S: r = va >= vb; break;
                        default: r = ua >= ub; break;
                      }

                    c->vsp -= 2;
                    push_const(c, T_I32, r);
                    break;
                  }

                use_cond(c, cmp_i32(c, op), &p);
              }
            else if (op >= WASM_OP_I64_EQ && op <= WASM_OP_I64_GE_U)
              {
                use_cond(c, cmp_i64(c, op), &p);
              }
            else if (op >= WASM_OP_F32_EQ && op <= WASM_OP_F32_GE)
              {
                use_cond(c, cmp_f32(c, op), &p);
              }
            else if (op >= WASM_OP_F64_EQ && op <= WASM_OP_F64_GE)
              {
                op_f64(c, op);
              }
            else if (op >= WASM_OP_I32_CLZ && op <= WASM_OP_I32_POPCNT)
              {
                op_i32_unary(c, op);
              }
            else if (op >= WASM_OP_I32_DIV_S && op <= WASM_OP_I32_REM_U)
              {
                op_i32_div(c, op);
              }
            else if (op >= WASM_OP_I32_ADD && op <= WASM_OP_I32_ROTR)
              {
                if (!((op == WASM_OP_I32_AND || op == WASM_OP_I32_OR ||
                       op == WASM_OP_I32_XOR || op == WASM_OP_I32_SUB) &&
                      fuse_test(c, op, &p)))
                  {
                    op_i32_bin(c, op);
                  }
              }
            else if (op >= WASM_OP_I64_CLZ && op <= WASM_OP_I64_POPCNT)
              {
                op_i64_unary(c, op);
              }
            else if (op >= WASM_OP_I64_DIV_S && op <= WASM_OP_I64_REM_U)
              {
                op_i64_div(c, op);
              }
            else if (op >= WASM_OP_I64_ADD && op <= WASM_OP_I64_ROTR)
              {
                op_i64_bin(c, op);
              }
            else if (op >= WASM_OP_F32_ABS && op <= WASM_OP_F32_SQRT)
              {
                op_f32_unary(c, op);
              }
            else if (op >= WASM_OP_F32_ADD && op <= WASM_OP_F32_COPYSIGN)
              {
                op_f32_bin(c, op);
              }
            else if (op >= WASM_OP_F64_ABS && op <= WASM_OP_F64_COPYSIGN)
              {
                op_f64(c, op);
              }
            else if (op >= WASM_OP_I32_WRAP_I64 &&
                     op <= WASM_OP_F64_REINTERPRET_I64)
              {
                op_convert(c, op);
              }
            else if (op == WASM_OP_I32_EXTEND8_S ||
                     op == WASM_OP_I32_EXTEND16_S)
              {
                op_i32_unary(c, op);
              }
            else if (op >= WASM_OP_I64_EXTEND8_S &&
                     op <= WASM_OP_I64_EXTEND32_S)
              {
                op_i64_unary(c, op);
              }
            else
              {
                fail(c, "instruction 0x%02x not supported", op);
              }
            break;
        }

      p = finish_hint(c, p);
    }

  if (!c->failed && c->csp != 0)
    {
      fail(c, "unbalanced blocks", 0);
    }
}

/* Which functions may grow the memory (and move it): memory.grow, an
 * import (the host may call back into the module), call_indirect, or a
 * call of one that may.  The others' callers keep r10 and r9 as they are.
 */

static void find_growers(jc_t *c)
{
  WASMModule *m = c->mod;
  uint32_t ni = m->import_function_count;
  uint32_t n = ni + m->function_count;
  uint32_t *edge = NULL;          /* (caller, callee) pairs */
  uint32_t ne = 0;
  uint32_t cap = 0;
  uint32_t i;
  bool changed = true;

  c->may_grow = jit_zalloc(n + 1);
  if (c->may_grow == NULL)
    {
      return;
    }

  for (i = 0; i < ni; i++)
    {
      c->may_grow[i] = 1;
    }

  for (i = 0; i < m->function_count; i++)
    {
      WASMFunction *f = m->functions[i];
      const uint8_t *p = f->code;
      const uint8_t *end = f->code + f->code_size;

      while (p < end)
        {
          uint8_t op = *p++;
          const uint8_t *q = p;

          if (op == WASM_OP_MEMORY_GROW || op == WASM_OP_CALL_INDIRECT)
            {
              c->may_grow[ni + i] = 1;
            }
          else if (op == WASM_OP_CALL)
            {
              if (ne >= cap)
                {
                  uint32_t *e = jit_realloc(edge, (cap = cap ? cap * 2 : 256)
                                            * 2 * sizeof(uint32_t));

                  if (e == NULL)
                    {
                      jit_free(edge);
                      jit_free(c->may_grow);
                      c->may_grow = NULL;       /* (reload after every call) */
                      return;
                    }

                  edge = e;
                }

              edge[2 * ne] = ni + i;
              edge[2 * ne + 1] = rd_u32(&q);
              ne++;
            }

          if (!skip_imm(op, &p))
            {
              c->may_grow[ni + i] = 1;    /* (not compiled anyway) */
              break;
            }
        }
    }

  while (changed)
    {
      changed = false;
      for (i = 0; i < ne; i++)
        {
          if (!c->may_grow[edge[2 * i]] && c->may_grow[edge[2 * i + 1]])
            {
              c->may_grow[edge[2 * i]] = 1;
              changed = true;
            }
        }
    }

  jit_free(edge);
}

/* Place a body (after its prologue) in the module's code */

static void place_body(jc_t *c)
{
  uint32_t base = c->mb.len;
  uint32_t i;

  if (!jb_grow(&c->mb, c->fb.len))
    {
      return;
    }

  memcpy(c->mb.buf + base, c->fb.buf, c->fb.len);
  c->mb.len += c->fb.len;

  for (i = 0; i < c->nfars; i++)
    {
      far_t *f = &c->fars[i];
      uint32_t pos = base + f->pos;

      if (f->kind == F_BL)
        {
          if (!GROW(c, c->calls, c->ncalls, c->calls_cap))
            {
              return;
            }

          c->calls[c->ncalls].pos = pos;
          c->calls[c->ncalls].target = f->target;
          c->ncalls++;
        }
      else if (f->kind == F_B)
        {
          t_b_at(&c->mb, pos, (int32_t)f->target - (int32_t)pos, false);
        }
      else
        {
          t_bcc_at(&c->mb, pos, f->cond, (int32_t)f->target - (int32_t)pos);
        }
    }

  for (i = 0; i < c->nlheads; i++)
    {
      if (!GROW(c, c->mloops, c->nmloops, c->mlcap))
        {
          return;
        }

      c->mloops[c->nmloops++] = base + c->lheads[i];
    }
}

static bool compile_function1(jc_t *c, uint32_t fidx)
{
  WASMFunction *f = c->mod->functions[fidx];
  uint32_t n;
  uint32_t i;
  uint32_t push;

  c->func   = f;
  c->fidx   = fidx;
  c->ftype  = f->func_type;
  c->code   = f->code;
  c->code_end = f->code + f->code_size;
  c->nparams = c->ftype->param_count;
  c->nlocals = c->nparams + f->local_count;
  n = c->nlocals + 1;

  jit_free(c->ltype);
  jit_free(c->lkind);
  jit_free(c->lreg);
  jit_free(c->lreg2);
  jit_free(c->loff);
  jit_free(c->lweight);
  jit_free(c->lfirst);
  c->lfirst  = jit_zalloc(n);
  c->lreg2   = jit_zalloc(n);
  c->ltype   = jit_zalloc(n);
  c->lkind   = jit_zalloc(n);
  c->lreg    = jit_zalloc(n);
  c->loff    = jit_zalloc(n * sizeof(uint32_t));
  c->lweight = jit_zalloc(n * sizeof(uint32_t));
  if (c->ltype == NULL || c->lkind == NULL || c->lreg == NULL ||
      c->lreg2 == NULL ||
      c->loff == NULL || c->lweight == NULL || c->lfirst == NULL)
    {
      fail(c, "out of memory", 0);
      return false;
    }

  for (i = 0; i < c->nparams; i++)
    {
      if (c->ftype->types[i] == VALUE_TYPE_V128)
        {
          fail(c, "v128 not supported", 0);
          return false;
        }

      c->ltype[i] = (uint8_t)jtype(c->ftype->types[i]);
    }

  for (i = 0; i < f->local_count; i++)
    {
      if (f->local_types[i] == VALUE_TYPE_V128)
        {
          fail(c, "v128 not supported", 0);
          return false;
        }

      c->ltype[c->nparams + i] = (uint8_t)jtype(f->local_types[i]);
    }

  c->out_words = 0;
  c->makes_calls = false;
  c->callw = 0;
  c->ncsl = 0;
  c->nkc = 0;
  c->wide = false;
  if (!prescan(c))
    {
      return false;
    }

  defassign(c);
  if (c->wide && c->loc_max > 5)
    {
      c->loc_max = 5;               /* (64-bit operands: 4 temporaries) */
    }

  c->fb.len = 0;
  c->vsp = 0;
  c->csp = 0;
  c->nlabels = 0;
  c->nfix = 0;
  c->nfars = 0;
  c->ncold = 0;
  c->nlheads = 0;
  c->nbc = 0;
  c->kvalid = 0;
  c->cused = 0;
  assign_locals(c);
  c->epilogue = new_label(c);
  compile_body(c);
  if (c->failed || c->fb.oom)
    {
      if (c->fb.oom)
        {
          fail(c, "out of memory", 0);
        }

      return false;
    }

  /* The multiple results' slots (the epilogue reads them) */

  if (type_words(c->ftype->types + c->ftype->param_count,
                 c->ftype->result_count) > 2)
    {
      slot_off(c, c->ftype->result_count);
    }

  c->frame_size = c->slot_base + 8 * c->nslots;
  if (c->frame_size > 65536)
    {
      fail(c, "frame too large (%u bytes)", c->frame_size);
      return false;
    }

  /* What the prologue saves: lr unless a leaf needs it for nothing (no
   * temporary in it, no frame of locals or stack arguments to set up, no
   * stack check); a function that calls keeps sp 8-byte aligned
   */

  push = c->cused & CALLEE_TEMPS;
  {
    bool frame_locals = false;
    uint32_t i;

    for (i = 0; i < c->nlocals; i++)
      {
        frame_locals = frame_locals || c->lkind[i] == L_FRAME;
      }

    if (!c->leaf || c->lr_used || frame_locals || c->frame_size > 256 ||
        type_words(c->ftype->types, c->nparams) > 4 ||
        type_words(c->ftype->types + c->nparams, c->ftype->result_count) > 2)
      {
        push |= 1u << R_LR;
      }
  }

  if (!c->leaf &&
      ((4 * __builtin_popcount(push) + 4 * c->sused + c->frame_size) & 7)
      != 0)
    {
      c->frame_size += 4;
    }

  emit_epilogue(c, push);
  emit_cold(c);
  if (c->failed || c->fb.oom)
    {
      return false;
    }

  while (c->mb.len & 3)
    {
      e16(&c->mb, 0xbf00);
    }

  c->func_off[c->mod->import_function_count + fidx] = c->mb.len;
  emit_prologue(c, push);
  place_body(c);
  c->jm->stats.wasm_bytes += f->code_size;
  c->jm->stats.functions++;
  return !c->failed && !c->mb.oom;
}

/* A function; again with br_table's entries to fragments of their own when
 * one of them was out of a TBH's reach (a function of over 128 KB)
 */

static bool compile_function(jc_t *c, uint32_t fidx)
{
  c->tbh_near = false;
  c->loc_max  = 7;
  for (; ; )
    {
      c->tbh_far  = false;
      c->regs_out = false;
      if (compile_function1(c, fidx))
        {
          return true;
        }

      if (c->fb.oom || c->mb.oom || (!c->tbh_far && !c->regs_out) ||
          (c->regs_out && c->loc_max <= 3))
        {
          return false;
        }

      /* Again: its br_table's entries to fragments of their own; fewer
       * locals in registers (more temporaries)
       */

      c->failed   = false;
      c->tbh_near = c->tbh_near || c->tbh_far;
      if (c->regs_out)
        {
          c->loc_max -= 2;
        }
    }
}

/****************************************************************************
 * The module's own code
 ****************************************************************************/

static uint32_t mb_bcc(jbuf_t *b)
{
  uint32_t pos = b->len;

  e32(b, 0, 0);
  return pos;
}

/* enter, unwind, exc, the traps */

static void emit_stubs(jc_t *c)
{
  jbuf_t *b = &c->mb;
  uint32_t enter = b->len;
  uint32_t loop;
  uint32_t done;
  uint32_t ex;
  uint32_t trap_common;
  uint32_t to_exit[3];
  uint32_t i;

  /* enter(buf, reserve, code, inst) */

  t_push(b, 0x4ff0);                          /* r4-r11, lr */
  t_sp_add(b, -4);
  t_ldst(b, LS_LDR, R_IP, R3, EXT_OFF(EXT_TRAP_SP));
  t_push(b, 1u << R0 | 1u << R_IP);
  t_mov(b, R_IP, R_SP);
  t_ldst(b, LS_STR, R_IP, R3, EXT_OFF(EXT_TRAP_SP));
  t_mov(b, R11, R3);
  reload_mem(c, b);
  t_mov(b, 4, R0);
  t_mov(b, 5, R2);
  t_shi(b, SH_LSL, 6, R1, 2);
  t_dp_reg(b, DP_SUB, 0, R_SP, R_SP, 6, SH_LSL, 0);
  t_movi(b, 7, 0);
  loop = b->len;                              /* the stack arguments */
  t_cmp(b, 7, 6);
  done = mb_bcc(b);
  t_dp_reg(b, DP_ADD, 0, R_IP, 4, 7, SH_LSL, 0);
  t_ldst(b, LS_LDR, R_IP, R_IP, 16);
  t_ldst_reg(b, LS_STR, R_IP, R_SP, 7, 0);
  t_addi(b, 7, 7, 4);
  e32(b, 0, 0);
  t_b_at(b, b->len - 4, (int32_t)loop - (int32_t)(b->len - 4), false);
  t_bcc_at(b, done, CC_HS, (int32_t)b->len - (int32_t)done);
  e32(b, 0xe890 | 4, 0x000f);                 /* ldm r4, {r0-r3} */
  t_blx(b, 5);
  t_ldst(b, LS_STR, R0, 4, 0);
  t_ldst(b, LS_STR, R1, 4, 4);
  t_movi(b, 7, 0);
  loop = b->len;                              /* the stack results */
  t_cmp(b, 7, 6);
  done = mb_bcc(b);
  t_ldst_reg(b, LS_LDR, R_IP, R_SP, 7, 0);
  t_dp_reg(b, DP_ADD, 0, 8, 4, 7, SH_LSL, 0);
  t_ldst(b, LS_STR, R_IP, 8, 16);
  t_addi(b, 7, 7, 4);
  e32(b, 0, 0);
  t_b_at(b, b->len - 4, (int32_t)loop - (int32_t)(b->len - 4), false);
  t_bcc_at(b, done, CC_HS, (int32_t)b->len - (int32_t)done);
  t_movi(b, R0, 1);

  ex = b->len;                                /* exit (r0 the status) */
  t_ldst(b, LS_LDR, R_IP, R11, EXT_OFF(EXT_TRAP_SP));
  t_mov(b, R_SP, R_IP);
  t_pop(b, 1u << R1 | 1u << R_IP);
  t_ldst(b, LS_STR, R_IP, R11, EXT_OFF(EXT_TRAP_SP));
  t_sp_add(b, 4);
  t_pop(b, 0x8ff0);                           /* r4-r11, pc */

  /* unwind(inst): from C; exc: from the code (r11 valid), the exception
   * set
   */

  c->unwind_pos = b->len;
  t_mov(b, R11, R0);
  c->exc_pos = b->len;
  t_movi(b, R0, 0);
  to_exit[0] = mb_bcc(b);
  t_b_at(b, to_exit[0], (int32_t)ex - (int32_t)to_exit[0], false);
  to_exit[1] = 0;

  /* The traps: r0 = the trap */

  trap_common = b->len;
  t_mov(b, R1, R0);
  t_mov(b, R0, R11);
  t_movi(b, R_IP, (uint32_t)(uintptr_t)jit_raise);
  t_blx(b, R_IP);
  t_movi(b, R0, 0);
  to_exit[2] = mb_bcc(b);
  t_b_at(b, to_exit[2], (int32_t)ex - (int32_t)to_exit[2], false);
  for (i = 0; i < JT_COUNT; i++)
    {
      uint32_t pos;

      c->trap_pos[i] = b->len;
      t_movi(b, R0, i);
      pos = mb_bcc(b);
      t_b_at(b, pos, (int32_t)trap_common - (int32_t)pos, false);
    }

  c->jm->enter = (jit_enter_t)(uintptr_t)enter;     /* (offsets for now) */
}

/* Whether an import can be called directly: a native function with i32
 * parameters (and an i32 result, or none) and nothing for WAMR to convert
 */

static bool import_fast(WASMFunctionImport *imp)
{
  WASMFuncType *t = imp->func_type;
  const char *sig = imp->signature;
  uint32_t i;

  if (imp->call_conv_raw || imp->call_conv_wasm_c_api ||
      imp->func_ptr_linked == NULL || imp->attachment != NULL ||
      t->result_count > 1 || t->param_count > 12)
    {
      return false;
    }

  for (i = 0; i < (uint32_t)(t->param_count + t->result_count); i++)
    {
      if (t->types[i] != VALUE_TYPE_I32)
        {
          return false;
        }
    }

  if (sig != NULL)
    {
      for (; *sig != '\0'; sig++)
        {
          if (*sig != '(' && *sig != ')' && *sig != 'i')
            {
              return false;
            }
        }
    }

  return true;
}

static void emit_import(jc_t *c, uint32_t idx)
{
  jbuf_t *b = &c->mb;
  WASMFunctionImport *imp = &c->mod->import_functions[idx].u.function;
  WASMFuncType *t = imp->func_type;
  uint32_t aw = type_words(t->types, t->param_count);
  uint32_t pos;
  uint32_t i;

  c->func_off[idx] = b->len;
  t_push(b, 1u << 4 | 1u << R_LR);

  if (import_fast(imp))
    {
      uint32_t nst = aw > 3 ? aw - 3 : 0;
      uint32_t bytes = 4 * ((nst + 1) & ~1u);

      /* Room on the native stack for the host function */

      t_ldst(b, LS_LDR, R_IP, JR_INST, EXT_OFF(EXT_LIMIT));
      t_addi(b, R_IP, R_IP, JIT_STACK_HOST - JIT_STACK_MARGIN);
      t_mov(b, 4, R_SP);
      t_cmp(b, 4, R_IP);
      pos = mb_bcc(b);
      t_bcc_at(b, pos, CC_LO, (int32_t)c->trap_pos[JT_STACK] - (int32_t)pos);

      /* (env, w0, w1, w2, stack: w3...) from (r0-r3, stack: w4...) */

      t_sp_add(b, -(int32_t)bytes);
      for (i = 0; i < nst; i++)
        {
          if (i == 0)
            {
              t_ldst(b, LS_STR, R3, R_SP, 0);
            }
          else
            {
              t_ldst(b, LS_LDR, R_IP, R_SP, bytes + 8 + 4 * (i - 1));
              t_ldst(b, LS_STR, R_IP, R_SP, 4 * i);
            }
        }

      t_mov(b, R3, R2);
      t_mov(b, R2, R1);
      t_mov(b, R1, R0);
      t_ldst(b, LS_LDR, R0, JR_INST, EXT_OFF(EXT_ENV));
      t_movi(b, R_IP, (uint32_t)(uintptr_t)imp->func_ptr_linked);
      t_blx(b, R_IP);
      t_sp_add(b, (int32_t)bytes);
      t_ldst(b, LS_LDRB, R_IP, JR_INST,
             offsetof(WASMModuleInstance, cur_exception));
      t_cmpi_enc(b, R_IP, 0);
      pos = mb_bcc(b);
      t_bcc_at(b, pos, CC_NE, (int32_t)c->exc_pos - (int32_t)pos);
      c->jm->stats.fast_imports++;
    }
  else
    {
      uint32_t nw = aw > 2 ? aw : 2;
      uint32_t bytes = (4 * nw + 7) & ~7u;

      t_sp_add(b, -(int32_t)bytes);
      for (i = 0; i < aw; i++)
        {
          if (i < 4)
            {
              t_ldst(b, LS_STR, R0 + i, R_SP, 4 * i);
            }
          else
            {
              t_ldst(b, LS_LDR, R_IP, R_SP, bytes + 8 + 4 * (i - 4));
              t_ldst(b, LS_STR, R_IP, R_SP, 4 * i);
            }
        }

      t_mov(b, R2, R_SP);
      t_movi(b, R1, idx);
      t_mov(b, R0, JR_INST);
      t_movi(b, R_IP, (uint32_t)(uintptr_t)jit_call_import);
      t_blx(b, R_IP);
      t_ldst(b, LS_LDR, R0, R_SP, 0);
      t_ldst(b, LS_LDR, R1, R_SP, 4);
      t_sp_add(b, (int32_t)bytes);
      c->jm->stats.slow_imports++;
    }

  t_pop(b, 1u << 4 | 1u << R_PC);
  while (b->len & 3)
    {
      e16(b, 0xbf00);
    }
}

/****************************************************************************
 * Entering compiled code (patch 0005's hook)
 ****************************************************************************/

static void jit_ext_call(WASMModuleInstance *inst, WASMExecEnv *env,
                         WASMFunctionInstance *f, uint32 argc, uint32 argv[])
{
  jit_module_t *jm = inst->e->ext_data;
  uint32_t idx = (uint32_t)(f - inst->e->functions);
  uint32_t argw = f->param_cell_num;
  uint32_t retw = f->ret_cell_num;
  uint32_t reserve = argw > 4 ? argw - 4 : 0;
  uint32_t save[7];
  uint8_t *bound;
  int ok;

  if (retw > 2 && retw > reserve)
    {
      reserve = retw;
    }

  reserve = (reserve + 1) & ~1u;

  {
    uint32_t buf[4 + reserve];

    memset(buf, 0, sizeof(buf));
    memcpy(buf, argv, 4 * argw);

    bound = env->user_native_stack_boundary != NULL ?
            env->user_native_stack_boundary : env->native_stack_boundary;
    memcpy(save, inst->ext_words, sizeof(save));
    inst->ext_words[EXT_LIMIT]  = bound != NULL ?
                                  (uint32_t)(uintptr_t)bound +
                                  JIT_STACK_MARGIN : 0;
    inst->ext_words[EXT_ENV]    = (uint32_t)(uintptr_t)env;
    inst->ext_words[EXT_AUXB]   = (uint32_t)env->aux_stack_boundary;
    inst->ext_words[EXT_AUXT]   = (uint32_t)env->aux_stack_bottom;
    jit_set_lim4(inst);

    ok = jm->enter(buf, reserve, jm->funcs[idx].code, inst);

    save[EXT_TRAP_SP] = inst->ext_words[EXT_TRAP_SP];
    save[EXT_FUNCS]   = inst->ext_words[EXT_FUNCS];
    save[EXT_LIM4]    = inst->ext_words[EXT_LIM4];   /* (it only grows) */
    memcpy(inst->ext_words, save, sizeof(save));
    if (ok)
      {
        memcpy(argv, retw <= 2 ? buf : buf + 4, 4 * retw);
      }
  }

  (void)argc;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool wasm_jit_supported(void)
{
  return true;
}

#ifdef CONFIG_ARCH_USE_TEXT_HEAP
extern const char *g_wamr_text_section;   /* (patch 0001) */
#endif

static void jc_free(jc_t *c)
{
  jit_free(c->mb.buf);
  jit_free(c->fb.buf);
  jit_free(c->func_off);
  jit_free(c->may_grow);
  jit_free(c->typeid);
  jit_free(c->calls);
  jit_free(c->ltype);
  jit_free(c->lkind);
  jit_free(c->lreg);
  jit_free(c->lreg2);
  jit_free(c->loff);
  jit_free(c->lweight);
  jit_free(c->lfirst);
  jit_free(c->vs);
  jit_free(c->cs);
  jit_free(c->labels);
  jit_free(c->fix);
  jit_free(c->fars);
  jit_free(c->cold);
  jit_free(c->lheads);
  jit_free(c->mloops);
}

bool wasm_jit_attach(wasm_module_inst_t module_inst, unsigned flags,
                     char *err, size_t errlen)
{
  WASMModuleInstance *inst = (WASMModuleInstance *)module_inst;
  WASMModule *mod;
  jit_module_t *jm;
  jc_t c;
  uint64_t t0 = os_time_get_boot_us();
  uint32_t nfuncs;
  uint32_t peak0;
  uint32_t i;
  uint32_t j;
  uint8_t *code;

  if (inst == NULL || inst->module_type != Wasm_Module_Bytecode)
    {
      snprintf(err, errlen, "not an interpreted module");
      return false;
    }

  if (inst->e->ext_call != NULL)
    {
      return true;
    }

  mod = inst->module;
  if (inst->memory_count > 1 || mod->memory_count + mod->import_memory_count
      > 1)
    {
      snprintf(err, errlen, "more than one memory");
      return false;
    }

  memset(&c, 0, sizeof(c));
  c.inst   = inst;
  c.mod    = mod;
  c.err    = err;
  c.errlen = errlen;
  g_jit_peak = g_jit_bytes;
  peak0 = g_jit_bytes;

  jm = jit_zalloc(sizeof(*jm));
  nfuncs = mod->import_function_count + mod->function_count;
  c.func_off = jit_zalloc((nfuncs + 1) * sizeof(uint32_t));
  c.typeid   = jit_zalloc((mod->type_count + 1) * sizeof(uint32_t));
  if (jm == NULL || c.func_off == NULL || c.typeid == NULL)
    {
      snprintf(err, errlen, "out of memory");
      jit_free(jm);
      jc_free(&c);
      return false;
    }

  c.jm = jm;
  jm->nfuncs = nfuncs;
  jm->nimports = mod->import_function_count;

  if (inst->memory_count == 1)
    {
      c.has_mem = true;
      c.mem_off = (uint32_t)((uint8_t *)inst->memories[0] - (uint8_t *)inst);
      if (inst->memories[0]->memory_data_size >= 65536)
        {
          c.guard = JIT_GUARD;      /* (the memory only grows) */
        }

#ifndef __NuttX__
      if (getenv("WASM_JIT_EXACT") != NULL)
        {
          c.guard = 0;              /* (the harness: the other way too) */
        }
#endif

      jm->guard = c.guard;
    }

  for (i = 0; i < mod->type_count; i++)
    {
      for (j = 0; j < i && mod->types[j] != mod->types[i]; j++);
      c.typeid[i] = j;
    }

  find_growers(&c);
  emit_stubs(&c);
  for (i = 0; i < mod->import_function_count && !c.failed; i++)
    {
      emit_import(&c, i);
    }

  for (i = 0; i < mod->function_count && !c.failed; i++)
    {
      compile_function(&c, i);
    }

  if (!c.failed && c.mb.oom)
    {
      snprintf(err, errlen, "out of memory");
      c.failed = true;
    }

  if (c.failed)
    {
      jit_free(jm);
      jc_free(&c);
      return false;
    }

  /* The calls between functions */

  for (i = 0; i < c.ncalls; i++)
    {
      t_b_at(&c.mb, c.calls[i].pos,
             (int32_t)c.func_off[c.calls[i].target] - (int32_t)c.calls[i].pos,
             true);
    }

#ifndef __NuttX__
  /* The harness (jitrun): WASM_JIT_DUMP=FILE writes the code and FILE.map
   * (each function's offset) for objdump
   */

  if (getenv("WASM_JIT_DUMP") != NULL)
    {
      char name[256];
      FILE *fp = fopen(getenv("WASM_JIT_DUMP"), "wb");

      if (fp != NULL)
        {
          fwrite(c.mb.buf, 1, c.mb.len, fp);
          fclose(fp);
        }

      snprintf(name, sizeof(name), "%s.map", getenv("WASM_JIT_DUMP"));
      fp = fopen(name, "w");
      if (fp != NULL)
        {
          for (i = 0; i < nfuncs; i++)
            {
              fprintf(fp, "%u %#x\n", (unsigned)i, (unsigned)c.func_off[i]);
            }

          fclose(fp);
        }
    }
#endif

  /* Into executable memory */

#ifdef CONFIG_ARCH_USE_TEXT_HEAP
  g_wamr_text_section = (flags & WASM_JIT_SRAM) ||
                        (!(flags & WASM_JIT_PSRAM) &&
                         wasm_jit_sram_fits(c.mb.len)) ?
                        ".time_critical.wasmjit" : ".text";
#endif
  code = os_mmap(NULL, c.mb.len, MMAP_PROT_READ | MMAP_PROT_WRITE |
                 MMAP_PROT_EXEC, 0, os_get_invalid_handle());
#ifdef CONFIG_ARCH_USE_TEXT_HEAP
  g_wamr_text_section = ".text";
#endif
  jm->funcs = jit_zalloc(nfuncs * sizeof(jit_fent_t) + 8);
  if (code == NULL || jm->funcs == NULL)
    {
      snprintf(err, errlen, "no memory for %u bytes of code",
               (unsigned)c.mb.len);
      if (code != NULL)
        {
          os_munmap(code, c.mb.len);
        }

      jit_free(jm->funcs);
      jit_free(jm);
      jc_free(&c);
      return false;
    }

  memcpy(code, c.mb.buf, c.mb.len);
  os_icache_flush(code, c.mb.len);
#if defined(__NuttX__)
  __asm__ __volatile__("dsb\n\tisb" ::: "memory");
#else
  __builtin___clear_cache((char *)code, (char *)code + c.mb.len);
#endif

  jm->code = code;
  jm->code_size = c.mb.len;
  jm->loops = c.mloops;                 /* (kept: wasm_jit_interrupt()) */
  jm->nloops = c.nmloops;
  c.mloops = NULL;
  jm->exc_off = c.exc_pos;
  jm->in_sram = ((uintptr_t)code >> 28) == 2;
  jm->enter  = (jit_enter_t)((uintptr_t)code + 1);     /* (at 0) */
  jm->unwind = (jit_unwind_t)((uintptr_t)code + c.unwind_pos + 1);
  for (i = 0; i < nfuncs; i++)
    {
      WASMFuncType *t = func_type_of(mod, i);

      jm->funcs[i].code = (uint32_t)(uintptr_t)code + c.func_off[i] + 1;
      for (j = 0; j < mod->type_count && mod->types[j] != (WASMType *)t;
           j++);
      jm->funcs[i].typeid = j < mod->type_count ? c.typeid[j] : 0xffffffffu;
    }

  jm->stats.code_bytes = c.mb.len;
  jm->stats.in_sram = jm->in_sram;
  jm->stats.compile_us = (uint32_t)(os_time_get_boot_us() - t0);
  jc_free(&c);
  jm->stats.peak_bytes = g_jit_peak - peak0;

  if (flags & WASM_JIT_MAP)
    {
      /* (for a profile: fpcswd.py profile's addresses to functions) */

      for (i = 0; i < nfuncs; i++)
        {
          fprintf(stderr, "%08x t wasm_f%u\n",
                  (unsigned)(jm->funcs[i].code & ~1u), (unsigned)i);
        }

      fprintf(stderr, "%08x t wasm_jit_end\n",
              (unsigned)((uintptr_t)code + c.mb.len));
    }

  inst->ext_words[EXT_FUNCS] = (uint32_t)(uintptr_t)jm->funcs;
  inst->e->ext_data = jm;
  inst->e->ext_call = jit_ext_call;

  if (flags & WASM_JIT_VERBOSE)
    {
      fprintf(stderr, "wasm: JIT: %u functions, %u bytes of wasm, %u bytes "
              "of code (%s) in %u ms (%u KB of memory at most); %u imports "
              "direct, %u generic\n",
              (unsigned)jm->stats.functions, (unsigned)jm->stats.wasm_bytes,
              (unsigned)jm->stats.code_bytes, jm->in_sram ? "SRAM" : "PSRAM",
              (unsigned)(jm->stats.compile_us / 1000),
              (unsigned)((jm->stats.peak_bytes + 1023) / 1024),
              (unsigned)jm->stats.fast_imports,
              (unsigned)jm->stats.slow_imports);
    }

  return true;
}

void wasm_jit_detach(wasm_module_inst_t module_inst)
{
  WASMModuleInstance *inst = (WASMModuleInstance *)module_inst;
  jit_module_t *jm;

  if (inst == NULL || inst->module_type != Wasm_Module_Bytecode ||
      inst->e->ext_data == NULL)
    {
      return;
    }

  jm = inst->e->ext_data;
  inst->e->ext_call = NULL;
  inst->e->ext_data = NULL;
  inst->ext_words[EXT_FUNCS] = 0;
  os_munmap(jm->code, jm->code_size);
  jit_free(jm->loops);
  jit_free(jm->funcs);
  jit_free(jm);
}

void wasm_jit_interrupt(wasm_module_inst_t module_inst)
{
  WASMModuleInstance *inst = (WASMModuleInstance *)module_inst;
  jit_module_t *jm;
  uint32_t i;

  if (inst == NULL || inst->module_type != Wasm_Module_Bytecode ||
      inst->e->ext_data == NULL)
    {
      return;
    }

  jm = inst->e->ext_data;
  for (i = 0; i < jm->nloops; i++)
    {
      uint32_t pos = jm->loops[i];
      uint8_t hw[4];
      jbuf_t tb;

      tb.buf = hw;
      tb.len = 4;
      tb.cap = 4;
      tb.oom = false;
      t_b_at(&tb, 0, (int32_t)jm->exc_off - (int32_t)pos, false);

      /* One word: the halfwords of b.w, the first at the lower address */

      *(volatile uint32_t *)(jm->code + pos) =
        hw[0] | hw[1] << 8 | hw[2] << 16 | (uint32_t)hw[3] << 24;
    }

  os_icache_flush(jm->code, jm->code_size);
#if defined(__NuttX__)
  __asm__ __volatile__("dsb\n\tisb" ::: "memory");
#else
  __builtin___clear_cache((char *)jm->code,
                          (char *)jm->code + jm->code_size);
#endif
}

bool wasm_jit_stats(wasm_module_inst_t module_inst,
                    struct wasm_jit_stats_s *st)
{
  WASMModuleInstance *inst = (WASMModuleInstance *)module_inst;
  jit_module_t *jm;

  if (inst == NULL || inst->module_type != Wasm_Module_Bytecode ||
      inst->e->ext_data == NULL)
    {
      return false;
    }

  jm = inst->e->ext_data;
  *st = jm->stats;
  return true;
}

#endif /* JIT_HAVE */

bool wasm_jit_sram_fits(uint32_t size)
{
#if defined(__NuttX__) && defined(CONFIG_ARCH_USE_TEXT_HEAP) && \
    defined(CONFIG_MM_KERNEL_HEAP)
  struct mallinfo mi = kmm_mallinfo();     /* (the kernel heap: the SRAM) */

  return size <= WASM_SRAM_CODE_MAX &&
         (uint32_t)mi.mxordblk >= size + WASM_SRAM_FLOOR;
#else
  (void)size;
  return false;
#endif
}

#if !JIT_HAVE

/* No compiler for this target (the simulator): everything interpreted */

bool wasm_jit_supported(void)
{
  return false;
}

bool wasm_jit_attach(wasm_module_inst_t inst, unsigned flags, char *err,
                     size_t errlen)
{
  snprintf(err, errlen, "no JIT for this CPU");
  return false;
}

void wasm_jit_detach(wasm_module_inst_t inst)
{
}

void wasm_jit_interrupt(wasm_module_inst_t inst)
{
}

bool wasm_jit_stats(wasm_module_inst_t inst, struct wasm_jit_stats_s *st)
{
  return false;
}

#endif
