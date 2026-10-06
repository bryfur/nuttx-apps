/****************************************************************************
 * apps/graphics/fpcwasm/jit/test/jitrun.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The JIT's test harness (a Linux ARM program, run under qemu-arm):
 *
 *   jitrun spec FILE.cmds          spec test commands (spec.py): every
 *                                  module instantiated twice, interpreted
 *                                  and compiled; every command run on both,
 *                                  checked against the expected result and
 *                                  each other
 *   jitrun fuzz FILE.wasm SEED     every exported function called with
 *                                  random arguments, interpreted and
 *                                  compiled: results, traps, memory, globals
 *                                  compared
 *   jitrun run [-i] [-t N] [-k MS] FILE.wasm [ARGS]   a WASI command (compiled
 *                                  unless -i), its time
 ****************************************************************************/

#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wasm_export.h"
#include "wasm_jit.h"

#define STACK_SIZE (512 * 1024)      /* The wasm stack (interpreter) */

/****************************************************************************
 * Instances
 ****************************************************************************/

typedef struct inst_s
{
  wasm_module_inst_t inst;
  wasm_exec_env_t env;
  bool jit;
} inst_t;

static uint8_t *g_buf;
static wasm_module_t g_module;
static inst_t g_in[2];                  /* 0 interpreted, 1 compiled */
static bool g_have;
static char g_jit_err[160];
static int g_verbose;

static uint8_t *read_file(const char *path, uint32_t *size)
{
  FILE *f = fopen(path, "rb");
  uint8_t *b;
  long n;

  if (f == NULL)
    {
      return NULL;
    }

  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  b = malloc(n > 0 ? n : 1);
  if (fread(b, 1, n, f) != (size_t)n)
    {
      free(b);
      fclose(f);
      return NULL;
    }

  fclose(f);
  *size = (uint32_t)n;
  return b;
}

static void bound_stack(wasm_exec_env_t env)
{
  /* The native stack (the JIT's frames): 1 MB from here, so that neither
   * mode runs out first at the depths the tests reach on purpose
   */

  char here;

  wasm_runtime_set_native_stack_boundary(env,
                                         (uint8_t *)((uintptr_t)&here -
                                                     1024 * 1024));
}

static void drop_module(void)
{
  int i;

  for (i = 0; i < 2; i++)
    {
      if (g_in[i].env != NULL)
        {
          wasm_runtime_destroy_exec_env(g_in[i].env);
        }

      if (g_in[i].inst != NULL)
        {
          wasm_jit_detach(g_in[i].inst);
          wasm_runtime_deinstantiate(g_in[i].inst);
        }

      memset(&g_in[i], 0, sizeof(g_in[i]));
    }

  if (g_module != NULL)
    {
      wasm_runtime_unload(g_module);
      g_module = NULL;
    }

  free(g_buf);
  g_buf = NULL;
  g_have = false;
}

/* Load and instantiate twice; false if the module does not load (not
 * this runtime's) or the JIT refuses it (*jit_ok false then)
 */

static bool load_module(const char *path, bool *jit_ok, char *why,
                        size_t whylen)
{
  char err[160];
  uint32_t size;
  int i;

  drop_module();
  *jit_ok = false;
  g_buf = read_file(path, &size);
  if (g_buf == NULL)
    {
      snprintf(why, whylen, "cannot read %s", path);
      return false;
    }

  g_module = wasm_runtime_load(g_buf, size, err, sizeof(err));
  if (g_module == NULL)
    {
      snprintf(why, whylen, "load: %s", err);
      return false;
    }

  for (i = 0; i < 2; i++)
    {
      InstantiationArgs ia;

      /* Linear memory at most 64 MB (two instances in a 32-bit process) */

      memset(&ia, 0, sizeof(ia));
      ia.default_stack_size = STACK_SIZE;
      ia.max_memory_pages = 1024;
      g_in[i].inst = wasm_runtime_instantiate_ex(g_module, &ia, err,
                                                 sizeof(err));
      if (g_in[i].inst == NULL)
        {
          snprintf(why, whylen, "instantiate: %s", err);
          drop_module();
          return false;
        }

      g_in[i].env = wasm_runtime_create_exec_env(g_in[i].inst, STACK_SIZE);
      bound_stack(g_in[i].env);
    }

  g_jit_err[0] = '\0';
  *jit_ok = wasm_jit_attach(g_in[1].inst, 0, g_jit_err, sizeof(g_jit_err));
  g_in[1].jit = *jit_ok;
  g_have = true;
  return true;
}

