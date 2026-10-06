/****************************************************************************
 * apps/devtools/xcc/xcc_main.c
 *
 * SPDX-License-Identifier: MIT
 *
 * xcc: C to native NuttX modules for the board's Cortex-M33 (Thumb-2,
 * hard float), with xcc's own preprocessor, compiler (the ARM32 backend),
 * assembler and linker, all four in this program and run in this process
 * one after the other (xcc_run): no fork, no exec, the intermediate files
 * in memory.  The headers are NuttX's own (those the firmware was built
 * with) under CONFIG_DEVTOOLS_XCC_ROOT/arm; what a module calls in the
 * firmware is resolved by NuttX's ELF loader against its symbol table.
 *
 *   xcc [-c|-S|-E] [-o OUT] [-I DIR] [-D NAME[=V]] [-U NAME] [-O...] [-W...]
 *       [-L DIR] [-lNAME] [--stack=BYTES] [--priority=N] [-v] FILE...
 *
 ****************************************************************************/

#include "xcc_nuttx.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int xcc_cpp_main(int argc, char *argv[]);
int xcc_cc1_main(int argc, char *argv[]);
int xcc_as_main(int argc, char *argv[]);
int xcc_ld_main(int argc, char *argv[]);

XCC_BLOB(cpp);
XCC_BLOB(cc1);
XCC_BLOB(as);
XCC_BLOB(ld);

/* This driver is a program of its own too (xccd: xcc_entry.c runs it) */

#define MAX_ARGS 128

struct args_s
{
  int argc;
  char *argv[MAX_ARGS + 1];
};

static void arg(struct args_s *a, const char *s)
{
  if (a->argc < MAX_ARGS)
    {
      a->argv[a->argc++] = (char *)s;
      a->argv[a->argc] = NULL;
    }
}

static const char *kPredefined[] =
{
  "-D__XCC", "-D__ILP32__", "-D__arm__", "-D__thumb__", "-D__thumb2__",
  "-D__ARMEL__", "-D__ARM_EABI__", "-D__ARM_ARCH=8", "-D__ARM_ARCH_8M_MAIN__",
  "-D__ARM_PCS_VFP", "-D__VFP_FP__", "-D__ARM_FP=4", "-D__SOFTFP_DOUBLE__", "-D__CHAR_UNSIGNED__",
  "-D__STDC__", "-D__STDC_VERSION__=199901L",
  "-D__SIZEOF_POINTER__=4", "-D__SIZEOF_INT__=4", "-D__SIZEOF_LONG__=4",
  "-D__SIZEOF_LONG_LONG__=8", "-D__SIZEOF_SIZE_T__=4",
  "-D__NuttX__", "-D__asm__=__asm", "-D__volatile__=volatile",
  "-D__inline__=inline", "-D__restrict=restrict", "-D__restrict__=restrict",
};

enum stage_e
{
  STAGE_PP, STAGE_ASM, STAGE_OBJ, STAGE_LINK,
};

static bool g_verbose;

static int run(struct xcc_blob_s *blob, int (*mainfn)(int, char **), struct args_s *a)
{
  if (g_verbose)
    {
      for (int i = 0; i < a->argc; ++i)
        fprintf(stderr, "%s%s", i ? " " : "", a->argv[i]);
      fprintf(stderr, "\n");
    }

  return xcc_run(blob, mainfn, a->argc, a->argv);
}

static char *path_join(const char *a, const char *b)
{
  char *p = malloc(strlen(a) + strlen(b) + 2);
  sprintf(p, "%s/%s", a, b);
  return p;
}

static const char *ext_of(const char *path)
{
  const char *dot = strrchr(path, '.');
  const char *slash = strrchr(path, '/');
  return dot != NULL && (slash == NULL || dot > slash) ? dot + 1 : "";
}

