/****************************************************************************
 * apps/devtools/xcc/xcc_nuttx.c
 *
 * SPDX-License-Identifier: MIT
 *
 * xcc's process-like environment on NuttX (see xcc_nuttx.h): the arena,
 * exit(), the streams, memory files, and the snapshot of xcc's variables that makes
 * a built-in compiler (the simulator) start every run as a fresh process.
 *
 ****************************************************************************/

#define XCC_NUTTX_NO_MACROS
#include "xcc_nuttx.h"

#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * The arena
 ****************************************************************************/

/* Blocks of CHUNK bytes; a request larger than BIG gets a block of its
 * own.  Every allocation has its size in front of it (realloc needs it),
 * and the most recent one of a block can grow or shrink in place, which
 * is what xcc's growing vectors and buffers mostly do.  The blocks are
 * listed newest first, so a nested run (xcc_run) can give back what it
 * took: everything newer than its mark.
 */

#define CHUNK  (64 * 1024)
#define SMALL  256             /* Larger: the heap's, freed when freed */
#define ALIGN  8
#define MAGIC  0x58434342      /* "XCCB" */
#define XCC_FILE_BUFFER 4096

struct chunk_s
{
  struct chunk_s *next;
  char *end;               /* End of the block */
  char *top;               /* First free byte */
  char *last;              /* The most recent allocation (its header) */
};

/* A larger allocation: from the heap, listed by the run that made it (all
 * given back at its end) */

struct big_s
{
  struct big_s *next;
  struct big_s *prev;
  uint32_t magic;
  uint32_t level;
  size_t pad;
  size_t word;             /* (size << 1) | 1: right before the memory */
};

/* An arena allocation's header: its word, (size << 1), right before it */

#define HDR  ((sizeof(size_t) + ALIGN - 1) & ~(ALIGN - 1))
#define CHDR ((sizeof(struct chunk_s) + ALIGN - 1) & ~(ALIGN - 1))
#define WORD(p) (((size_t *)(p))[-1])

#define MAX_DEPTH  4

static struct chunk_s *g_chunks;       /* All, newest first */
static struct chunk_s *g_cur;          /* Where small allocations go */
static struct big_s *g_big[MAX_DEPTH + 1];
static size_t g_arena_bytes;           /* Bytes of blocks held */
static size_t g_heap_bytes;            /* Bytes of larger allocations */
static size_t g_arena_peak;
static int g_depth;                    /* Runs nested (xcc_run) */

static void note_peak(void)
{
  if (g_arena_bytes + g_heap_bytes > g_arena_peak)
    {
      g_arena_peak = g_arena_bytes + g_heap_bytes;
    }
}

static struct chunk_s *chunk_new(void)
{
  struct chunk_s *c = malloc(CHUNK);

  if (c == NULL)
    {
      return NULL;
    }

  c->end = (char *)c + CHUNK;
  c->top = (char *)c + CHDR;
  c->last = NULL;
  c->next = g_chunks;
  g_chunks = c;
  g_cur = c;
  g_arena_bytes += CHUNK;
  note_peak();
  return c;
}

static void *big_alloc(size_t size)
{
  struct big_s *b = malloc(sizeof(*b) + size);
  int level = g_depth;

  if (b == NULL)
    {
      return NULL;
    }

  b->magic = MAGIC;
  b->level = level;
  b->word = (size << 1) | 1;
  b->prev = NULL;
  b->next = g_big[level];
  if (b->next != NULL)
    {
      b->next->prev = b;
    }

  g_big[level] = b;
  g_heap_bytes += size;
  note_peak();
  return b + 1;
}

static void big_unlink(struct big_s *b)
{
  if (b->prev != NULL)
    {
      b->prev->next = b->next;
    }
  else
    {
      g_big[b->level] = b->next;
    }

  if (b->next != NULL)
    {
      b->next->prev = b->prev;
    }

  g_heap_bytes -= b->word >> 1;
}

static struct big_s *big_of(void *p)
{
  struct big_s *b = (struct big_s *)p - 1;

  return (WORD(p) & 1) && b->magic == MAGIC ? b : NULL;
}