/****************************************************************************
 * Values
 ****************************************************************************/

typedef struct val_s
{
  uint8_t kind;                         /* WASM_I32 ... WASM_FUNCREF */
  uint8_t nan;                          /* expected: 1 canonical, 2 arith */
  bool any;                             /* expected: any non-null ref */
  bool null;
  uint64_t bits;
} val_t;

static bool parse_val(const char *s, val_t *v)
{
  memset(v, 0, sizeof(*v));
  if (strncmp(s, "i32:", 4) == 0)
    {
      v->kind = WASM_I32;
    }
  else if (strncmp(s, "i64:", 4) == 0)
    {
      v->kind = WASM_I64;
    }
  else if (strncmp(s, "f32:", 4) == 0)
    {
      v->kind = WASM_F32;
    }
  else if (strncmp(s, "f64:", 4) == 0)
    {
      v->kind = WASM_F64;
    }
  else if (strncmp(s, "funcref:", 8) == 0)
    {
      v->kind = WASM_FUNCREF;
      s += 4;
    }
  else if (strncmp(s, "externref:", 10) == 0)
    {
      v->kind = WASM_EXTERNREF;
      s += 6;
    }
  else
    {
      return false;
    }

  s += 4;
  if (strcmp(s, "nan:canonical") == 0)
    {
      v->nan = 1;
    }
  else if (strcmp(s, "nan:arithmetic") == 0)
    {
      v->nan = 2;
    }
  else if (strcmp(s, "null") == 0)
    {
      v->null = true;
    }
  else if (strcmp(s, "any") == 0)
    {
      v->any = true;
    }
  else
    {
      v->bits = strtoull(s, NULL, 0);
    }

  return true;
}

static int cells_of(uint8_t kind)
{
  return kind == WASM_I64 || kind == WASM_F64 ? 2 : 1;
}

/* Run FUNC on instance i: results into res; the exception or NULL */

static const char *invoke(int i, const char *func, val_t *args, int nargs,
                          val_t *res, int *nres)
{
  wasm_module_inst_t inst = g_in[i].inst;
  wasm_function_inst_t f = wasm_runtime_lookup_function(inst, func);
  wasm_valkind_t pk[64];
  wasm_valkind_t rk[64];
  uint32_t argv[128];
  uint32_t np;
  uint32_t nr;
  uint32_t n = 0;
  uint32_t k;
  static char ex[160];

  if (f == NULL)
    {
      snprintf(ex, sizeof(ex), "no function %s", func);
      return ex;
    }

  np = wasm_func_get_param_count(f, inst);
  nr = wasm_func_get_result_count(f, inst);
  if ((int)np != nargs || np > 64 || nr > 64)
    {
      snprintf(ex, sizeof(ex), "bad argument count for %s", func);
      return ex;
    }

  wasm_func_get_param_types(f, inst, pk);
  wasm_func_get_result_types(f, inst, rk);
  memset(argv, 0, sizeof(argv));
  for (k = 0; k < np; k++)
    {
      if (pk[k] == WASM_EXTERNREF)
        {
          uint32_t ref = 0xffffffffu;

          if (!args[k].null)
            {
              wasm_externref_obj2ref(inst,
                                     (void *)(uintptr_t)(args[k].bits + 1),
                                     &ref);
            }

          argv[n++] = ref;
        }
      else if (pk[k] == WASM_FUNCREF)
        {
          argv[n++] = args[k].null ? 0xffffffffu : (uint32_t)args[k].bits;
        }
      else if (cells_of(pk[k]) == 2)
        {
          argv[n++] = (uint32_t)args[k].bits;
          argv[n++] = (uint32_t)(args[k].bits >> 32);
        }
      else
        {
          argv[n++] = (uint32_t)args[k].bits;
        }
    }

  if (!wasm_runtime_call_wasm(g_in[i].env, f, n, argv))
    {
      const char *e = wasm_runtime_get_exception(inst);

      snprintf(ex, sizeof(ex), "%s", e != NULL ? e : "(failed)");
      wasm_runtime_clear_exception(inst);
      return ex;
    }

  n = 0;
  for (k = 0; k < nr; k++)
    {
      memset(&res[k], 0, sizeof(res[k]));
      res[k].kind = rk[k];
      if (rk[k] == WASM_EXTERNREF)
        {
          void *obj = NULL;

          if (argv[n] == 0xffffffffu)
            {
              res[k].null = true;
            }
          else if (wasm_externref_ref2obj(argv[n], &obj))
            {
              res[k].bits = (uint64_t)(uintptr_t)obj - 1;
            }

          n++;
        }
      else if (rk[k] == WASM_FUNCREF)
        {
          res[k].null = argv[n] == 0xffffffffu;
          res[k].bits = argv[n++];
        }
      else if (cells_of(rk[k]) == 2)
        {
          res[k].bits = argv[n] | (uint64_t)argv[n + 1] << 32;
          n += 2;
        }
      else
        {
          res[k].bits = argv[n++];
        }
    }

  *nres = (int)nr;
  return NULL;
}

