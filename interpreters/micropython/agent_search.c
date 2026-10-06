/****************************************************************************
 * apps/interpreters/micropython/agent_search.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * _agent.grep, _agent.glob, _agent.fnmatch: the agent's search tools,
 * walking folders in C.
 *
 *   grep(pattern, path=".", glob=None, icase=False, fixed=False,
 *        files=False, max=100)
 *       -> (matches, files searched, truncated); matches are (path,
 *          line number, line) or, with files, paths.  The pattern is a
 *          regular expression of MicroPython's re (re1.5: . [] [^] * + ?
 *          | () ^ $ \d \w \s ...), or a plain string with fixed.  glob
 *          picks the files by name ("*.py", "*.{c,h}").  Binary files
 *          (a NUL in their first KB) are left out.
 *   glob(pattern, root=".", max=500) -> [(path, mtime, size)], newest
 *       first.  * ? [abc] {a,b} in a part, ** any number of folders.
 *   fnmatch(pattern, name) -> bool
 *
 * Hidden folders (.git ...) are not walked into unless the pattern or the
 * path names them; nor /proc, /dev, /sys from /.  Ctrl-C stops a search.
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "py/runtime.h"
#include "lib/re1.5/re1.5.h"

#include "modagent.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAXDEPTH        24
#define LINEMAX         2048           /* Longer lines are matched in parts */
#define SHOWMAX         300            /* Of a matching line, shown */
#define FILEMAX         (4 * 1024 * 1024)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct grep_s
{
  /* What */

  ByteProg   *prog;
  const char *fixed;
  size_t      fixedn;
  bool        icase;
  bool        files;
  const char *glob;
  int         max;

  /* Found */

  mp_obj_t    list;
  int         nfound;
  int         nfiles;
  bool        truncated;
  char        line[LINEMAX + 1];
  char        lower[LINEMAX + 1];
};

/****************************************************************************
 * Private Functions: names
 ****************************************************************************/

/* fnmatch with {a,b}: * ? [set] [!set] {alt,alt} \x */

static bool match_here(const char *p, const char *pe, const char *s,
                       const char *se)
{
  while (p < pe)
    {
      char c = *p;

      if (c == '*')
        {
          p++;
          while (p < pe && *p == '*')
            {
              p++;
            }

          if (p == pe)
            {
              return true;
            }

          for (; s <= se; s++)
            {
              if (match_here(p, pe, s, se))
                {
                  return true;
                }
            }

          return false;
        }

      if (c == '{')
        {
          /* Each alternative followed by the rest of the pattern */

          const char *close = p + 1;
          int depth = 0;

          while (close < pe && (*close != '}' || depth > 0))
            {
              depth += *close == '{' ? 1 : *close == '}' ? -1 : 0;
              close++;
            }

          if (close < pe)
            {
              const char *a = p + 1;

              for (; ; )
                {
                  const char *e = a;
                  int d = 0;
                  char tmp[256];
                  size_t n1;
                  size_t n2;

                  while (e < close && (*e != ',' || d > 0))
                    {
                      d += *e == '{' ? 1 : *e == '}' ? -1 : 0;
                      e++;
                    }

                  n1 = e - a;
                  n2 = pe - (close + 1);
                  if (n1 + n2 < sizeof(tmp))
                    {
                      memcpy(tmp, a, n1);
                      memcpy(tmp + n1, close + 1, n2);
                      if (match_here(tmp, tmp + n1 + n2, s, se))
                        {
                          return true;
                        }
                    }

                  if (e >= close)
                    {
                      return false;
                    }

                  a = e + 1;
                }
            }
        }

      if (s >= se)
        {
          return false;
        }

      if (c == '?')
        {
          p++;
          s++;
          continue;
        }

      if (c == '[')
        {
          const char *q = p + 1;
          bool neg = false;
          bool ok = false;

          if (q < pe && (*q == '!' || *q == '^'))
            {
              neg = true;
              q++;
            }

          while (q < pe && (*q != ']' || q == p + 1 + neg))
            {
              if (q + 2 < pe && q[1] == '-' && q[2] != ']')
                {
                  ok |= *s >= q[0] && *s <= q[2];
                  q += 3;
                }
              else
                {
                  ok |= *s == *q;
                  q++;
                }
            }

          if (q >= pe || ok == neg)
            {
              return false;
            }

          p = q + 1;
          s++;
          continue;
        }

      if (c == '\\' && p + 1 < pe)
        {
          p++;
          c = *p;
        }

      if (*s != c)
        {
          return false;
        }

      p++;
      s++;
    }

  return s == se;
}

