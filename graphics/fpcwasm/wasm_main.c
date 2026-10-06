/****************************************************************************
 * apps/graphics/fpcwasm/wasm_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The `wasm` command: WebAssembly apps on the FPC (docs/wasm.md).
 *
 *   wasm run [OPTIONS] APP [ARGS...]   run an app
 *   wasm info [APP]                    the runtime; an app's file, imports
 *   wasm list                          the apps installed
 *
 * APP is a .wasm or .aot file, an app's directory, or the name of an app
 * installed in CONFIG_FPCWASM_APPDIR (/mnt/sd0/apps/NAME/: app.wasm, an
 * app.aot compiled ahead of time for this CPU by wamrc, a manifest, an
 * icon, its data).  An .aot beside the .wasm is run instead when this
 * runtime's AOT version made it and it loads (made for this CPU); else
 * the .wasm is compiled as it loads (jit/: WebAssembly to Thumb-2), or
 * interpreted (WAMR's fast interpreter) where the compiler cannot (a
 * feature it does not have; the simulator).  -i, -j, -a or the
 * manifest's "run" line choose.
 *
 * The runtime is WAMR (apps/interpreters/wamr), with WASI (stdio, the
 * app's own directory as ".", clocks, random numbers) and the FPC's own
 * functions (wasm_fpc.c: a full-screen game, images, sprites, input,
 * sound).  Everything it allocates (the module, the linear memory, the
 * AOT code) is the PSRAM's (the user heap), given back when the app ends.
 *
 * The sandbox: an app touches nothing but its linear memory, the
 * functions it imports and WASI's view (its directory).  A trap (out of
 * bounds, unreachable, a division by zero, the wasm stack exhausted, a bad
 * pointer given to the host) ends it with a message and status 1; a game
 * that traps gives the display and the keyboard back first.  A callback
 * of a game that does not return within the watchdog's time (5 s), or
 * Ctrl-C, terminates it the same way (the interpreter and AOT code
 * compiled with --enable-multi-thread check for that at every loop and
 * call).
 ****************************************************************************/

#include "wasm_internal.h"
#include "wasm_jit.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#ifndef CONFIG_FPCWASM_APPDIR
#  define CONFIG_FPCWASM_APPDIR "/mnt/sd0/apps"
#endif

#define WASM_STACK_SIZE     (32 * 1024)   /* The wasm operand stack */
#define WASM_MAX_FILE       (8 * 1024 * 1024)
#define WASM_DEF_MAXMEM_KB  4096          /* The linear memory's limit */
#define WASM_WATCHDOG_S     5
#define WASM_MAX_DIRS       4

/* The watchdog's stack: it calls syslog() (and the simulator's threads
 * need room for its host's signal frames)
 */

#if CONFIG_PTHREAD_STACK_DEFAULT > 3072
#  define WASM_WATCHDOG_STACK CONFIG_PTHREAD_STACK_DEFAULT
#else
#  define WASM_WATCHDOG_STACK 3072
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

struct wasm_app_s g_wapp;

#ifdef CONFIG_ARCH_USE_TEXT_HEAP
extern const char *g_wamr_text_section;   /* (wamr's nuttx_platform.c) */
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wasm_opts_s
{
  const char *script;                     /* -s */
  bool interp;                            /* -i: interpreted */
  bool jit;                               /* -j: compiled here (no .aot) */
  bool aot;                               /* -a: the .aot or nothing */
  bool sram;                              /* --sram: AOT code in SRAM */
  bool verbose;                           /* -v */
  int maxmem_kb;                          /* -m */
  int watchdog_s;                         /* -w */
  const char *dirs[WASM_MAX_DIRS];        /* --dir */
  int ndirs;
};

/* An app as found: its file, its directory, its manifest's say */