/* Whether an actual value is what was expected */

static bool val_ok(const val_t *got, const val_t *want)
{
  if (want->kind == WASM_FUNCREF || want->kind == WASM_EXTERNREF)
    {
      if (want->null)
        {
          return got->null;
        }

      if (want->any)
        {
          return !got->null;
        }

      return !got->null && got->bits == want->bits;
    }

  if (want->nan != 0)
    {
      if (want->kind == WASM_F32)
        {
          uint32_t b = (uint32_t)got->bits;

          return want->nan == 1 ? (b & 0x7fffffffu) == 0x7fc00000u :
                 (b & 0x7fc00000u) == 0x7fc00000u;
        }

      return want->nan == 1 ?
             (got->bits & 0x7fffffffffffffffull) == 0x7ff8000000000000ull :
             (got->bits & 0x7ff8000000000000ull) == 0x7ff8000000000000ull;
    }

  if (want->kind == WASM_I32 || want->kind == WASM_F32)
    {
      return (uint32_t)got->bits == (uint32_t)want->bits;
    }

  return got->bits == want->bits;
}

static void print_vals(const char *tag, val_t *v, int n)
{
  int i;

  fprintf(stderr, "    %s:", tag);
  for (i = 0; i < n; i++)
    {
      fprintf(stderr, " %d:%#" PRIx64 "%s", v[i].kind, v[i].bits,
              v[i].null ? "(null)" : "");
    }

  fprintf(stderr, "\n");
}

/****************************************************************************
 * spec
 ****************************************************************************/

static int hexval(int c)
{
  return isdigit(c) ? c - '0' : tolower(c) - 'a' + 10;
}

static void unhex(const char *h, char *out, size_t len)
{
  size_t i = 0;

  while (h[0] && h[1] && i + 1 < len)
    {
      out[i++] = (char)(hexval(h[0]) << 4 | hexval(h[1]));
      h += 2;
    }

  out[i] = '\0';
}

static char *tok(char **pp)
{
  char *p = *pp;
  char *s;

  while (*p == ' ')
    {
      p++;
    }

  if (*p == '\0' || *p == '\n')
    {
      return NULL;
    }

  s = p;
  while (*p != '\0' && *p != ' ' && *p != '\n')
    {
      p++;
    }

  if (*p != '\0')
    {
      *p++ = '\0';
    }

  *pp = p;
  return s;
}

/* Trap messages that may differ: running out of stack */

static bool same_trap(const char *a, const char *b)
{
  if (strcmp(a, b) == 0)
    {
      return true;
    }

  return (strstr(a, "stack overflow") != NULL || strstr(a, "stack exhaust")
          != NULL) && (strstr(b, "stack overflow") != NULL ||
          strstr(b, "stack exhaust") != NULL);
}