static bool name_match(const char *pat, size_t pn, const char *name)
{
  return match_here(pat, pat + pn, name, name + strlen(name));
}

static bool has_magic(const char *s, size_t n)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      if (strchr("*?[{", s[i]) != NULL)
        {
          return true;
        }
    }

  return false;
}

/* Folders never walked into from above them */

static bool skip_dir(const char *path, const char *name)
{
  if (name[0] == '.')
    {
      return true;
    }

  return strcmp(path, "/proc") == 0 || strcmp(path, "/dev") == 0 ||
         strcmp(path, "/sys") == 0 || strcmp(name, "__pycache__") == 0;
}

static size_t path_add(char *path, size_t len, const char *name)
{
  size_t n = strlen(name);

  if (len + 1 + n >= PATH_MAX)
    {
      return 0;
    }

  if (len == 0 || path[len - 1] != '/')
    {
      path[len++] = '/';
    }

  memcpy(path + len, name, n + 1);
  return len + n;
}

static bool is_dir_entry(const char *path, struct dirent *d)
{
#ifdef DT_DIR
  if (d->d_type == DT_DIR)
    {
      return true;
    }

  if (d->d_type == DT_REG)
    {
      return false;
    }
#endif

  struct stat st;

  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/****************************************************************************
 * Private Functions: grep
 ****************************************************************************/

static void lower_into(char *dst, const char *src, size_t n)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      dst[i] = tolower((unsigned char)src[i]);
    }

  dst[n] = '\0';
}

static bool line_match(struct grep_s *g, const char *l, size_t n)
{
  if (g->icase)
    {
      lower_into(g->lower, l, n);
      l = g->lower;
    }

  if (g->fixed != NULL)
    {
      return memmem(l, n, g->fixed, g->fixedn) != NULL;
    }
  else
    {
      const char *caps[2 * (MAXSUB + 1)];
      Subject subj;

      subj.begin_line = l;
      subj.begin      = l;
      subj.end        = l + n;
      return re1_5_recursiveloopprog(g->prog, &subj, caps,
                                     (g->prog->sub + 1) * 2, 0) > 0;
    }
}

static void add_match(struct grep_s *g, const char *path, int lineno,
                      const char *l, size_t n)
{
  mp_obj_t t[3];

  if (g->nfound >= g->max)
    {
      g->truncated = true;
      return;
    }

  g->nfound++;
  if (g->files)
    {
      mp_obj_list_append(g->list, mp_obj_new_str_from_cstr(path));
      return;
    }

  while (n > 0 && (l[n - 1] == '\r' || l[n - 1] == '\n'))
    {
      n--;
    }

  if (n > SHOWMAX)
    {
      n = SHOWMAX;
      while (n > 0 && ((uint8_t)l[n] & 0xc0) == 0x80)
        {
          n--;
        }
    }

  t[0] = mp_obj_new_str_from_cstr(path);
  t[1] = MP_OBJ_NEW_SMALL_INT(lineno);
  t[2] = mp_obj_new_str(l, n);
  mp_obj_list_append(g->list, mp_obj_new_tuple(3, t));
}