struct wasm_found_s
{
  char path[PATH_MAX];                    /* The file to run */
  char alt[PATH_MAX];                     /* The .wasm if path is an .aot */
  char dir[PATH_MAX];                     /* Its directory, if an app's */
  char name[32];
  char kind[8];                           /* game, app, cli */
  char run[8];                            /* aot, jit, interp: preferred */
  int api;
  int width;                              /* A game with no _start */
  int height;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* One app at a time where programs share their statics (a flat build in
 * which the runtime is built in: the simulator); on the board each run of
 * the module has its own
 */

static pthread_mutex_t g_one = PTHREAD_MUTEX_INITIALIZER;
static bool g_inited;
static volatile bool g_wdstop;
static volatile bool g_sigint;

/****************************************************************************
 * Private Functions: files
 ****************************************************************************/

static bool is_dir(const char *path)
{
  struct stat st;

  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *path)
{
  struct stat st;

  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool ends_with(const char *s, const char *suffix)
{
  size_t n = strlen(s);
  size_t m = strlen(suffix);

  return n >= m && strcmp(s + n - m, suffix) == 0;
}

static FAR uint8_t *read_file(const char *path, uint32_t *size)
{
  FAR uint8_t *buf;
  struct stat st;
  ssize_t got;
  int fd;

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return NULL;
    }

  if (fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > WASM_MAX_FILE)
    {
      close(fd);
      errno = EFBIG;
      return NULL;
    }

  buf = malloc(st.st_size);
  if (buf == NULL)
    {
      close(fd);
      errno = ENOMEM;
      return NULL;
    }

  got = read(fd, buf, st.st_size);
  close(fd);
  if (got != st.st_size)
    {
      free(buf);
      errno = EIO;
      return NULL;
    }

  *size = (uint32_t)st.st_size;
  return buf;
}

/* The manifest: "key value" lines (name, kind, api, icon, color, width,
 * height); # comments
 */

static void read_manifest(FAR struct wasm_found_s *f)
{
  char path[PATH_MAX];
  char line[128];
  FILE *fp;

  snprintf(path, sizeof(path), "%s/manifest", f->dir);
  fp = fopen(path, "r");
  if (fp == NULL)
    {
      return;
    }

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      char *k = line;
      char *v;
      char *e;

      while (isspace((unsigned char)*k))
        {
          k++;
        }

      if (*k == '#' || *k == '\0')
        {
          continue;
        }

      for (v = k; *v != '\0' && !isspace((unsigned char)*v); v++);
      if (*v != '\0')
        {
          *v++ = '\0';
        }

      while (isspace((unsigned char)*v))
        {
          v++;
        }

      for (e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); e--);
      *e = '\0';

      if (strcmp(k, "name") == 0)
        {
          strlcpy(f->name, v, sizeof(f->name));
        }
      else if (strcmp(k, "kind") == 0)
        {
          strlcpy(f->kind, v, sizeof(f->kind));
        }
      else if (strcmp(k, "api") == 0)
        {
          f->api = atoi(v);
        }
      else if (strcmp(k, "run") == 0)
        {
          strlcpy(f->run, v, sizeof(f->run));
        }
      else if (strcmp(k, "width") == 0)
        {
          f->width = atoi(v);
        }
      else if (strcmp(k, "height") == 0)
        {
          f->height = atoi(v);
        }
    }

  fclose(fp);
}

/* path without its extension, then ext */

static void with_ext(char *out, size_t size, const char *path,
                     const char *ext)
{
  const char *dot = strrchr(path, '.');
  const char *slash = strrchr(path, '/');
  size_t n = dot != NULL && (slash == NULL || dot > slash) ?
             (size_t)(dot - path) : strlen(path);

  snprintf(out, size, "%.*s%s", (int)n, path, ext);
}

/* Find APP: a file, a directory, or an installed app's name */