static int cmd_spec(const char *path)
{
  FILE *f = fopen(path, "r");
  char line[16384];
  int lineno = 0;
  int ncmd = 0;
  int nmod = 0;
  int nskipmod = 0;
  int nrefused = 0;
  int fail[2] = { 0, 0 };
  int jit_only = 0;
  int mismatch = 0;
  bool skipping = true;
  bool jit_ok = false;
  char modpath[512] = "";

  if (f == NULL)
    {
      fprintf(stderr, "jitrun: %s: cannot open\n", path);
      return 2;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      char *p = line;
      char *cmd = tok(&p);
      char name[1024];
      val_t args[64];
      val_t want[64];
      val_t got[2][64];
      const char *ex[2];
      int nargs;
      int nwant = 0;
      int ngot[2] = { 0, 0 };
      char *t;
      char *msg = NULL;
      int i;
      int k;

      lineno++;
      if (cmd == NULL)
        {
          continue;
        }

      if (strcmp(cmd, "module") == 0)
        {
          char why[256];

          t = tok(&p);
          snprintf(modpath, sizeof(modpath), "%s", t);
          nmod++;
          skipping = !load_module(t, &jit_ok, why, sizeof(why));
          if (skipping)
            {
              nskipmod++;
              if (g_verbose)
                {
                  fprintf(stderr, "%s:%d: module skipped: %s\n", path, lineno,
                          why);
                }
            }
          else if (!jit_ok)
            {
              nrefused++;
              fprintf(stderr, "%s:%d: JIT refused %s: %s\n", path, lineno,
                      t, g_jit_err);
            }

          continue;
        }

      if (skipping)
        {
          continue;
        }

      if (strcmp(cmd, "get") == 0)
        {
          wasm_global_inst_t gi[2];
          val_t w;

          unhex(tok(&p), name, sizeof(name));
          tok(&p);
          parse_val(tok(&p), &w);
          ncmd++;
          for (i = 0; i < 2; i++)
            {
              val_t g;

              memset(&g, 0, sizeof(g));
              if (!wasm_runtime_get_export_global_inst(g_in[i].inst, name,
                                                       &gi[i]))
                {
                  fail[i]++;
                  continue;
                }

              memcpy(&g.bits, gi[i].global_data, cells_of(gi[i].kind) * 4);
              g.kind = gi[i].kind;
              if (!val_ok(&g, &w))
                {
                  fail[i]++;
                  fprintf(stderr, "%s:%d: %s: global %s wrong\n", path,
                          lineno, i ? "JIT" : "interp", name);
                }
            }

          continue;
        }

      if (strcmp(cmd, "assert_return") != 0 &&
          strcmp(cmd, "assert_trap") != 0 &&
          strcmp(cmd, "assert_exhaustion") != 0 &&
          strcmp(cmd, "invoke") != 0)
        {
          continue;
        }

      unhex(tok(&p), name, sizeof(name));
      nargs = atoi(tok(&p));
      for (k = 0; k < nargs; k++)
        {
          parse_val(tok(&p), &args[k]);
        }

      if (strcmp(cmd, "assert_return") == 0)
        {
          nwant = atoi(tok(&p));
          for (k = 0; k < nwant; k++)
            {
              parse_val(tok(&p), &want[k]);
            }
        }
      else if (strcmp(cmd, "assert_trap") == 0)
        {
          msg = p;
          while (*msg == ' ')
            {
              msg++;
            }

          msg[strcspn(msg, "\n")] = '\0';
        }

      ncmd++;
      for (i = 0; i < 2; i++)
        {
          ex[i] = invoke(i, name, args, nargs, got[i], &ngot[i]);
          if (ex[i] != NULL)
            {
              ex[i] = strdup(ex[i]);
            }
        }

      for (i = 0; i < 2; i++)
        {
          static bool ok0;
          bool ok = true;

          if (strcmp(cmd, "assert_return") == 0)
            {
              ok = ex[i] == NULL && ngot[i] == nwant;
              for (k = 0; ok && k < nwant; k++)
                {
                  ok = val_ok(&got[i][k], &want[k]);
                }
            }
          else if (strcmp(cmd, "assert_trap") == 0)
            {
              ok = ex[i] != NULL && strstr(ex[i], msg) != NULL;
            }
          else if (strcmp(cmd, "assert_exhaustion") == 0)
            {
              ok = ex[i] != NULL && strstr(ex[i], "stack") != NULL;
            }

          if (i == 0)
            {
              ok0 = ok;
            }
          else if (!ok && ok0)
            {
              jit_only++;
            }

          if (!ok)
            {
              fail[i]++;
              fprintf(stderr, "%s:%d: %s: %s %s: %s%s%s\n", path, lineno,
                      i ? "JIT" : "interp", cmd, name,
                      ex[i] ? "trap: " : "", ex[i] ? ex[i] : "",
                      msg ? " (want trap)" : "");
              if (ex[i] == NULL && nwant > 0)
                {
                  print_vals("got ", got[i], ngot[i]);
                  print_vals("want", want, nwant);
                }
            }
        }

      /* The two modes against each other */

      if ((ex[0] == NULL) != (ex[1] == NULL) ||
          (ex[0] != NULL && !same_trap(ex[0], ex[1])))
        {
          mismatch++;
          fprintf(stderr, "%s:%d: MISMATCH %s: interp %s, JIT %s\n", path,
                  lineno, name, ex[0] ? ex[0] : "ok", ex[1] ? ex[1] : "ok");
        }
      else if (ex[0] == NULL)
        {
          for (k = 0; k < ngot[0]; k++)
            {
              bool nan0 = (got[0][k].kind == WASM_F32 &&
                           ((uint32_t)got[0][k].bits & 0x7fffffffu) >
                           0x7f800000u) ||
                          (got[0][k].kind == WASM_F64 &&
                           (got[0][k].bits & 0x7fffffffffffffffull) >
                           0x7ff0000000000000ull);

              if (got[0][k].bits != got[1][k].bits && !nan0 &&
                  got[0][k].kind != WASM_FUNCREF)
                {
                  mismatch++;
                  fprintf(stderr, "%s:%d: MISMATCH %s result %d\n", path,
                          lineno, name, k);
                  print_vals("interp", got[0], ngot[0]);
                  print_vals("JIT   ", got[1], ngot[1]);
                  break;
                }
            }
        }

      free((void *)ex[0]);
      free((void *)ex[1]);
    }

  fclose(f);
  drop_module();
  printf("%s: %d modules (%d skipped, %d refused by the JIT), %d commands; "
         "failed: interp %d, JIT %d (JIT only %d); mismatches %d\n", path,
         nmod, nskipmod, nrefused, ncmd, fail[0], fail[1], jit_only,
         mismatch);
  return jit_only != 0 || mismatch != 0 || nrefused != 0;
}