static void grep_file(struct grep_s *g, const char *path)
{
  char buf[1024];
  size_t ln = 0;              /* Bytes in g->line */
  size_t total = 0;
  int lineno = 1;
  bool first = true;
  int fd;

  if (g->glob != NULL)
    {
      const char *base = strrchr(path, '/');

      if (!name_match(g->glob, strlen(g->glob), base ? base + 1 : path) &&
          !name_match(g->glob, strlen(g->glob), path))
        {
          return;
        }
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return;
    }

  g->nfiles++;
  for (; ; )
    {
      ssize_t n = read(fd, buf, sizeof(buf));
      ssize_t i;

      if (n <= 0)
        {
          break;
        }

      if (first && memchr(buf, 0, n) != NULL)
        {
          break;                       /* Binary */
        }

      first = false;
      total += n;
      for (i = 0; i < n; i++)
        {
          char c = buf[i];

          if (c != '\n' && ln < LINEMAX)
            {
              g->line[ln++] = c;
              continue;
            }

          g->line[ln] = '\0';
          if (line_match(g, g->line, ln))
            {
              add_match(g, path, lineno, g->line, ln);
              if (g->files || g->truncated)
                {
                  goto done;
                }
            }

          if (c == '\n')
            {
              lineno++;
              ln = 0;
            }
          else
            {
              ln = 0;
              g->line[ln++] = c;       /* A long line: its next part */
            }
        }

      if (total > FILEMAX)
        {
          break;
        }
    }

  if (ln > 0)
    {
      g->line[ln] = '\0';
      if (line_match(g, g->line, ln))
        {
          add_match(g, path, lineno, g->line, ln);
        }
    }

done:
  close(fd);
}

static void grep_walk(struct grep_s *g, char *path, size_t len, int depth)
{
  DIR *dir;
  struct dirent *d;

  if (depth > MAXDEPTH || g->truncated)
    {
      return;
    }

  agent_check_interrupt();
  dir = opendir(path);
  if (dir == NULL)
    {
      return;
    }

  while ((d = readdir(dir)) != NULL && !g->truncated)
    {
      size_t nl;

      if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
        {
          continue;
        }

      nl = path_add(path, len, d->d_name);
      if (nl == 0)
        {
          continue;
        }

      if (is_dir_entry(path, d))
        {
          path[len] = '\0';
          if (!skip_dir(path, d->d_name))
            {
              path_add(path, len, d->d_name);
              grep_walk(g, path, nl, depth + 1);
            }
        }
      else
        {
          struct stat st;

          if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
            {
              grep_file(g, path);
              agent_check_interrupt();
            }
        }

      path[len] = '\0';
    }

  closedir(dir);
}

/* The pattern lowered for icase: letters, not what follows a backslash */

static char *lower_pattern(const char *p)
{
  size_t n = strlen(p);
  char *q = m_new(char, n + 1);
  size_t i;

  for (i = 0; i < n; i++)
    {
      if (p[i] == '\\' && i + 1 < n)
        {
          q[i] = p[i];
          q[i + 1] = p[i + 1];
          i++;
          continue;
        }

      q[i] = tolower((unsigned char)p[i]);
    }

  q[n] = '\0';
  return q;
}

static mp_obj_t agent_grep(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
  enum
  {
    A_PATTERN, A_PATH, A_GLOB, A_ICASE, A_FIXED, A_FILES, A_MAX
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_pattern, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_path,    MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_glob,    MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_icase,   MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_fixed,   MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_files,   MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_max,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 100} },
  };

  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  struct grep_s *g;
  const char *pat;
  const char *root;
  struct stat st;
  char *path;
  mp_obj_t t[3];

  mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
  pat  = mp_obj_str_get_str(a[A_PATTERN].u_obj);
  root = a[A_PATH].u_obj != mp_const_none ?
         mp_obj_str_get_str(a[A_PATH].u_obj) : ".";

  g = m_new0(struct grep_s, 1);
  g->icase = a[A_ICASE].u_bool;
  g->files = a[A_FILES].u_bool;
  g->max   = a[A_MAX].u_int > 0 ? a[A_MAX].u_int : 100;
  g->glob  = a[A_GLOB].u_obj != mp_const_none ?
             mp_obj_str_get_str(a[A_GLOB].u_obj) : NULL;
  g->list  = mp_obj_new_list(0, NULL);
  if (g->icase && a[A_FIXED].u_bool)
    {
      size_t n = strlen(pat);
      char *q = m_new(char, n + 1);

      lower_into(q, pat, n);
      pat = q;
    }
  else if (g->icase)
    {
      pat = lower_pattern(pat);
    }

  if (a[A_FIXED].u_bool)
    {
      g->fixed  = pat;
      g->fixedn = strlen(pat);
    }
  else
    {
      int size = re1_5_sizecode(pat);

      if (size < 0)
        {
          mp_raise_ValueError(MP_ERROR_TEXT("bad regular expression"));
        }

      g->prog = m_new_obj_var(ByteProg, insts, char, size);
      if (re1_5_compilecode(g->prog, pat) != 0)
        {
          mp_raise_ValueError(MP_ERROR_TEXT("bad regular expression"));
        }
    }

  if (stat(root, &st) < 0)
    {
      agent_raise(errno, "%s: %s", root, strerror(errno));
    }

  path = m_new(char, PATH_MAX);
  strlcpy(path, root, PATH_MAX);
  if (S_ISDIR(st.st_mode))
    {
      size_t len = strlen(path);

      while (len > 1 && path[len - 1] == '/')
        {
          path[--len] = '\0';
        }

      grep_walk(g, path, len, 0);
    }
  else
    {
      g->glob = NULL;                  /* A file named: searched */
      grep_file(g, path);
    }

  t[0] = g->list;
  t[1] = MP_OBJ_NEW_SMALL_INT(g->nfiles);
  t[2] = mp_obj_new_bool(g->truncated);
  return mp_obj_new_tuple(3, t);
}