static int find_app(const char *app, FAR const struct wasm_opts_s *o,
                    FAR struct wasm_found_s *f)
{
  char wasm[PATH_MAX];
  char aot[PATH_MAX];
  const char *base;

  memset(f, 0, sizeof(*f));
  strlcpy(f->kind, "cli", sizeof(f->kind));

  if (is_dir(app))
    {
      strlcpy(f->dir, app, sizeof(f->dir));
    }
  else if (strcmp(app, "app.wasm") == 0 || strcmp(app, "app.aot") == 0 ||
           ends_with(app, "/app.wasm") || ends_with(app, "/app.aot"))
    {
      /* An app's file: the app (its directory, its manifest) */

      const char *slash = strrchr(app, '/');

      if (slash == NULL)
        {
          strlcpy(f->dir, ".", sizeof(f->dir));
        }
      else
        {
          snprintf(f->dir, sizeof(f->dir), "%.*s",
                   slash == app ? 1 : (int)(slash - app), app);
        }

    }
  else if (strchr(app, '/') == NULL && !ends_with(app, ".wasm") &&
           !ends_with(app, ".aot"))
    {
      snprintf(f->dir, sizeof(f->dir), "%s/%s", CONFIG_FPCWASM_APPDIR, app);
      if (!is_dir(f->dir))
        {
          fprintf(stderr, "wasm: no app %s (in %s)\n", app,
                  CONFIG_FPCWASM_APPDIR);
          return -ENOENT;
        }
    }

  if (f->dir[0] != '\0')
    {
      size_t n = strlen(f->dir);

      while (n > 1 && f->dir[n - 1] == '/')
        {
          f->dir[--n] = '\0';
        }

      strlcpy(f->kind, "game", sizeof(f->kind));
      read_manifest(f);
      snprintf(wasm, sizeof(wasm), "%s/app.wasm", f->dir);
      snprintf(aot, sizeof(aot), "%s/app.aot", f->dir);
      base = strrchr(f->dir, '/');
      base = base != NULL ? base + 1 : f->dir;
    }
  else
    {
      if (ends_with(app, ".aot"))
        {
          strlcpy(aot, app, sizeof(aot));
          with_ext(wasm, sizeof(wasm), app, ".wasm");
        }
      else
        {
          strlcpy(wasm, app, sizeof(wasm));
          with_ext(aot, sizeof(aot), app, ".aot");
        }

      base = strrchr(app, '/');
      base = base != NULL ? base + 1 : app;
    }

  if (f->name[0] == '\0')
    {
      with_ext(f->name, sizeof(f->name), base, "");
    }

  if (!o->interp && !o->jit && !o->aot &&
      (strcmp(f->run, "jit") == 0 || strcmp(f->run, "interp") == 0))
    {
      /* The manifest prefers the .wasm (and how to run it) */
    }
  else if (!o->interp && !o->jit && is_file(aot))
    {
      strlcpy(f->path, aot, sizeof(f->path));
      if (!o->aot && is_file(wasm))
        {
          strlcpy(f->alt, wasm, sizeof(f->alt));
        }
    }
  else if (!o->aot && is_file(wasm))
    {
      strlcpy(f->path, wasm, sizeof(f->path));
    }
  else
    {
      fprintf(stderr, "wasm: %s: no %s\n", app, o->aot ? aot : wasm);
      return -ENOENT;
    }

  return OK;
}

/****************************************************************************
 * Private Functions: the runtime
 ****************************************************************************/

static bool runtime_init(void)
{
  RuntimeInitArgs args;

  if (g_inited)
    {
      return true;
    }

  memset(&args, 0, sizeof(args));
  args.mem_alloc_type = Alloc_With_System_Allocator;   /* The PSRAM */
  args.max_thread_num = 1;
  if (!wasm_runtime_full_init(&args))
    {
      fprintf(stderr, "wasm: the runtime did not start\n");
      return false;
    }

  wasm_runtime_set_log_level(WASM_LOG_LEVEL_ERROR);
  if (!wasm_fpc_register())
    {
      fprintf(stderr, "wasm: the fpc functions did not register\n");
      wasm_runtime_destroy();
      return false;
    }

  g_inited = true;
  return true;
}

static void runtime_end(void)
{
  if (g_inited)
    {
      wasm_runtime_destroy();
      g_inited = false;
    }
}