/****************************************************************************
 * fuzz
 ****************************************************************************/

static uint64_t g_rng;

static uint64_t rnd(void)
{
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return g_rng;
}

static uint64_t rnd_val(uint8_t kind)
{
  static const uint64_t special[] =
  {
    0, 1, 2, 0xffffffffu, 0x80000000u, 0x7fffffffu, 0xffffffffffffffffull,
    0x8000000000000000ull, 31, 32, 63, 64, 0x3f800000u, 0x7fc00000u,
    0x3ff0000000000000ull, 0x7ff8000000000000ull, 0xff800000u, 65536, 100
  };

  uint64_t r = rnd();

  if ((r & 3) == 0)
    {
      return special[(r >> 8) % (sizeof(special) / sizeof(special[0]))];
    }

  if ((r & 3) == 1)
    {
      return (r >> 16) & 0xff;
    }

  return kind == WASM_I32 || kind == WASM_F32 ? (uint32_t)(r >> 11) : r;
}

static uint32_t mem_hash(wasm_module_inst_t inst, uint32_t *pages)
{
  wasm_memory_inst_t m = wasm_runtime_get_default_memory(inst);
  uint32_t h = 2166136261u;
  uint8_t *b;
  uint64_t n;
  uint64_t i;

  *pages = 0;
  if (m == NULL)
    {
      return 0;
    }

  *pages = (uint32_t)wasm_memory_get_cur_page_count(m);
  n = (uint64_t)*pages * wasm_memory_get_bytes_per_page(m);
  b = wasm_memory_get_base_address(m);
  for (i = 0; i < n; i++)
    {
      h = (h ^ b[i]) * 16777619u;
    }

  return h;
}

