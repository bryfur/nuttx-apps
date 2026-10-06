/****************************************************************************
 * apps/interpreters/micropython/agent_proc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * _agent.Proc: an NSH command line run with its output captured.
 *
 *   p = Proc(cmd)                 NSH (the sh builtin) runs cmd: its
 *                                 stdout and stderr into a pipe, its
 *                                 stdin /dev/null; the environment (and
 *                                 so the current directory) this task's,
 *                                 less secrets (*API_KEY*, *TOKEN* ...)
 *   p.read(wait_ms=-1)            the output so far: bytes (b"" when
 *                                 wait_ms passed with nothing), None at
 *                                 the end (everything that writes to it
 *                                 has closed it)
 *   p.wait(wait_ms=2000)          the exit status, None if still running
 *   p.kill()                      the shell deleted (and the read end
 *                                 closed: what it started gets EPIPE)
 *   p.pid, p.close()
 *
 * NuttX keeps no exit status for a task nobody waits for (no
 * CONFIG_SCHED_HAVE_PARENT), and waitpid() would hold the interpreter, so
 * the shell is a task of our own that runs NSH's session (as sh does) and
 * leaves its result in a block the object owns.  NSH says 0 or 1 (a
 * command's failure), not the program's own code.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <nuttx/lib/builtin.h>

/* NSH's own session, run as sh does it (nsh_system.c) but returning */

#include "../../nshlib/nsh.h"
#include "../../nshlib/nsh_console.h"

#include "py/runtime.h"

#include "modagent.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The shell's stack: sh's own (CONFIG_SYSTEM_NSH_STACKSIZE, 2 KB) is for
 * system()'s one-liners; a session as the Terminal's (8 KB)
 */

#ifdef CONFIG_FPCDESK_TERM_STACKSIZE
#  define SH_STACKSIZE   CONFIG_FPCDESK_TERM_STACKSIZE
#else
#  define SH_STACKSIZE   8192
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Written by the shell's task (C heap: outlives a collected object until
 * the task has gone)
 */

struct proc_blk_s
{
  volatile int done;
  volatile int status;
};

typedef struct
{
  mp_obj_base_t base;
  pid_t pid;
  int   fd;                     /* The pipe's read end, -1 closed */
  struct proc_blk_s *blk;
} proc_obj_t;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The shell's task: argv = sh -c CMD BLOCK.  NSH's session as sh's main
 * (nsh_system_ctty()) runs it, except that sh's ends with exit(), which
 * leaves no status (no atexit functions in this configuration): here the
 * session returns, its status goes into the block, then the task ends.
 */

static int proc_entry(int argc, char *argv[])
{
  struct console_stdio_s *pstate;
  struct proc_blk_s *blk;
  char *args[4];
  int ret = 1;

  if (argc < 4)
    {
      return 1;
    }

  blk = (struct proc_blk_s *)(uintptr_t)strtoul(argv[3], NULL, 16);
  args[0] = argv[0];
  args[1] = argv[1];
  args[2] = argv[2];
  args[3] = NULL;
  pstate = nsh_newconsole(false);
  if (pstate != NULL)
    {
      ret = nsh_session(pstate, NSH_LOGIN_NONE, 3, args);
      pstate->cn_vtbl.release(&pstate->cn_vtbl);
    }

  fflush(stdout);
  fflush(stderr);
  blk->status = ret;
  blk->done   = 1;
  return ret;
}

static bool proc_alive(proc_obj_t *o)
{
  return o->pid > 0 && !o->blk->done && kill(o->pid, 0) == 0;
}

static void proc_end(proc_obj_t *o, bool force)
{
  if (o->fd >= 0)
    {
      close(o->fd);
      o->fd = -1;
    }

  if (o->blk != NULL)
    {
      if (proc_alive(o))
        {
          if (!force)
            {
              return;                  /* Still running: kept */
            }

          task_delete(o->pid);
        }

      free(o->blk);
      o->blk = NULL;
    }
}

/* The environment for the shell: this task's, less the secrets (API keys,
 * tokens: the agent's key must not reach a command's output, the screen
 * and the model)
 */

static bool secret(const char *e)
{
  const char *eq = strchr(e, '=');
  size_t n = eq != NULL ? (size_t)(eq - e) : strlen(e);
  static const char *const words[] =
  {
    "API_KEY", "TOKEN", "SECRET", "PASSWORD"
  };

  for (size_t w = 0; w < sizeof(words) / sizeof(words[0]); w++)
    {
      size_t k = strlen(words[w]);

      for (size_t i = 0; i + k <= n; i++)
        {
          if (strncasecmp(e + i, words[w], k) == 0)
            {
              return true;
            }
        }
    }

  return false;
}

static char **proc_env(void)
{
  char **env = environ;
  char **out;
  size_t n = 0;
  size_t k = 0;

  while (env != NULL && env[n] != NULL)
    {
      n++;
    }

  out = m_new(char *, n + 1);
  for (size_t i = 0; i < n; i++)
    {
      if (!secret(env[i]))
        {
          out[k++] = env[i];
        }
    }

  out[k] = NULL;
  return out;
}

/* Proc(cmd) */

