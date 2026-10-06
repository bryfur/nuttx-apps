/****************************************************************************
 * apps/devtools/xcc/xcc_nuttx.h
 *
 * SPDX-License-Identifier: MIT
 *
 * Included first in every file of xcc (-include) when it is built for
 * NuttX: what a compiler that expects a process of its own needs from a
 * flat NuttX build.
 *
 * - Memory: xcc frees almost nothing (it lets the process end do that), and
 *   in a flat build a task's malloc'd memory outlives it.  Its allocations
 *   go to an arena of large blocks (xcc_malloc...) that is given back whole
 *   when the compiler returns, however it ends.
 * - exit(): an error deep in the compiler exits; here that longjmps back to
 *   xcc_run(), which cleans up and returns the status.
 * - Temporary files: there is no /tmp on the board.  tmpfile() and the
 *   intermediate files of a compile (named, e.g. the object an executable
 *   is linked from) are memory files (fopencookie), which fopen(), stat()
 *   and remove() know by name.
 *
 ****************************************************************************/

#ifndef __APPS_DEVTOOLS_XCC_XCC_NUTTX_H
#define __APPS_DEVTOOLS_XCC_XCC_NUTTX_H

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* xcc has getopt's variables of its own (util.c): NuttX's are macros */

#undef optarg
#undef optind
#undef opterr
#undef optopt
#define optarg xcc_optarg
#define optind xcc_optind
#define opterr xcc_opterr
#define optopt xcc_optopt

void *xcc_malloc(size_t size);
void *xcc_calloc(size_t n, size_t size);
void *xcc_realloc(void *p, size_t size);
void xcc_free(void *p);
char *xcc_strdup(const char *s);
char *xcc_strndup(const char *s, size_t n);
ssize_t xcc_getline(char **lineptr, size_t *n, FILE *fp);
void xcc_exit(int status) __attribute__((noreturn));
int xcc_atexit(void (*fn)(void));

FILE *xcc_fopen(const char *path, const char *mode);
FILE *xcc_tmpfile(void);
int xcc_remove(const char *path);
int xcc_stat(const char *path, struct stat *st);

/* A memory file of this name (created, or emptied), open for writing */

FILE *xcc_memfile(const char *name);

/* One of xcc's programs linked into one object (the Makefile, xcc.ld):
 * the bounds of its variables, put back as loaded before each run
 */

struct xcc_blob_s
{
  char *data_start;
  char *data_end;
  char *bss_start;
  char *bss_end;
  char *saved;
};

#define XCC_BLOB(p) \
  extern char p##_data_start[], p##_data_end[], p##_bss_start[], p##_bss_end[]; \
  static struct xcc_blob_s p##_blob = \
    { p##_data_start, p##_data_end, p##_bss_start, p##_bss_end, NULL }

/* Runs a program's main() as if it were a process of its own: its exit(),
 * its memory (given back at the end), its variables as loaded.  A run may
 * start another (the driver its compiler...).
 */

int xcc_run(struct xcc_blob_s *blob, int (*mainfn)(int, char **), int argc,
            char **argv);

/* The arena's high-water mark (bytes): XCC_STATS=1 prints it at the end */

size_t xcc_arena_peak(void);

#ifndef XCC_NUTTX_NO_MACROS
#  define malloc(n)        xcc_malloc(n)
#  define calloc(n, s)     xcc_calloc(n, s)
#  define realloc(p, n)    xcc_realloc(p, n)
#  define free(p)          xcc_free(p)
#  define strdup(s)        xcc_strdup(s)
#  define strndup(s, n)    xcc_strndup(s, n)
#  define getline(l, n, f) xcc_getline(l, n, f)
#  define exit(s)          xcc_exit(s)
#  define atexit(f)        xcc_atexit(f)
#  define fopen(p, m)      xcc_fopen(p, m)
#  define tmpfile()        xcc_tmpfile()
#  define remove(p)        xcc_remove(p)
#  define stat(p, s)       xcc_stat(p, s)
#endif

#endif /* __APPS_DEVTOOLS_XCC_XCC_NUTTX_H */