MP_DEFINE_CONST_FUN_OBJ_KW(agent_grep_obj, 1, agent_grep);

/****************************************************************************
 * Private Functions: glob
 ****************************************************************************/

struct glob_s
{
  mp_obj_t list;
  int      n;
  int      max;
  bool     truncated;
  char    *path;
};

static void glob_add(struct glob_s *g, const char *path, struct stat *st)
{
  mp_obj_t t[3];

  if (g->n >= g->max)
    {
      g->truncated = true;
      return;
    }

  g->n++;
  t[0] = mp_obj_new_str_from_cstr(path);
  t[1] = mp_obj_new_int_from_ll((long long)st->st_mtime);
  t[2] = mp_obj_new_int_from_ll((long long)st->st_size);
  mp_obj_list_append(g->list, mp_obj_new_tuple(3, t));
}

/* Match the parts of pat from p on against path (len bytes) */

static void glob_walk(struct glob_s *g, size_t len, const char *p,
                      int depth)
{
  const char *e;
  size_t pn;
  DIR *dir;
  struct dirent *d;
  bool last;

  if (depth > MAXDEPTH || g->truncated)
    {
      return;
    }

  while (*p == '/')
    {
      p++;
    }

  e    = strchr(p, '/');
  pn   = e != NULL ? (size_t)(e - p) : strlen(p);
  last = e == NULL || e[1 + strspn(e + 1, "/")] == '\0';

  if (pn == 2 && p[0] == '*' && p[1] == '*')
    {
      /* **: this folder, and every folder below it */

      const char *rest = e != NULL ? e + 1 : "*";

      glob_walk(g, len, rest, depth + 1);
      agent_check_interrupt();
      dir = opendir(len > 0 ? g->path : ".");
      if (dir == NULL)
        {
          return;
        }

      while ((d = readdir(dir)) != NULL && !g->truncated)
        {
          size_t nl;

          if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            {
              continue;
            }

          nl = len > 0 ? path_add(g->path, len, d->d_name) :
                         (strlcpy(g->path, d->d_name, PATH_MAX),
                          strlen(d->d_name));
          if (nl > 0 && is_dir_entry(g->path, d))
            {
              g->path[len] = '\0';
              if (!skip_dir(len > 0 ? g->path : ".", d->d_name))
                {
                  if (len > 0)
                    {
                      path_add(g->path, len, d->d_name);
                    }
                  else
                    {
                      strlcpy(g->path, d->d_name, PATH_MAX);
                    }

                  glob_walk(g, nl, p, depth + 1);
                }
            }

          g->path[len] = '\0';
        }

      closedir(dir);
      return;
    }

  if (!has_magic(p, pn))
    {
      /* A plain name: no listing */

      char name[NAME_MAX + 1];
      struct stat st;
      size_t nl;

      if (pn > NAME_MAX)
        {
          return;
        }

      memcpy(name, p, pn);
      name[pn] = '\0';
      nl = len > 0 ? path_add(g->path, len, name) :
                     (strlcpy(g->path, name, PATH_MAX), pn);
      if (nl > 0 && stat(g->path, &st) == 0)
        {
          if (last)
            {
              if (S_ISREG(st.st_mode))
                {
                  glob_add(g, g->path, &st);
                }
            }
          else if (S_ISDIR(st.st_mode))
            {
              glob_walk(g, nl, e + 1, depth + 1);
            }
        }

      g->path[len] = '\0';
      return;
    }

  dir = opendir(len > 0 ? g->path : ".");
  if (dir == NULL)
    {
      return;
    }

  while ((d = readdir(dir)) != NULL && !g->truncated)
    {
      struct stat st;
      size_t nl;

      if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0 ||
          (d->d_name[0] == '.' && p[0] != '.') ||
          !name_match(p, pn, d->d_name))
        {
          continue;
        }

      nl = len > 0 ? path_add(g->path, len, d->d_name) :
                     (strlcpy(g->path, d->d_name, PATH_MAX),
                      strlen(d->d_name));
      if (nl > 0 && stat(g->path, &st) == 0)
        {
          if (last)
            {
              if (S_ISREG(st.st_mode))
                {
                  glob_add(g, g->path, &st);
                }
            }
          else if (S_ISDIR(st.st_mode))
            {
              glob_walk(g, nl, e + 1, depth + 1);
            }
        }

      g->path[len] = '\0';
    }

  closedir(dir);
}