static int cmd_fuzz(const char *path, uint64_t seed)
{
  char why[256];
  bool jit_ok;
  int32_t n;
  int32_t i;
  int calls = 0;
  int traps = 0;
  int bad = 0;
  int round;

  if (!load_module(path, &jit_ok, why, sizeof(why)))
    {
      printf("%s: skipped: %s\n", path, why);
      return 0;
    }

  if (!jit_ok)
    {
      printf("%s: JIT refused: %s\n", path, g_jit_err);
      drop_module();
      return strstr(g_jit_err, "not supported") ? 0 : 1;
    }

  g_rng = seed * 2654435761u + 1;
  n = wasm_runtime_get_export_count(g_module);
  for (round = 0; round < 3; round++)
    {
      for (i = 0; i < n; i++)
        {
          wasm_export_t e;
          wasm_function_inst_t f;
          wasm_valkind_t pk[64];
          val_t args[64];
          val_t got[2][64];
          int ngot[2];
          const char *ex[2];
          uint32_t np;
          uint32_t k;
          uint32_t h[2];
          uint32_t pg[2];
          int j;

          wasm_runtime_get_export_type(g_module, i, &e);
          if (e.kind != WASM_IMPORT_EXPORT_KIND_FUNC)
            {
              continue;
            }

          f = wasm_runtime_lookup_function(g_in[0].inst, e.name);
          if (f == NULL)
            {
              continue;
            }

          np = wasm_func_get_param_count(f, g_in[0].inst);
          if (np > 64)
            {
              continue;
            }

          wasm_func_get_param_types(f, g_in[0].inst, pk);
          for (k = 0; k < np; k++)
            {
              memset(&args[k], 0, sizeof(args[k]));
              args[k].kind = pk[k];
              args[k].bits = rnd_val(pk[k]);
              if (pk[k] == WASM_EXTERNREF || pk[k] == WASM_FUNCREF)
                {
                  args[k].null = true;
                }
            }

          for (j = 0; j < 2; j++)
            {
              ex[j] = invoke(j, e.name, args, (int)np, got[j], &ngot[j]);
              ex[j] = ex[j] != NULL ? strdup(ex[j]) : NULL;
              h[j] = mem_hash(g_in[j].inst, &pg[j]);
            }

          calls++;
          traps += ex[0] != NULL;
          if ((ex[0] == NULL) != (ex[1] == NULL) ||
              (ex[0] != NULL && !same_trap(ex[0], ex[1])))
            {
              bad++;
              printf("%s: %s: interp %s, JIT %s\n", path, e.name,
                     ex[0] ? ex[0] : "ok", ex[1] ? ex[1] : "ok");
            }
          else if (ex[0] == NULL)
            {
              for (k = 0; k < (uint32_t)ngot[0]; k++)
                {
                  bool nan0 = (got[0][k].kind == WASM_F32 &&
                               ((uint32_t)got[0][k].bits & 0x7fffffffu) >
                               0x7f800000u) ||
                              (got[0][k].kind == WASM_F64 &&
                               (got[0][k].bits & 0x7fffffffffffffffull) >
                               0x7ff0000000000000ull);

                  if (got[0][k].bits != got[1][k].bits && !nan0)
                    {
                      bad++;
                      printf("%s: %s: result %u: interp %#" PRIx64
                             ", JIT %#" PRIx64 "\n", path, e.name,
                             (unsigned)k, got[0][k].bits, got[1][k].bits);
                      break;
                    }
                }
            }

          if (h[0] != h[1] || pg[0] != pg[1])
            {
              bad++;
              printf("%s: %s: memory differs after the call\n", path,
                     e.name);
            }

          free((void *)ex[0]);
          free((void *)ex[1]);
          if (bad > 0)
            {
              goto done;
            }
        }
    }

done:
  printf("%s: %d calls (%d traps), %d differences\n", path, calls, traps,
         bad);
  drop_module();
  return bad != 0;
}

/****************************************************************************
 * run
 ****************************************************************************/

/* -k: terminated after MS ms, as the board's watchdog does */

struct killer_s
{
  wasm_module_inst_t inst;
  int ms;
};

static void *killer(void *arg)
{
  struct killer_s *k = arg;

  usleep(k->ms * 1000);
  wasm_runtime_terminate(k->inst);
  wasm_jit_interrupt(k->inst);
  return NULL;
}