void *xcc_malloc(size_t size)
{
  size_t need = HDR + ((size + ALIGN - 1) & ~(size_t)(ALIGN - 1));
  struct chunk_s *c = g_cur;
  char *h;

  if (size > SMALL)
    {
      return big_alloc(size);
    }

  if (c == NULL || (size_t)(c->end - c->top) < need)
    {
      c = chunk_new();
      if (c == NULL)
        {
          return NULL;
        }
    }

  h = c->top;
  c->top += need;
  c->last = h;
  WORD(h + HDR) = size << 1;
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
  struct big_s *b;

  if (p == NULL)
    {
      return;
    }

  if (WORD(p) & 1)
    {
      b = big_of(p);
      if (b != NULL)
        {
          big_unlink(b);
          b->magic = 0;
          free(b);
        }
      else
        {
          free(p);                     /* The C library's */
        }
    }
  else if (g_cur != NULL && g_cur->last == (char *)p - HDR)
    {
      g_cur->top = g_cur->last;        /* The most recent: given back */
      g_cur->last = NULL;
    }

  /* Any other small one stays till the end of the run */
}

void *xcc_realloc(void *p, size_t size)
{
  size_t old;
  struct big_s *b;
  void *q;

  if (p == NULL)
    {
      return xcc_malloc(size);
    }

  if (WORD(p) & 1)
    {
      b = big_of(p);
      if (b == NULL)
        {
          return realloc(p, size);     /* The C library's */
        }

      if (size > SMALL)
        {
          struct big_s *n;

          big_unlink(b);
          n = realloc(b, sizeof(*b) + size);
          if (n == NULL)
            {
              /* Still there: listed again */

              n = b;
              size = n->word >> 1;
              q = NULL;
            }
          else
            {
              q = n + 1;
            }

          n->word = (size << 1) | 1;
          n->prev = NULL;
          n->next = g_big[n->level];
          if (n->next != NULL)
            {
              n->next->prev = n;
            }

          g_big[n->level] = n;
          g_heap_bytes += size;
          note_peak();
          return q;
        }

      old = b->word >> 1;
    }
  else
    {
      old = WORD(p) >> 1;
      if (size <= SMALL && g_cur != NULL && g_cur->last == (char *)p - HDR)
        {
          size_t need = HDR + ((size + ALIGN - 1) & ~(size_t)(ALIGN - 1));

          if ((size_t)(g_cur->end - g_cur->last) >= need)
            {
              g_cur->top = g_cur->last + need;
              WORD(p) = size << 1;
              return p;
            }
        }

      if (size <= old)
        {
          return p;                    /* Smaller: stays where it is */
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

char *xcc_getcwd(char *buf, size_t size)
{
  char tmp[PATH_MAX];

  if (buf != NULL)
    {
      return getcwd(buf, size);
    }

  if (getcwd(tmp, sizeof(tmp)) == NULL)
    {
      return NULL;
    }

  return xcc_strdup(tmp);
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

  if (*lineptr == NULL || *n < 2)
    {
      *n = 128;
      *lineptr = xcc_realloc(*lineptr, *n);
      if (*lineptr == NULL)
        {
          return -1;
        }
    }

  /* fgets: a lock and a copy a line (getc took them a character) */

  for (; ; )
    {
      if (fgets(*lineptr + len, *n - len, fp) == NULL)
        {
          break;
        }

      len += strlen(*lineptr + len);
      if (len > 0 && (*lineptr)[len - 1] == '\n')
        {
          break;
        }

      if (len + 1 >= *n)
        {
          char *l = xcc_realloc(*lineptr, *n * 2);

          if (l == NULL)
            {
              return -1;
            }

          *lineptr = l;
          *n *= 2;
        }
    }

  (*lineptr)[len] = '\0';

  /* The line kept as long as it is (the preprocessor keeps most) */

  if (len + 1 < *n)
    {
      char *l = xcc_realloc(*lineptr, len + 1);

      if (l != NULL)
        {
          *lineptr = l;
          *n = len + 1;
        }
    }

  return len > 0 ? (ssize_t)len : -1;
}

size_t xcc_arena_peak(void)
{
  return g_arena_peak;
}

struct arena_mark_s
{
  struct chunk_s *chunks;
  struct chunk_s *cur;
  char *top;
  char *last;
};

static void arena_mark(struct arena_mark_s *m)
{
  m->chunks = g_chunks;
  m->cur = g_cur;
  m->top = g_cur != NULL ? g_cur->top : NULL;
  m->last = g_cur != NULL ? g_cur->last : NULL;
}

/* Everything allocated since the mark given back: the blocks newer than
 * it, and the larger allocations of the run at `level` */

static void arena_release_to(const struct arena_mark_s *m, int level)
{
  while (g_chunks != m->chunks)
    {
      struct chunk_s *c = g_chunks;

      g_chunks = c->next;
      g_arena_bytes -= c->end - (char *)c;
      free(c);
    }

  g_cur = m->cur;
  if (g_cur != NULL)
    {
      g_cur->top = m->top;
      g_cur->last = m->last;
    }

  while (g_big[level] != NULL)
    {
      struct big_s *b = g_big[level];

      big_unlink(b);
      b->magic = 0;
      free(b);
    }
}

/****************************************************************************
 * Streams
 ****************************************************************************/

/* Every stream a run opens, with the run's depth: those still open when
 * the run ends (an error exit deep in the compiler leaves its files open)
 * are closed then.  In a flat build nothing else would free them: a FILE,
 * its buffer and a memory stream's cookie are the C library's heap's and
 * outlive the task.
 */

struct stream_s
{
  struct stream_s *next;
  FILE *fp;
  int level;
};

static struct stream_s *g_streams;
static bool g_releasing;               /* Closing a run's leftovers */

static FILE *stream_track(FILE *fp)
{
  struct stream_s *s;

  if (fp == NULL)
    {
      return NULL;
    }

  s = malloc(sizeof(*s));
  if (s == NULL)
    {
      fclose(fp);
      errno = ENOMEM;
      return NULL;
    }

  s->fp = fp;
  s->level = g_depth;
  s->next = g_streams;
  g_streams = s;
  return fp;
}

int xcc_fclose(FILE *fp)
{
  struct stream_s **pp;

  for (pp = &g_streams; *pp != NULL; pp = &(*pp)->next)
    {
      if ((*pp)->fp == fp)
        {
          struct stream_s *s = *pp;

          *pp = s->next;
          free(s);
          break;
        }
    }

  return fclose(fp);
}

FILE *xcc_fdopen(int fd, const char *mode)
{
  return stream_track(fdopen(fd, mode));
}

/* The streams of runs at `level` and deeper closed */

static void streams_release(int level)
{
  struct stream_s **pp = &g_streams;

  g_releasing = true;
  while (*pp != NULL)
    {
      struct stream_s *s = *pp;

      if (s->level >= level)
        {
          *pp = s->next;
          fclose(s->fp);
          free(s);
        }
      else
        {
          pp = &s->next;
        }
    }

  g_releasing = false;
}

/* open_memstream(): the buffer is the arena's (xcc_realloc), freed with
 * free() as the C library's would be, or when the run ends (the C
 * library's, realloc'd to the exact size at every write, outlived the
 * task).  Its capacity doubles; it always ends with a '\0'.
 */

struct dynstream_s
{
  char **bufp;
  size_t *sizep;
  char *buf;
  size_t len;
  size_t cap;                          /* Bytes of buf, its '\0' included */
  size_t pos;
};

static void ds_publish(struct dynstream_s *ds)
{
  /* Not while a run's leftovers are closed: `bufp` may be a variable of a
   * function that an exit() left (preprocess_one_line's)
   */

  if (!g_releasing)
    {
      *ds->bufp = ds->buf;
      *ds->sizep = ds->pos < ds->len ? ds->pos : ds->len;
    }
}

static ssize_t ds_write(void *cookie, const char *buf, size_t size)
{
  struct dynstream_s *ds = cookie;
  size_t end = ds->pos + size;

  if (end + 1 > ds->cap)
    {
      size_t cap = ds->cap * 2;
      char *b;

      if (cap < end + 1)
        {
          cap = end + 1;
        }

      b = xcc_realloc(ds->buf, cap);
      if (b == NULL)
        {
          errno = ENOMEM;
          return -1;
        }

      ds->buf = b;
      ds->cap = cap;
    }

  if (ds->pos > ds->len)
    {
      memset(ds->buf + ds->len, 0, ds->pos - ds->len);
    }

  memcpy(ds->buf + ds->pos, buf, size);
  ds->pos = end;
  if (end > ds->len)
    {
      ds->len = end;
      ds->buf[end] = '\0';
    }

  ds_publish(ds);
  return size;
}

static off_t ds_seek(void *cookie, off_t *offset, int whence)
{
  struct dynstream_s *ds = cookie;
  off_t base = whence == SEEK_SET ? 0 :
               whence == SEEK_CUR ? (off_t)ds->pos : (off_t)ds->len;

  if (base + *offset < 0)
    {
      errno = EINVAL;
      return -1;
    }

  ds->pos = base + *offset;
  *offset = ds->pos;
  ds_publish(ds);
  return ds->pos;
}

static int ds_close(void *cookie)
{
  struct dynstream_s *ds = cookie;

  ds_publish(ds);
  free(ds);
  return 0;
}

FILE *xcc_open_memstream(char **bufp, size_t *sizep)
{
  cookie_io_functions_t io =
  {
    NULL, ds_write, ds_seek, ds_close
  };

  struct dynstream_s *ds = malloc(sizeof(*ds));
  FILE *fp;

  if (ds == NULL)
    {
      return NULL;
    }

  ds->cap = 64;
  ds->buf = xcc_malloc(ds->cap);
  if (ds->buf == NULL)
    {
      free(ds);
      return NULL;
    }

  ds->buf[0] = '\0';
  ds->len = 0;
  ds->pos = 0;
  ds->bufp = bufp;
  ds->sizep = sizep;
  fp = fopencookie(ds, "w", io);
  if (fp == NULL)
    {
      xcc_free(ds->buf);
      free(ds);
      return NULL;
    }

  ds_publish(ds);
  return stream_track(fp);
}

/* fmemopen(), for reading (all xcc does with it) */

struct rostream_s
{
  const char *buf;
  size_t size;
  size_t pos;
};

static ssize_t ro_read(void *cookie, char *buf, size_t size)
{
  struct rostream_s *rs = cookie;
  size_t n = rs->pos < rs->size ? rs->size - rs->pos : 0;

  if (n > size)
    {
      n = size;
    }

  memcpy(buf, rs->buf + rs->pos, n);
  rs->pos += n;
  return n;
}

static off_t ro_seek(void *cookie, off_t *offset, int whence)
{
  struct rostream_s *rs = cookie;
  off_t base = whence == SEEK_SET ? 0 :
               whence == SEEK_CUR ? (off_t)rs->pos : (off_t)rs->size;

  if (base + *offset < 0 || base + *offset > (off_t)rs->size)
    {
      errno = EINVAL;
      return -1;
    }

  rs->pos = base + *offset;
  *offset = rs->pos;
  return rs->pos;
}

static int ro_close(void *cookie)
{
  free(cookie);
  return 0;
}

FILE *xcc_fmemopen(void *buf, size_t size, const char *mode)
{
  cookie_io_functions_t io =
  {
    ro_read, NULL, ro_seek, ro_close
  };

  struct rostream_s *rs;
  FILE *fp;

  if (mode[0] != 'r' || strchr(mode, '+') != NULL)
    {
      errno = EINVAL;
      return NULL;
    }

  rs = malloc(sizeof(*rs));
  if (rs == NULL)
    {
      return NULL;
    }

  rs->buf = buf;
  rs->size = size;
  rs->pos = 0;
  fp = fopencookie(rs, mode, io);
  if (fp == NULL)
    {
      free(rs);
      return NULL;
    }

  return stream_track(fp);
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
  if (fp != NULL)
    {
      setvbuf(fp, NULL, _IOFBF, XCC_FILE_BUFFER);  /* Not line by line */
    }

  if (fp == NULL)
    {
      free(ms);
      return NULL;
    }

  mf->opens++;
  return stream_track(fp);
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
  FILE *fp;

  if (mf != NULL)
    {
      return memfile_open(mf, mode);
    }

  fp = fopen(path, mode);
  if (fp != NULL)
    {
      /* A larger buffer than the C library's (CONFIG_STDIO_BUFFER_SIZE):
       * fewer reads of the SD card, fewer calls through its layers
       */

      setvbuf(fp, NULL, _IOFBF, XCC_FILE_BUFFER);
    }

  return stream_track(fp);
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
  /* The runs' streams are closed by now (streams_release): any still
   * open is the caller's
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
 * exit(), atexit() and the runs
 ****************************************************************************/

/* A run of one of xcc's programs (its main); a run may start another (the
 * driver its compiler, assembler, linker): their exits, atexit functions
 * and arena use nest.
 */

#define MAX_ATEXIT 8

struct level_s
{
  jmp_buf jmp;
  int status;
  void (*atexit[MAX_ATEXIT])(void);
  int natexit;
  struct arena_mark_s mark;
};

static struct level_s g_levels[MAX_DEPTH];

void xcc_exit(int status)
{
  if (g_depth > 0)
    {
      struct level_s *l = &g_levels[g_depth - 1];

      l->status = status;
      longjmp(l->jmp, 1);
    }

  exit(status);
}

int xcc_atexit(void (*fn)(void))
{
  struct level_s *l;

  if (g_depth == 0)
    {
      return atexit(fn);
    }

  l = &g_levels[g_depth - 1];
  if (l->natexit >= MAX_ATEXIT)
    {
      return -1;
    }

  l->atexit[l->natexit++] = fn;
  return 0;
}

/* A program's variables: the build puts them all between its markers
 * (xcc_marks.c, xcc.ld), so that a program can be put back as it was
 * loaded before every run (a built-in compiler; a program run twice by
 * the driver).
 */

/* The programs whose variables are saved: a loadable module (XCC_MODULE)
 * gives the copies back when its outermost run ends (the next start loads
 * the module again, its variables as in the file); a built-in compiler
 * keeps them for its next run
 */

#define MAX_BLOBS 8

static struct xcc_blob_s *g_blobs[MAX_BLOBS];
static int g_nblobs;

static void blobs_release(void)
{
#ifdef XCC_MODULE
  while (g_nblobs > 0)
    {
      struct xcc_blob_s *b = g_blobs[--g_nblobs];

      free(b->saved);
      b->saved = NULL;
    }
#endif
}

static void blob_restore(struct xcc_blob_s *b)
{
  size_t len = b->data_end - b->data_start;

  if (b->saved == NULL)
    {
      b->saved = malloc(len ? len : 1);
      if (b->saved != NULL)
        {
          memcpy(b->saved, b->data_start, len);
          if (g_nblobs < MAX_BLOBS)
            {
              g_blobs[g_nblobs++] = b;
            }
        }
    }
  else
    {
      memcpy(b->data_start, b->saved, len);
    }

  memset(b->bss_start, 0, b->bss_end - b->bss_start);
}

int xcc_run(struct xcc_blob_s *blob, int (*mainfn)(int, char **), int argc,
            char **argv)
{
  volatile int status;
  struct level_s *l;
  char *stats = getenv("XCC_STATS");

  if (g_depth >= MAX_DEPTH)
    {
      fprintf(stderr, "xcc: runs nested too deep\n");
      return 1;
    }

  if (g_depth == 0)
    {
      g_arena_peak = g_arena_bytes + g_heap_bytes;
    }

  l = &g_levels[g_depth];
  l->natexit = 0;
  arena_mark(&l->mark);
  blob_restore(blob);
  ++g_depth;
  if (setjmp(l->jmp) == 0)
    {
      status = mainfn(argc, argv);
    }
  else
    {
      status = l->status;
    }

  while (l->natexit > 0)
    {
      l->atexit[--l->natexit]();
    }

  --g_depth;
  streams_release(g_depth + 1);
  fflush(stdout);
  fflush(stderr);
  arena_release_to(&l->mark, g_depth + 1);
  if (g_depth == 0)
    {
      memfiles_release();
      blobs_release();
      if (stats != NULL && stats[0] == '1')
        {
          fprintf(stderr, "xcc: arena peak %zu KB\n", g_arena_peak / 1024);
        }
    }

  return status;
}