static char *change_ext(const char *path, const char *ext)
{
  const char *slash = strrchr(path, '/');
  const char *base = slash != NULL ? slash + 1 : path;
  const char *dot = strrchr(base, '.');
  size_t n = dot != NULL ? (size_t)(dot - base) : strlen(base);
  char *p = malloc(n + strlen(ext) + 2);
  memcpy(p, base, n);
  sprintf(p + n, ".%s", ext);
  return p;
}

/* A name for an intermediate file, made a memory file */

static char *tmpname(const char *ext)
{
  static int no;
  char buf[32];
  snprintf(buf, sizeof(buf), "/tmp/xcc-%d.%s", ++no, ext);
  FILE *fp = xcc_memfile(buf);
  if (fp != NULL)
    fclose(fp);
  return strdup(buf);
}

static void usage(FILE *fp)
{
  fprintf(fp,
    "Usage: xcc [options] file...\n"
    "  -c / -S / -E       object / assembly / preprocessed only\n"
    "  -o FILE            the output (default a.out: a NuttX module)\n"
    "  -I DIR, -D NAME[=VALUE], -U NAME\n"
    "  -L DIR, -lNAME     libraries (libNAME.a)\n"
    "  --stack=BYTES      the module's stack (default 8192)\n"
    "  --priority=N       its priority\n"
    "  -v                 the steps\n");
}

