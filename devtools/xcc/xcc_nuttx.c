/****************************************************************************
 * apps/devtools/xcc/xcc_nuttx.c
 *
 * SPDX-License-Identifier: MIT
 *
 * xcc's process-like environment on NuttX (see xcc_nuttx.h): the arena,
 * exit(), memory files, and the snapshot of xcc's variables that makes
 * a built-in compiler (the simulator) start every run as a fresh process.
 *
 ****************************************************************************/

#define XCC_NUTTX_NO_MACROS
#include "xcc_nuttx.h"

#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * The arena
 ****************************************************************************/

/* Blocks of CHUNK bytes; a request larger than BIG gets a block of its
 * own.  Every allocation has its size in front of it (realloc needs it),
 * and the most recent one of a block can grow or shrink in place, which
 * is what xcc's growing vectors and buffers mostly do.
 */

#define CHUNK  (64 * 1024)
#define BIG    (16 * 1024)
#define ALIGN  8

struct chunk_s
{
  struct chunk_s *next;
  char *end;               /* End of the block */
  char *top;               /* First free byte */
  char *last;              /* The most recent allocation (its header) */
};

#define HDR  ((sizeof(size_t) + ALIGN - 1) & ~(ALIGN - 1))
#define CHDR ((sizeof(struct chunk_s) + ALIGN - 1) & ~(ALIGN - 1))

static struct chunk_s *g_chunks;       /* Newest first */
static size_t g_arena_bytes;           /* Bytes of blocks held */
static size_t g_arena_peak;

static struct chunk_s *chunk_of(void *p)
{
  struct chunk_s *c;

  for (c = g_chunks; c != NULL; c = c->next)
    {
      if ((char *)p > (char *)c && (char *)p < c->end)
        {
          return c;
        }
    }

  return NULL;
}

static struct chunk_s *chunk_new(size_t need)
{
  size_t size = need + CHDR > CHUNK ? need + CHDR : CHUNK;
  struct chunk_s *c = malloc(size);

  if (c == NULL)
    {
      return NULL;
    }

  c->end = (char *)c + size;
  c->top = (char *)c + CHDR;
  c->last = NULL;
  g_arena_bytes += size;
  if (g_arena_bytes > g_arena_peak)
    {
      g_arena_peak = g_arena_bytes;
    }

  if (need > BIG && g_chunks != NULL)
    {
      /* A block of its own: behind the current one, which stays the one
       * small allocations come from.
       */

      c->next = g_chunks->next;
      g_chunks->next = c;
    }
  else
    {
      c->next = g_chunks;
      g_chunks = c;
    }

  return c;
}

void *xcc_malloc(size_t size)
{
  size_t need = HDR + ((size + ALIGN - 1) & ~(size_t)(ALIGN - 1));
  struct chunk_s *c = g_chunks;
  char *h;

  if (size > BIG || c == NULL || (size_t)(c->end - c->top) < need)
    {
      c = chunk_new(need);
      if (c == NULL)
        {
          return NULL;
        }
    }

  h = c->top;
  c->top += need;
  c->last = h;
  *(size_t *)h = size;
  return h + HDR;
}

void *xcc_calloc(size_t n, size_t size)
{
  void *p = xcc_malloc(n * size);

  if (p != NULL)
    {
      memset(p, 0, n * size);
    }

  return p;
}

void xcc_free(void *p)
{
  struct chunk_s *c;

  if (p == NULL)
    {
      return;
    }

  c = chunk_of(p);
  if (c == NULL)
    {
      free(p);                         /* The C library's (getcwd...) */
    }
  else if (c->last == (char *)p - HDR)
    {
      c->top = c->last;                /* The most recent: given back */
      c->last = NULL;
    }
}

void *xcc_realloc(void *p, size_t size)
{
  struct chunk_s *c;
  size_t old;
  void *q;

  if (p == NULL)
    {
      return xcc_malloc(size);
    }

  c = chunk_of(p);
  if (c == NULL)
    {
      return realloc(p, size);
    }

  old = *(size_t *)((char *)p - HDR);
  if (c->last == (char *)p - HDR)
    {
      size_t need = HDR + ((size + ALIGN - 1) & ~(size_t)(ALIGN - 1));

      if ((size_t)(c->end - c->last) >= need)
        {
          c->top = c->last + need;
          *(size_t *)c->last = size;
          return p;
        }
    }

  q = xcc_malloc(size);
  if (q != NULL)
    {
      memcpy(q, p, old < size ? old : size);
      xcc_free(p);
    }

  return q;
}

char *xcc_strdup(const char *s)
{
  size_t n = strlen(s) + 1;
  char *d = xcc_malloc(n);

  if (d != NULL)
    {
      memcpy(d, s, n);
    }

  return d;
}