/* A file this runtime can run: a .wasm, or an .aot of its AOT version */

static int package_check(const uint8_t *buf, uint32_t size, bool *aot)
{
  package_type_t t = wasm_runtime_get_file_package_type(buf, size);

  if (t == Wasm_Module_Bytecode)
    {
      *aot = false;
      return OK;
    }

  if (t == Wasm_Module_AoT)
    {
      uint32_t v = wasm_runtime_get_file_package_version(buf, size);
      uint32_t want = wasm_runtime_get_current_package_version(t);

      *aot = true;
      if (v != want)
        {
          fprintf(stderr, "wasm: AOT file version %u, this runtime's is %u "
                  "(compile it again with this runtime's wamrc)\n",
                  (unsigned)v, (unsigned)want);
          return -ENOEXEC;
        }

      return OK;
    }

  return -ENOEXEC;
}

/* The imports this runtime does not have (the API is versioned: a newer
 * app may want a function an older runtime lacks): named, and refused
 */

static bool imports_linked(wasm_module_t module, const char *name)
{
  int32_t n = wasm_runtime_get_import_count(module);
  bool ok = true;
  int32_t i;

  for (i = 0; i < n; i++)
    {
      wasm_import_t t;

      wasm_runtime_get_import_type(module, i, &t);
      if (t.kind == WASM_IMPORT_EXPORT_KIND_FUNC && !t.linked)
        {
          fprintf(stderr, "wasm: %s needs %s.%s, which this runtime "
                  "(fpc API v%d) does not have\n", name, t.module_name,
                  t.name, WASM_API_VERSION);
          ok = false;
        }
    }

  return ok;
}

/* The watchdog: a game's callback that does not return in time (a loop
 * that never ends) is terminated, as a trap
 */

static FAR void *watchdog(FAR void *arg)
{
  int limit = (int)(intptr_t)arg * 10;
  uint32_t last = g_wapp.beat;
  int still = 0;

  while (!g_wdstop)
    {
      usleep(100 * 1000);
      if (g_sigint && !g_wapp.killed)
        {
          g_wapp.killed = true;
          wasm_runtime_terminate(g_wapp.inst);
        }

      if (limit > 0 && g_wapp.in_callback && g_wapp.beat == last)
        {
          if (++still >= limit && !g_wapp.killed)
            {
              syslog(LOG_WARNING, "wasm %s: a frame took over %d s: "
                     "terminated\n", g_wapp.name, limit / 10);
              g_wapp.killed = true;
              wasm_runtime_terminate(g_wapp.inst);
            }
        }
      else
        {
          still = 0;
          last  = g_wapp.beat;
        }
    }

  return NULL;
}

/* Ctrl-C: the watchdog terminates it (not here: a signal handler must not
 * take the runtime's locks)
 */

static void sigint_handler(int signo)
{
  g_sigint = true;
}

/* Run what find_app() found; the exit status */