static int glob_cmp(const void *a, const void *b)
{
  mp_obj_tuple_t *x = MP_OBJ_TO_PTR(*(const mp_obj_t *)a);
  mp_obj_tuple_t *y = MP_OBJ_TO_PTR(*(const mp_obj_t *)b);
  long long tx = mp_obj_get_int(x->items[1]);
  long long ty = mp_obj_get_int(y->items[1]);

  if (tx != ty)
    {
      return tx > ty ? -1 : 1;
    }

  return strcmp(mp_obj_str_get_str(x->items[0]),
                mp_obj_str_get_str(y->items[0]));
}

static mp_obj_t agent_glob(size_t n_args, const mp_obj_t *pos, mp_map_t *kw)
{
  enum
  {
    A_PATTERN, A_ROOT, A_MAX
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_pattern, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_root,    MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_max,     MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 500} },
  };

  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  struct glob_s g;
  const char *pat;
  size_t len = 0;
  size_t n;
  mp_obj_t *items;

  mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
  pat = mp_obj_str_get_str(a[A_PATTERN].u_obj);
  memset(&g, 0, sizeof(g));
  g.list = mp_obj_new_list(0, NULL);
  g.max  = a[A_MAX].u_int > 0 ? a[A_MAX].u_int : 500;
  g.path = m_new(char, PATH_MAX);
  g.path[0] = '\0';

  if (pat[0] == '/')
    {
      strcpy(g.path, "/");
      len = 1;
    }
  else if (a[A_ROOT].u_obj != mp_const_none)
    {
      strlcpy(g.path, mp_obj_str_get_str(a[A_ROOT].u_obj), PATH_MAX);
      len = strlen(g.path);
      while (len > 1 && g.path[len - 1] == '/')
        {
          g.path[--len] = '\0';
        }

      if (strcmp(g.path, ".") == 0)
        {
          len = 0;
          g.path[0] = '\0';
        }
    }

  glob_walk(&g, len, pat, 0);
  mp_obj_list_get(g.list, &n, &items);
  qsort(items, n, sizeof(mp_obj_t), glob_cmp);
  return g.list;
}

MP_DEFINE_CONST_FUN_OBJ_KW(agent_glob_obj, 1, agent_glob);

static mp_obj_t agent_fnmatch(mp_obj_t pat_in, mp_obj_t name_in)
{
  size_t pn;
  const char *pat = mp_obj_str_get_data(pat_in, &pn);

  return mp_obj_new_bool(name_match(pat, pn, mp_obj_str_get_str(name_in)));
}

MP_DEFINE_CONST_FUN_OBJ_2(agent_fnmatch_obj, agent_fnmatch);