static mp_obj_t proc_make_new(const mp_obj_type_t *type, size_t n_args,
                              size_t n_kw, const mp_obj_t *args)
{
  const struct builtin_s *b;
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  struct sched_param sp;
  const char *cmd;
  proc_obj_t *o;
  char addr[24];
  char *argv[4];
  int fds[2];
  int idx;
  int ret;

  mp_arg_check_num(n_args, n_kw, 1, 1, false);
  cmd = mp_obj_str_get_str(args[0]);
  idx = builtin_isavail("sh");
  b = idx >= 0 ? builtin_for_index(idx) : NULL;
  if (b == NULL)
    {
      agent_raise(ENOENT, "no sh builtin");
    }

  o = mp_obj_malloc_with_finaliser(proc_obj_t, type);
  o->pid = -1;
  o->fd  = -1;
  o->blk = calloc(1, sizeof(*o->blk));
  if (o->blk == NULL)
    {
      mp_raise_OSError(ENOMEM);
    }

  if (pipe(fds) < 0)
    {
      ret = errno;
      free(o->blk);
      o->blk = NULL;
      agent_raise(ret, "pipe: %s", strerror(ret));
    }

  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  posix_spawn_file_actions_addclose(&fa, fds[0]);
  posix_spawn_file_actions_addclose(&fa, fds[1]);

  posix_spawnattr_init(&attr);
  sp.sched_priority = b->priority;
  posix_spawnattr_setschedparam(&attr, &sp);
  posix_spawnattr_setstacksize(&attr, b->stacksize > SH_STACKSIZE ?
                               b->stacksize : SH_STACKSIZE);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSCHEDPARAM);

  snprintf(addr, sizeof(addr), "%lx", (unsigned long)(uintptr_t)o->blk);
  /* task_spawn() makes argv[0] the task's name: these follow it */

  argv[0] = "-c";
  argv[1] = (char *)cmd;
  argv[2] = addr;
  argv[3] = NULL;
  ret = task_spawn("sh", proc_entry, &fa, &attr, argv, proc_env());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  close(fds[1]);
  if (ret < 0)
    {
      close(fds[0]);
      free(o->blk);
      o->blk = NULL;
      agent_raise(-ret, "cannot start sh: %s", strerror(-ret));
    }

  o->pid = ret;
  o->fd  = fds[0];
  return MP_OBJ_FROM_PTR(o);
}

/* read(wait_ms=-1) */

static mp_obj_t proc_read(size_t n_args, const mp_obj_t *args)
{
  proc_obj_t *o = MP_OBJ_TO_PTR(args[0]);
  int wait_ms = n_args > 1 && args[1] != mp_const_none ?
                mp_obj_get_int(args[1]) : -1;
  uint8_t buf[1024];
  ssize_t n;

  if (o->fd < 0)
    {
      return mp_const_none;
    }

  if (agent_wait_fd(o->fd, POLLIN, wait_ms) == 0)
    {
      return mp_const_empty_bytes;
    }

  n = read(o->fd, buf, sizeof(buf));
  if (n < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
        {
          return mp_const_empty_bytes;
        }

      n = 0;
    }

  if (n == 0)
    {
      close(o->fd);
      o->fd = -1;
      return mp_const_none;
    }

  return mp_obj_new_bytes(buf, n);
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(proc_read_obj, 1, 2, proc_read);

/* wait(wait_ms=2000) */

static mp_obj_t proc_wait(size_t n_args, const mp_obj_t *args)
{
  proc_obj_t *o = MP_OBJ_TO_PTR(args[0]);
  int wait_ms = n_args > 1 && args[1] != mp_const_none ?
                mp_obj_get_int(args[1]) : 2000;
  uint32_t t0 = agent_ms();

  if (o->blk == NULL)
    {
      return mp_const_none;
    }

  for (; ; )
    {
      if (o->blk->done)
        {
          return MP_OBJ_NEW_SMALL_INT(o->blk->status);
        }

      if (kill(o->pid, 0) < 0)
        {
          /* Gone without a word: NSH exits at a command's failure (its
           * errexit option)
           */

          return MP_OBJ_NEW_SMALL_INT(1);
        }

      if (wait_ms >= 0 && (int)(agent_ms() - t0) >= wait_ms)
        {
          return mp_const_none;
        }

      usleep(10000);
      agent_check_interrupt();
    }
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(proc_wait_obj, 1, 2, proc_wait);

static mp_obj_t proc_kill(mp_obj_t self)
{
  proc_end(MP_OBJ_TO_PTR(self), true);
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(proc_kill_obj, proc_kill);

/* close(): the pipe closed; a shell still running is ended */

static mp_obj_t proc_close(mp_obj_t self)
{
  proc_end(MP_OBJ_TO_PTR(self), true);
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(proc_close_obj, proc_close);

static void proc_attr(mp_obj_t self, qstr attr, mp_obj_t *dest)
{
  proc_obj_t *o = MP_OBJ_TO_PTR(self);

  if (dest[0] != MP_OBJ_NULL)
    {
      return;
    }

  if (attr == MP_QSTR_pid)
    {
      dest[0] = MP_OBJ_NEW_SMALL_INT(o->pid);
    }
  else if (attr == MP_QSTR_running)
    {
      dest[0] = mp_obj_new_bool(o->blk != NULL && proc_alive(o));
    }
  else
    {
      dest[1] = MP_OBJ_SENTINEL;
    }
}

static const mp_rom_map_elem_t proc_locals[] =
{
  { MP_ROM_QSTR(MP_QSTR_read),    MP_ROM_PTR(&proc_read_obj) },
  { MP_ROM_QSTR(MP_QSTR_wait),    MP_ROM_PTR(&proc_wait_obj) },
  { MP_ROM_QSTR(MP_QSTR_kill),    MP_ROM_PTR(&proc_kill_obj) },
  { MP_ROM_QSTR(MP_QSTR_close),   MP_ROM_PTR(&proc_close_obj) },
  { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&proc_close_obj) },
};

static MP_DEFINE_CONST_DICT(proc_locals_dict, proc_locals);

MP_DEFINE_CONST_OBJ_TYPE(
  agent_proc_type, MP_QSTR_Proc, MP_TYPE_FLAG_NONE,
  make_new, proc_make_new,
  attr, proc_attr,
  locals_dict, &proc_locals_dict);