int main(int argc, char *argv[])
{
  const char *root = getenv("XCC_ROOT");
  if (root == NULL)
    root = CONFIG_DEVTOOLS_XCC_ROOT;
  char *armroot = path_join(root, "arm");

  enum stage_e stage = STAGE_LINK;
  const char *ofn = NULL;
  const char *stack = "--stack=8192";
  const char *priority = NULL;
  bool nostdlib = false;
  bool nostdinc = false;
  struct args_s cppopts = {0}, ccopts = {0}, ldopts = {0}, sources = {0};

  for (int i = 1; i < argc; ++i)
    {
      const char *a = argv[i];
      const char *next = i + 1 < argc ? argv[i + 1] : NULL;
      if (strcmp(a, "-c") == 0)
        stage = STAGE_OBJ;
      else if (strcmp(a, "-S") == 0)
        stage = STAGE_ASM;
      else if (strcmp(a, "-E") == 0)
        stage = STAGE_PP;
      else if (strcmp(a, "-o") == 0 && next != NULL)
        ofn = argv[++i];
      else if (strncmp(a, "-o", 2) == 0)
        ofn = a + 2;
      else if ((strcmp(a, "-I") == 0 || strcmp(a, "-D") == 0 || strcmp(a, "-U") == 0) &&
               next != NULL)
        {
          arg(&cppopts, a);
          arg(&cppopts, argv[++i]);
        }
      else if (strncmp(a, "-I", 2) == 0 || strncmp(a, "-D", 2) == 0 ||
               strncmp(a, "-U", 2) == 0)
        {
          arg(&cppopts, a);
        }
      else if (strncmp(a, "-W", 2) == 0 || strncmp(a, "-f", 2) == 0 ||
               strncmp(a, "-O", 2) == 0)
        {
          if (strncmp(a, "-Wl,", 4) != 0)
            arg(&ccopts, a);
        }
      else if (strcmp(a, "-L") == 0 && next != NULL)
        {
          arg(&ldopts, a);
          arg(&ldopts, argv[++i]);
        }
      else if (strncmp(a, "-L", 2) == 0 || strncmp(a, "-l", 2) == 0)
        arg(&ldopts, a);
      else if (strncmp(a, "--stack=", 8) == 0)
        stack = a;
      else if (strncmp(a, "--priority=", 11) == 0)
        priority = a;
      else if (strcmp(a, "-nostdlib") == 0)
        nostdlib = true;
      else if (strcmp(a, "-nostdinc") == 0)
        nostdinc = true;
      else if (strcmp(a, "-v") == 0)
        g_verbose = true;
      else if (strcmp(a, "-g") == 0 || strncmp(a, "-std=", 5) == 0 ||
               strcmp(a, "-ansi") == 0 || strcmp(a, "-pedantic") == 0 ||
               strncmp(a, "-m", 2) == 0)
        ;  // Ignored
      else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0)
        {
          usage(stdout);
          return 0;
        }
      else if (a[0] == '-' && a[1] != '\0')
        fprintf(stderr, "xcc: unknown option %s (ignored)\n", a);
      else
        arg(&sources, a);
    }

  if (sources.argc == 0)
    {
      usage(stderr);
      return 1;
    }

  if (ofn != NULL && stage != STAGE_LINK && sources.argc > 1)
    {
      fprintf(stderr, "xcc: -o with -c, -S or -E takes one file\n");
      return 1;
    }

  struct args_s objs = {0};
  for (int s = 0; s < sources.argc; ++s)
    {
      const char *src = sources.argv[s];
      const char *ext = ext_of(src);
      const char *asmfile = NULL;

      if (strcmp(ext, "c") == 0)
        {
          struct args_s a = {0};
          arg(&a, "cpp");
          for (size_t k = 0; k < sizeof(kPredefined) / sizeof(kPredefined[0]); ++k)
            arg(&a, kPredefined[k]);
          for (int k = 0; k < cppopts.argc; ++k)
            arg(&a, cppopts.argv[k]);
          if (!nostdinc)
            {
              arg(&a, "-I");
              arg(&a, path_join(armroot, "include"));
              arg(&a, "-I");
              arg(&a, path_join(armroot, "nuttx"));
            }

          if (stage == STAGE_PP)
            {
              if (ofn != NULL)
                {
                  arg(&a, "-o");
                  arg(&a, ofn);
                }

              arg(&a, src);
              if (run(&cpp_blob, xcc_cpp_main, &a) != 0)
                return 1;
              continue;
            }

          char *ifile = tmpname("i");
          arg(&a, "-o");
          arg(&a, ifile);
          arg(&a, src);
          if (run(&cpp_blob, xcc_cpp_main, &a) != 0)
            return 1;

          struct args_s c = {0};
          arg(&c, "cc1");
          for (int k = 0; k < ccopts.argc; ++k)
            arg(&c, ccopts.argv[k]);
          asmfile = stage == STAGE_ASM ? (ofn != NULL ? ofn : change_ext(src, "s"))
                                       : tmpname("s");
          arg(&c, "-o");
          arg(&c, asmfile);
          arg(&c, ifile);
          int rc = run(&cc1_blob, xcc_cc1_main, &c);
          remove(ifile);
          if (rc != 0)
            return 1;
          if (stage == STAGE_ASM)
            continue;
        }
      else if (strcmp(ext, "s") == 0 || strcmp(ext, "S") == 0)
        {
          if (stage <= STAGE_ASM)
            continue;
          asmfile = src;
        }
      else
        {
          arg(&objs, src);  // .o, .a
          continue;
        }

      const char *objfile = stage == STAGE_OBJ ? (ofn != NULL ? ofn : change_ext(src, "o"))
                                               : tmpname("o");
      struct args_s as = {0};
      arg(&as, "as");
      arg(&as, "-o");
      arg(&as, objfile);
      arg(&as, asmfile);
      int rc = run(&as_blob, xcc_as_main, &as);
      if (asmfile != src)
        remove(asmfile);
      if (rc != 0)
        return 1;
      arg(&objs, objfile);
    }

  if (stage != STAGE_LINK)
    return 0;

  struct args_s l = {0};
  arg(&l, "ld");
  arg(&l, "-o");
  arg(&l, ofn != NULL ? ofn : "a.out");
  arg(&l, stack);
  if (priority != NULL)
    arg(&l, priority);
  if (!nostdlib)
    arg(&l, path_join(armroot, "lib/crt0.o"));
  for (int k = 0; k < objs.argc; ++k)
    arg(&l, objs.argv[k]);
  for (int k = 0; k < ldopts.argc; ++k)
    arg(&l, ldopts.argv[k]);
  if (!nostdlib)
    {
      arg(&l, "-L");
      arg(&l, path_join(armroot, "lib"));
    }

  return run(&ld_blob, xcc_ld_main, &l) != 0;
}