char *xcc_strndup(const char *s, size_t n)
{
  size_t len = strnlen(s, n);
  char *d = xcc_malloc(len + 1);

  if (d != NULL)
    {
      memcpy(d, s, len);
      d[len] = '\0';
    }

  return d;
}

ssize_t xcc_getline(char **lineptr, size_t *n, FILE *fp)
{
  size_t len = 0;
  int ch;

  if (*lineptr == NULL || *n == 0)
    {
      *n = 128;
      *lineptr = xcc_realloc(*lineptr, *n);
      if (*lineptr == NULL)
        {
          return -1;
        }
    }

  while ((ch = getc(fp)) != EOF)
    {
      if (len + 2 > *n)
        {
          char *l = xcc_realloc(*lineptr, *n * 2);

          if (l == NULL)
            {
              return -1;
            }

          *lineptr = l;
          *n *= 2;
        }

      (*lineptr)[len++] = ch;
      if (ch == '\n')
        {
          break;
        }
    }

  (*lineptr)[len] = '\0';
  return len > 0 ? (ssize_t)len : -1;
}

size_t xcc_arena_peak(void)
{
  return g_arena_peak;
}

static void arena_release(void)
{
  while (g_chunks != NULL)
    {
      struct chunk_s *c = g_chunks;

      g_chunks = c->next;
      free(c);
    }

  g_arena_bytes = 0;
}

/****************************************************************************
 * Memory files
 ****************************************************************************/

struct memfile_s
{
  struct memfile_s *next;
  char *name;                          /* NULL: tmpfile() */
  char *buf;
  size_t len;
  size_t cap;
  int opens;                           /* Streams open on it */
  bool removed;                        /* Freed when the last one closes */
};

struct memstream_s
{
  struct memfile_s *mf;
  size_t pos;
};

static struct memfile_s *g_memfiles;

static struct memfile_s *memfile_find(const char *name)
{
  struct memfile_s *mf;

  for (mf = g_memfiles; mf != NULL; mf = mf->next)
    {
      if (mf->name != NULL && !mf->removed && strcmp(mf->name, name) == 0)
        {
          return mf;
        }
    }

  return NULL;
}

static void memfile_unlink(struct memfile_s *mf)
{
  struct memfile_s **pp;

  for (pp = &g_memfiles; *pp != NULL; pp = &(*pp)->next)
    {
      if (*pp == mf)
        {
          *pp = mf->next;
          free(mf->buf);
          free(mf->name);
          free(mf);
          return;
        }
    }
}

static ssize_t ms_read(void *cookie, char *buf, size_t size)
{
  struct memstream_s *ms = cookie;
  struct memfile_s *mf = ms->mf;
  size_t n = ms->pos < mf->len ? mf->len - ms->pos : 0;

  if (n > size)
    {
      n = size;
    }

  memcpy(buf, mf->buf + ms->pos, n);
  ms->pos += n;
  return n;
}

static ssize_t ms_write(void *cookie, const char *buf, size_t size)
{
  struct memstream_s *ms = cookie;
  struct memfile_s *mf = ms->mf;
  size_t end = ms->pos + size;

  if (end > mf->cap)
    {
      size_t cap = mf->cap ? mf->cap : 4096;
      char *b;

      while (cap < end)
        {
          cap *= 2;
        }

      b = realloc(mf->buf, cap);
      if (b == NULL)
        {
          errno = ENOMEM;
          return -1;
        }

      mf->buf = b;
      mf->cap = cap;
    }

  if (ms->pos > mf->len)
    {
      memset(mf->buf + mf->len, 0, ms->pos - mf->len);
    }

  memcpy(mf->buf + ms->pos, buf, size);
  ms->pos = end;
  if (end > mf->len)
    {
      mf->len = end;
    }

  return size;
}

static off_t ms_seek(void *cookie, off_t *offset, int whence)
{
  struct memstream_s *ms = cookie;
  off_t base = whence == SEEK_SET ? 0 :
               whence == SEEK_CUR ? (off_t)ms->pos : (off_t)ms->mf->len;

  if (base + *offset < 0)
    {
      errno = EINVAL;
      return -1;
    }

  ms->pos = base + *offset;
  *offset = ms->pos;
  return ms->pos;
}

static int ms_close(void *cookie)
{
  struct memstream_s *ms = cookie;
  struct memfile_s *mf = ms->mf;

  free(ms);
  if (--mf->opens == 0 && mf->removed)
    {
      memfile_unlink(mf);
    }

  return 0;
}

static FILE *memfile_open(struct memfile_s *mf, const char *mode)
{
  cookie_io_functions_t io =
  {
    ms_read, ms_write, ms_seek, ms_close
  };

  struct memstream_s *ms = malloc(sizeof(*ms));
  FILE *fp;

  if (ms == NULL)
    {
      return NULL;
    }

  if (mode[0] == 'w')
    {
      mf->len = 0;
    }

  ms->mf = mf;
  ms->pos = mode[0] == 'a' ? mf->len : 0;
  fp = fopencookie(ms, mode, io);
  if (fp == NULL)
    {
      free(ms);
      return NULL;
    }

  mf->opens++;
  return fp;
}