static int run_found(FAR struct wasm_found_s *f,
                     FAR const struct wasm_opts_s *o, int argc,
                     FAR char **argv)
{
  char err[128];
  char cwd[PATH_MAX];
  char map[PATH_MAX + 4];
  const char *maps[1];
  const char *dirs[WASM_MAX_DIRS + 1];
  const char *wargv[16];
  int ndirs;
  InstantiationArgs ia;
  wasm_module_t module = NULL;
  wasm_function_inst_t start;
  struct sigaction sa;
  struct sigaction oldsa;
  pthread_t wd;
  bool wdrun = false;
  FAR uint8_t *buf;
  uint32_t size;
  uint32_t t0;
  uint32_t t1;
  bool aot = false;
  bool jit = false;
  int status = 1;
  int nargs;
  int i;

  /* Load: the .aot if it is right for this runtime and CPU, else the
   * .wasm beside it
   */

  t0 = (uint32_t)clock();
  for (; ; )
    {
      buf = read_file(f->path, &size);
      if (buf == NULL)
        {
          fprintf(stderr, "wasm: %s: %s\n", f->path, strerror(errno));
          return 1;
        }

      if (package_check(buf, size, &aot) == OK)
        {
#ifdef CONFIG_ARCH_USE_TEXT_HEAP
          g_wamr_text_section = o->sram ? ".time_critical.wasm" : ".text";
#endif
          module = wasm_runtime_load(buf, size, err, sizeof(err));
          if (module != NULL)
            {
              break;
            }

          fprintf(stderr, "wasm: %s: %s\n", f->path, err);
        }
      else if (!aot)
        {
          fprintf(stderr, "wasm: %s: not a WebAssembly module\n", f->path);
        }

      free(buf);
      if (f->alt[0] == '\0')
        {
          return 1;
        }

      fprintf(stderr, "wasm: interpreting %s instead\n", f->alt);
      strlcpy(f->path, f->alt, sizeof(f->path));
      f->alt[0] = '\0';
    }

  t1 = (uint32_t)clock();
  if (!imports_linked(module, f->name))
    {
      goto out_unload;
    }

  /* WASI: the app's directory (or the shell's) as ".", --dir's as they
   * are; the arguments after APP
   */

  ndirs = 0;
  if (f->dir[0] != '\0' && strcmp(f->kind, "cli") != 0)
    {
      snprintf(map, sizeof(map), ".::%s", f->dir);
    }
  else
    {
      if (getcwd(cwd, sizeof(cwd)) == NULL)
        {
          strlcpy(cwd, "/", sizeof(cwd));
        }

      snprintf(map, sizeof(map), ".::%s", cwd);
      dirs[ndirs++] = cwd;                /* (absolute names too) */
    }

  maps[0] = map;
  for (i = 0; i < o->ndirs; i++)
    {
      dirs[ndirs++] = o->dirs[i];
    }

  wargv[0] = f->name;
  nargs = 1;
  for (i = 0; i < argc && nargs < 16; i++)
    {
      wargv[nargs++] = argv[i];
    }

  wasm_runtime_set_wasi_args(module, dirs, ndirs, maps, 1, NULL, 0,
                             (char **)wargv, nargs);

  memset(&ia, 0, sizeof(ia));
  ia.default_stack_size     = WASM_STACK_SIZE;
  ia.host_managed_heap_size = 0;          /* (wasi-libc has its malloc) */
  ia.max_memory_pages       = (o->maxmem_kb + 63) / 64;
  g_wapp.inst = wasm_runtime_instantiate_ex(module, &ia, err, sizeof(err));
  if (g_wapp.inst == NULL)
    {
      fprintf(stderr, "wasm: %s: %s\n", f->name, err);
      goto out_unload;
    }

  /* Compiled here, unless asked to interpret it (or it cannot be) */

  if (!aot && !o->interp && wasm_jit_supported() &&
      (o->jit || strcmp(f->run, "interp") != 0))
    {
      jit = wasm_jit_attach(g_wapp.inst, (o->sram ? WASM_JIT_SRAM : 0) |
                            (o->verbose ? WASM_JIT_VERBOSE : 0), err,
                            sizeof(err));
      if (!jit)
        {
          fprintf(stderr, "wasm: %s: not compiled (%s): interpreting\n",
                  f->name, err);
        }
    }

  g_wapp.env = wasm_runtime_create_exec_env(g_wapp.inst, WASM_STACK_SIZE);
  if (g_wapp.env == NULL)
    {
      fprintf(stderr, "wasm: %s: no memory for its stack\n", f->name);
      goto out_deinst;
    }

  strlcpy(g_wapp.name, f->name, sizeof(g_wapp.name));
  strlcpy(g_wapp.dir, f->dir, sizeof(g_wapp.dir));
  g_wapp.script = o->script;
  g_wapp.game_w = f->width;
  g_wapp.game_h = f->height;

  if (o->verbose)
    {
      wasm_memory_inst_t mem = wasm_runtime_get_default_memory(g_wapp.inst);

      fprintf(stderr, "wasm: %s: %s, %u bytes, loaded in %u ms; memory "
              "%u of at most %u pages\n", f->name,
              aot ? "AOT" : jit ? "JIT" : "interpreted", (unsigned)size,
              (unsigned)((t1 - t0) * 1000 / CLOCKS_PER_SEC),
              mem ? (unsigned)wasm_memory_get_cur_page_count(mem) : 0,
              mem ? (unsigned)wasm_memory_get_max_page_count(mem) : 0);
    }

  syslog(LOG_INFO, "wasm %s: %s (%s)\n", f->name, f->path,
         aot ? "AOT" : jit ? "JIT" : "interpreted");

  /* Ctrl-C and the watchdog end it as a trap would */

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigint_handler;
  sigaction(SIGINT, &sa, &oldsa);
  g_wdstop = false;
  g_sigint = false;
    {
      struct sched_param sp;
      pthread_attr_t attr;

      /* Above the app (which may spin, and FIFO does not share a priority) */

      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, WASM_WATCHDOG_STACK);
      sp.sched_priority = 180;
      pthread_attr_setschedparam(&attr, &sp);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
      wdrun = pthread_create(&wd, &attr, watchdog,
                             (FAR void *)(intptr_t)o->watchdog_s) == 0;
      if (wdrun)
        {
          pthread_setname_np(wd, "wasm watchdog");
        }
    }

  /* A command (_start: WASI's main), or a game with no main of its own
   * (fpc_draw exported, _initialize if it is a reactor)
   */

  start = wasm_runtime_lookup_function(g_wapp.inst, "_start");
  if (start != NULL)
    {
      if (wasm_runtime_call_wasm(g_wapp.env, start, 0, NULL))
        {
          status = 0;
        }
    }
  else if (wasm_runtime_lookup_function(g_wapp.inst, "fpc_draw") != NULL)
    {
      wasm_function_inst_t ini =
        wasm_runtime_lookup_function(g_wapp.inst, "_initialize");

      if (ini == NULL || wasm_runtime_call_wasm(g_wapp.env, ini, 0, NULL))
        {
          int ret = wasm_fpc_run_game(g_wapp.game_w, g_wapp.game_h);

          if (wasm_runtime_get_exception(g_wapp.inst) == NULL)
            {
              status = ret < 0 ? 1 : 0;
            }
        }
    }
  else
    {
      fprintf(stderr, "wasm: %s exports neither _start nor fpc_draw\n",
              f->name);
    }

  if (status != 0)
    {
      const char *ex = wasm_runtime_get_exception(g_wapp.inst);

      if (ex != NULL && strstr(ex, "wasi proc exit") != NULL)
        {
          status = (int)wasm_runtime_get_wasi_exit_code(g_wapp.inst);
        }
      else if (ex != NULL)
        {
          fprintf(stderr, "wasm: %s: %s%s\n", f->name, ex,
                  g_wapp.killed ? " (it did not return: watchdog or "
                  "Ctrl-C)" : "");
          syslog(LOG_WARNING, "wasm %s: trap: %s\n", f->name, ex);
        }
    }
  else
    {
      status = (int)wasm_runtime_get_wasi_exit_code(g_wapp.inst);
    }

  g_wdstop = true;
  if (wdrun)
    {
      pthread_join(wd, NULL);
    }

  sigaction(SIGINT, &oldsa, NULL);
  wasm_runtime_destroy_exec_env(g_wapp.env);
  g_wapp.env = NULL;