static int cmd_run(int argc, char **argv)
{
  bool interp = false;
  int times = 1;
  int kill_ms = 0;
  pthread_t kt;
  struct killer_s ks;
  char err[160];
  const char *dirs[1] = { "." };
  uint32_t size;
  uint8_t *buf;
  wasm_module_t m;
  wasm_module_inst_t inst;
  wasm_exec_env_t env;
  wasm_function_inst_t start;
  struct timespec t0;
  struct timespec t1;
  int i = 0;
  int status = 0;
  int r;

  while (i < argc && argv[i][0] == '-')
    {
      if (strcmp(argv[i], "-i") == 0)
        {
          interp = true;
        }
      else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
        {
          times = atoi(argv[++i]);
        }
      else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc)
        {
          kill_ms = atoi(argv[++i]);
        }

      i++;
    }

  if (i >= argc)
    {
      return 2;
    }

  buf = read_file(argv[i], &size);
  if (buf == NULL)
    {
      fprintf(stderr, "jitrun: %s: cannot read\n", argv[i]);
      return 2;
    }

  for (r = 0; r < times; r++)
    {
      m = wasm_runtime_load(buf, size, err, sizeof(err));
      if (m == NULL)
        {
          fprintf(stderr, "jitrun: %s\n", err);
          return 2;
        }

      wasm_runtime_set_wasi_args(m, dirs, 1, NULL, 0, NULL, 0, argv + i,
                                 argc - i);
      inst = wasm_runtime_instantiate(m, STACK_SIZE, 0, err, sizeof(err));
      if (inst == NULL)
        {
          fprintf(stderr, "jitrun: %s\n", err);
          return 2;
        }

      clock_gettime(CLOCK_MONOTONIC, &t0);
      if (!interp && !wasm_jit_attach(inst, WASM_JIT_VERBOSE |
                                      (getenv("JITRUN_MAP") != NULL ?
                                       WASM_JIT_MAP : 0), err, sizeof(err)))
        {
          fprintf(stderr, "jitrun: JIT: %s\n", err);
        }

      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (!interp)
        {
          fprintf(stderr, "jitrun: compiled in %.1f ms\n",
                  (t1.tv_sec - t0.tv_sec) * 1e3 +
                  (t1.tv_nsec - t0.tv_nsec) / 1e6);
        }

      env = wasm_runtime_create_exec_env(inst, STACK_SIZE);
      bound_stack(env);
      start = wasm_runtime_lookup_function(inst, "_start");
      clock_gettime(CLOCK_MONOTONIC, &t0);
      if (kill_ms > 0)
        {
          ks.inst = inst;
          ks.ms = kill_ms;
          pthread_create(&kt, NULL, killer, &ks);
        }

      if (start == NULL || !wasm_runtime_call_wasm(env, start, 0, NULL))
        {
          const char *e = wasm_runtime_get_exception(inst);

          if (e != NULL && strstr(e, "wasi proc exit") != NULL)
            {
              status = (int)wasm_runtime_get_wasi_exit_code(inst);
            }
          else
            {
              fprintf(stderr, "jitrun: %s\n", e ? e : "no _start");
              status = 1;
            }
        }
      else
        {
          status = (int)wasm_runtime_get_wasi_exit_code(inst);
        }

      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (kill_ms > 0)
        {
          pthread_join(kt, NULL);
        }

      fprintf(stderr, "jitrun: %s: status %d, %.1f ms\n",
              interp ? "interpreted" : "JIT", status,
              (t1.tv_sec - t0.tv_sec) * 1e3 +
              (t1.tv_nsec - t0.tv_nsec) / 1e6);
      wasm_runtime_destroy_exec_env(env);
      wasm_jit_detach(inst);
      wasm_runtime_deinstantiate(inst);
      wasm_runtime_unload(m);
    }

  free(buf);
  return status;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char **argv)
{
  RuntimeInitArgs args;
  int ret = 2;

  memset(&args, 0, sizeof(args));
  args.mem_alloc_type = Alloc_With_System_Allocator;
  args.max_thread_num = 4;
  if (!wasm_runtime_full_init(&args))
    {
      fprintf(stderr, "jitrun: the runtime did not start\n");
      return 2;
    }

  wasm_runtime_set_log_level(WASM_LOG_LEVEL_ERROR);
  if (getenv("JITRUN_VERBOSE") != NULL)
    {
      g_verbose = 1;
    }

  if (argc >= 3 && strcmp(argv[1], "spec") == 0)
    {
      ret = cmd_spec(argv[2]);
    }
  else if (argc >= 4 && strcmp(argv[1], "fuzz") == 0)
    {
      ret = cmd_fuzz(argv[2], strtoull(argv[3], NULL, 0));
    }
  else if (argc >= 3 && strcmp(argv[1], "run") == 0)
    {
      ret = cmd_run(argc - 2, argv + 2);
    }
  else
    {
      fprintf(stderr, "usage: jitrun spec FILE.cmds | fuzz FILE.wasm SEED | "
              "run [-i] [-t N] [-k MS] FILE.wasm [ARGS]\n");
    }

  wasm_runtime_destroy();
  return ret;
}