static struct memfile_s *memfile_new(const char *name)
{
  struct memfile_s *mf = calloc(1, sizeof(*mf));

  if (mf == NULL)
    {
      return NULL;
    }

  if (name != NULL && (mf->name = strdup(name)) == NULL)
    {
      free(mf);
      return NULL;
    }

  mf->next = g_memfiles;
  g_memfiles = mf;
  return mf;
}

FILE *xcc_memfile(const char *name)
{
  struct memfile_s *mf = memfile_find(name);

  if (mf == NULL && (mf = memfile_new(name)) == NULL)
    {
      return NULL;
    }

  return memfile_open(mf, "w+");
}

FILE *xcc_fopen(const char *path, const char *mode)
{
  struct memfile_s *mf = memfile_find(path);

  if (mf != NULL)
    {
      return memfile_open(mf, mode);
    }

  return fopen(path, mode);
}

FILE *xcc_tmpfile(void)
{
  struct memfile_s *mf = memfile_new(NULL);
  FILE *fp;

  if (mf == NULL)
    {
      return NULL;
    }

  mf->removed = true;                  /* Gone when closed */
  fp = memfile_open(mf, "w+");
  if (fp == NULL)
    {
      memfile_unlink(mf);
    }

  return fp;
}

int xcc_remove(const char *path)
{
  struct memfile_s *mf = memfile_find(path);

  if (mf == NULL)
    {
      return remove(path);
    }

  mf->removed = true;
  if (mf->opens == 0)
    {
      memfile_unlink(mf);
    }

  return 0;
}

int xcc_stat(const char *path, struct stat *st)
{
  struct memfile_s *mf = memfile_find(path);

  if (mf == NULL)
    {
      return stat(path, st);
    }

  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFREG | 0644;
  st->st_size = mf->len;
  return 0;
}

static void memfiles_release(void)
{
  /* Streams still open belong to the task, which closes them as it ends
   * (a built-in compiler's are left: an error exit leaks the FILE, not
   * the data).
   */

  struct memfile_s *mf = g_memfiles;

  while (mf != NULL)
    {
      struct memfile_s *next = mf->next;

      if (mf->opens == 0)
        {
          memfile_unlink(mf);
        }
      else
        {
          mf->removed = true;
        }

      mf = next;
    }
}

/****************************************************************************
 * exit(), atexit() and the run
 ****************************************************************************/

#define MAX_ATEXIT 8

static jmp_buf g_exit_jmp;
static bool g_running;
static int g_exit_status;
static void (*g_atexit[MAX_ATEXIT])(void);
static int g_natexit;

void xcc_exit(int status)
{
  if (g_running)
    {
      g_exit_status = status;
      longjmp(g_exit_jmp, 1);
    }

  exit(status);
}

int xcc_atexit(void (*fn)(void))
{
  if (g_natexit >= MAX_ATEXIT)
    {
      return -1;
    }

  g_atexit[g_natexit++] = fn;
  return 0;
}

/* xcc's variables: the build puts them all between these markers
 * (xcc.ld), so that a built-in compiler can put them back as they were
 * loaded before every run.  A module is loaded afresh for every run, but
 * the same holds there.
 */

extern char xcc_data_start[];
extern char xcc_data_end[];
extern char xcc_bss_start[];
extern char xcc_bss_end[];

static void snapshot_restore(void)
{
  static char *saved;
  size_t len = xcc_data_end - xcc_data_start;

  if (saved == NULL)
    {
      saved = malloc(len ? len : 1);
      if (saved != NULL)
        {
          memcpy(saved, xcc_data_start, len);
        }
    }
  else
    {
      memcpy(xcc_data_start, saved, len);
    }

  memset(xcc_bss_start, 0, xcc_bss_end - xcc_bss_start);
}

int xcc_run(int (*mainfn)(int, char **), int argc, char **argv)
{
  volatile int status;
  char *stats = getenv("XCC_STATS");

  snapshot_restore();
  g_natexit = 0;
  g_arena_peak = 0;
  g_running = true;
  if (setjmp(g_exit_jmp) == 0)
    {
      status = mainfn(argc, argv);
    }
  else
    {
      status = g_exit_status;
    }

  g_running = false;
  while (g_natexit > 0)
    {
      g_atexit[--g_natexit]();
    }

  fflush(stdout);
  fflush(stderr);
  memfiles_release();
  arena_release();
  if (stats != NULL && stats[0] == '1')
    {
      fprintf(stderr, "xcc: arena peak %zu KB\n", g_arena_peak / 1024);
    }

  return status;
}