out_deinst:
  wasm_jit_detach(g_wapp.inst);
  wasm_runtime_deinstantiate(g_wapp.inst);
  g_wapp.inst = NULL;

out_unload:
  wasm_runtime_unload(module);
  free(buf);
  return status;
}

/****************************************************************************
 * Private Functions: the commands
 ****************************************************************************/

static bool one_take(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 10;
  if (pthread_mutex_timedlock(&g_one, &ts) != 0)
    {
      fprintf(stderr, "wasm: another app is running\n");
      return false;
    }

  return true;
}

static int usage(void)
{
  fprintf(stderr,
          "usage: wasm run [-i|-j|-a] [-s SCRIPT] [-m KB] [-w S] [--dir DIR] "
          "[--sram] [-v] APP [ARGS...]\n"
          "       wasm info [APP]\n"
          "       wasm list\n"
          "APP: a .wasm or .aot file, an app's directory, or an app's name "
          "(%s/NAME)\n"
          "  -i  interpret the .wasm (no .aot)   -a  the .aot only\n"
          "  -j  compile the .wasm here (JIT, no .aot)\n"
          "  -s  a game's input script (fpcgame)  -m  linear memory limit, "
          "KB (%d)\n"
          "  -w  watchdog: a frame's limit, s (%d; 0 off)\n"
          "  --dir DIR  WASI access to DIR      --sram  AOT/JIT code in SRAM\n",
          CONFIG_FPCWASM_APPDIR, WASM_DEF_MAXMEM_KB, WASM_WATCHDOG_S);
  return 2;
}

static int cmd_run(int argc, FAR char **argv)
{
  struct wasm_opts_s o;
  struct wasm_found_s f;
  int status;
  int i;

  memset(&o, 0, sizeof(o));
  o.maxmem_kb  = WASM_DEF_MAXMEM_KB;
  o.watchdog_s = WASM_WATCHDOG_S;

  for (i = 0; i < argc && argv[i][0] == '-'; i++)
    {
      const char *a = argv[i];

      if (strcmp(a, "-i") == 0)
        {
          o.interp = true;
        }
      else if (strcmp(a, "-j") == 0)
        {
          o.jit = true;
        }
      else if (strcmp(a, "-a") == 0)
        {
          o.aot = true;
        }
      else if (strcmp(a, "-v") == 0)
        {
          o.verbose = true;
        }
      else if (strcmp(a, "--sram") == 0)
        {
          o.sram = true;
        }
      else if (strcmp(a, "-s") == 0 && i + 1 < argc)
        {
          o.script = argv[++i];
        }
      else if (strcmp(a, "-m") == 0 && i + 1 < argc)
        {
          o.maxmem_kb = atoi(argv[++i]);
          o.maxmem_kb = o.maxmem_kb < 64 ? 64 : o.maxmem_kb;
        }
      else if (strcmp(a, "-w") == 0 && i + 1 < argc)
        {
          o.watchdog_s = atoi(argv[++i]);
        }
      else if (strcmp(a, "--dir") == 0 && i + 1 < argc &&
               o.ndirs < WASM_MAX_DIRS)
        {
          o.dirs[o.ndirs++] = argv[++i];
        }
      else
        {
          return usage();
        }
    }

  if (i >= argc || o.interp + o.aot + o.jit > 1)
    {
      return usage();
    }

  if (find_app(argv[i], &o, &f) < 0)
    {
      return 1;
    }

  if (f.api > WASM_API_VERSION)
    {
      fprintf(stderr, "wasm: %s wants fpc API v%d; this runtime has v%d\n",
              f.name, f.api, WASM_API_VERSION);
      return 1;
    }

  if (!one_take())
    {
      return 1;
    }

  if (!runtime_init())
    {
      pthread_mutex_unlock(&g_one);
      return 1;
    }

  memset(&g_wapp, 0, sizeof(g_wapp));
  status = run_found(&f, &o, argc - i - 1, argv + i + 1);
  runtime_end();
  pthread_mutex_unlock(&g_one);
  return status;
}

static int info_locked(int argc, FAR char **argv)
{
  struct wasm_opts_s o;
  struct wasm_found_s f;
  char err[128];
  FAR uint8_t *buf;
  uint32_t size;
  wasm_module_t m;
  bool aot;
  int32_t n;
  int32_t i;
  uint32_t major;
  uint32_t minor;
  uint32_t patch;

  if (!runtime_init())
    {
      return 1;
    }

  wasm_runtime_get_version(&major, &minor, &patch);
  printf("WAMR %u.%u.%u: fast interpreter, %sAOT (version %u, %s), "
         "WASI; fpc API v%d\n", (unsigned)major, (unsigned)minor,
         (unsigned)patch, wasm_jit_supported() ? "JIT (Thumb-2), " : "",
         (unsigned)wasm_runtime_get_current_package_version(Wasm_Module_AoT),
#if defined(CONFIG_ARCH_SIM)
         "x86_64",
#else
         "thumbv8m.main cortex-m33 gnueabihf",
#endif
         WASM_API_VERSION);
  printf("apps: %s\n", CONFIG_FPCWASM_APPDIR);
  if (argc < 1)
    {
      runtime_end();
      return 0;
    }

  memset(&o, 0, sizeof(o));
  if (find_app(argv[0], &o, &f) < 0)
    {
      runtime_end();
      return 1;
    }

  printf("%s: %s (kind %s, api %d)\n", f.name, f.path, f.kind, f.api);
  buf = read_file(f.path, &size);
  if (buf == NULL)
    {
      runtime_end();
      return 1;
    }

  if (package_check(buf, size, &aot) < 0)
    {
      free(buf);
      runtime_end();
      return 1;
    }

  m = wasm_runtime_load(buf, size, err, sizeof(err));
  if (m == NULL)
    {
      printf("  does not load: %s\n", err);
      free(buf);
      runtime_end();
      return 1;
    }

  printf("  %s, %u bytes\n", aot ? "AOT" : "bytecode", (unsigned)size);
  n = wasm_runtime_get_import_count(m);
  for (i = 0; i < n; i++)
    {
      wasm_import_t t;

      wasm_runtime_get_import_type(m, i, &t);
      printf("  import %s.%s%s\n", t.module_name, t.name,
             t.kind == WASM_IMPORT_EXPORT_KIND_FUNC && !t.linked ?
             " (missing)" : "");
    }

  n = wasm_runtime_get_export_count(m);
  for (i = 0; i < n; i++)
    {
      wasm_export_t t;

      wasm_runtime_get_export_type(m, i, &t);
      printf("  export %s\n", t.name);
    }

  wasm_runtime_unload(m);
  free(buf);
  runtime_end();
  return 0;
}

static int cmd_info(int argc, FAR char **argv)
{
  int ret;

  if (!one_take())
    {
      return 1;
    }

  ret = info_locked(argc, argv);
  pthread_mutex_unlock(&g_one);
  return ret;
}

static int cmd_list(void)
{
  DIR *d = opendir(CONFIG_FPCWASM_APPDIR);
  struct dirent *e;

  if (d == NULL)
    {
      printf("no apps (%s)\n", CONFIG_FPCWASM_APPDIR);
      return 0;
    }

  while ((e = readdir(d)) != NULL)
    {
      struct wasm_opts_s o;
      struct wasm_found_s f;
      char dir[PATH_MAX];

      if (e->d_name[0] == '.')
        {
          continue;
        }

      snprintf(dir, sizeof(dir), "%s/%s", CONFIG_FPCWASM_APPDIR, e->d_name);
      memset(&o, 0, sizeof(o));
      if (is_dir(dir) && find_app(dir, &o, &f) == OK)
        {
          printf("%-12s %-5s %s%s\n", e->d_name, f.kind, f.name,
                 ends_with(f.path, ".aot") ? " (AOT)" : "");
        }
    }

  closedir(d);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  if (argc >= 2 && strcmp(argv[1], "run") == 0)
    {
      return cmd_run(argc - 2, argv + 2);
    }

  if (argc >= 2 && strcmp(argv[1], "info") == 0)
    {
      return cmd_info(argc - 2, argv + 2);
    }

  if (argc >= 2 && strcmp(argv[1], "list") == 0)
    {
      return cmd_list();
    }

  return usage();
}
