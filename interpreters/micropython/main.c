/****************************************************************************
 * apps/interpreters/micropython/main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MicroPython for NuttX: the program.
 *
 *   python                     the REPL (line editing, history, Ctrl-E
 *                              paste mode, Ctrl-D to leave)
 *   python FILE [ARGS...]      run a script
 *   python -c CODE [ARGS...]   run a string
 *   python -m MODULE [ARGS...] run a module
 *   options: -i (the REPL after), -X heapsize=N[K|M], -v, -h, --version
 *
 * Standard input that is not a terminal is run as a script.
 *
 * Each python task is an interpreter of its own, with its own state and
 * heap (patches/0002, mphalport.c): several run at once (up to four), a
 * REPL in a terminal and programs with windows, say.  A builtin's data are
 * shared by its runs, so nothing is kept in static variables.
 *
 * Its heap (Kconfig, -X heapsize) comes from the C heap: the PSRAM on the
 * FPC board.  Its stack is the task's (in the SRAM there), or with
 * CONFIG_INTERPRETERS_MICROPYTHON_PSRAM_STACK a thread's with a stack from
 * the heap.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <nuttx/sched.h>

#include "py/builtin.h"
#include "py/compile.h"
#include "py/cstack.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "extmod/vfs.h"
#include "extmod/vfs_posix.h"
#include "genhdr/mpversion.h"
#include "shared/runtime/gchelper.h"
#include "shared/runtime/pyexec.h"
#include "shared/readline/readline.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define STACK_SPARE       2048        /* Below the limit: NuttX, printf */

/****************************************************************************
 * External Function Prototypes
 ****************************************************************************/

void *mp_nuttx_attach(size_t ctxsize);
void mp_nuttx_detach(void);
#ifdef CONFIG_INTERPRETERS_MICROPYTHON_FPC
void mp_fpc_deinit(void);
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct run_s
{
  int argc;
  char **argv;
  size_t heapsize;
  int ret;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

const char nuttx_help_text[] =
  "Welcome to MicroPython on NuttX!\n"
  "\n"
  "Files are NuttX's: open(), os.listdir(), os.stat(), os.remove() ...\n"
  "the microSD is /mnt/sd0 on the FPC board, the PC's folder /host in the\n"
  "simulator.  import looks in the script's folder, then sys.path.\n"
  "\n"
  "With the FPC desktop, import fpc opens windows: see help(fpc).\n"
  "\n"
  "Control commands:\n"
  "  CTRL-C        -- interrupt a running program\n"
  "  CTRL-D        -- on a blank line, leave\n"
  "  CTRL-E        -- on a blank line, enter paste mode\n"
  "\n"
  "For a list of available modules, type help('modules')\n"
  "For further help on a specific object, type help(obj)\n";

/****************************************************************************
 * Private Functions: the terminal
 ****************************************************************************/

/* Raw: bytes as they are typed, no echo (readline echoes, Ctrl-C is seen
 * while Python runs); output as before (\n is \r\n)
 */

static void term_raw(struct mp_nuttx_port_s *p)
{
  struct termios t;

  if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &p->termios) < 0)
    {
      return;
    }

  t = p->termios;
  t.c_iflag &= ~(ICRNL | INLCR | IXON);
  t.c_lflag &= ~(ICANON | ECHO | ECHONL | ISIG);
  t.c_cc[VMIN]  = 1;
  t.c_cc[VTIME] = 0;
  p->rawterm = tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0;
}

void mp_nuttx_term_restore(void)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();

  if (p != NULL && p->rawterm)
    {
      tcsetattr(STDIN_FILENO, TCSANOW, &p->termios);
      p->rawterm = false;
    }
}

/****************************************************************************
 * Private Functions: running
 ****************************************************************************/

/* Exit codes, as CPython: 0, 1 for an exception, SystemExit's value */

static int exit_code(int ret)
{
  if (ret & PYEXEC_FORCED_EXIT)
    {
      return ret & 0xff;
    }

  return ret;
}

/* Standard input that is not a terminal: a script */

static int run_stdin(void)
{
  nlr_buf_t nlr;

  mp_hal_set_interrupt_char(CHAR_CTRL_C);
  if (nlr_push(&nlr) == 0)
    {
      mp_lexer_t *lex = mp_lexer_new_from_fd(MP_QSTR__lt_stdin_gt_,
                                             STDIN_FILENO, false);
      mp_parse_tree_t tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
      mp_obj_t fun = mp_compile(&tree, MP_QSTR__lt_stdin_gt_, false);

      mp_call_function_0(fun);
      mp_hal_set_interrupt_char(-1);
      mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
      nlr_pop();
      return 0;
    }

  mp_hal_set_interrupt_char(-1);
  mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_CLEAR_EXCEPTIONS);
  if (mp_obj_is_subclass_fast(
        MP_OBJ_FROM_PTR(((mp_obj_base_t *)nlr.ret_val)->type),
        MP_OBJ_FROM_PTR(&mp_type_SystemExit)))
    {
      mp_obj_t v = mp_obj_exception_get_value(MP_OBJ_FROM_PTR(nlr.ret_val));
      mp_int_t code = 0;

      if (v != mp_const_none && !mp_obj_get_int_maybe(v, &code))
        {
          code = 1;
        }

      return code & 0xff;
    }

  mp_obj_print_exception(&mp_stderr_print, MP_OBJ_FROM_PTR(nlr.ret_val));
  return 1;
}

static int run_module(const char *name)
{
  mp_obj_t args[4];
  nlr_buf_t nlr;

  args[0] = mp_obj_new_str_from_cstr(name);
  args[1] = mp_const_none;
  args[2] = mp_const_none;
  args[3] = mp_const_false;          /* It is __main__ */

  mp_hal_set_interrupt_char(CHAR_CTRL_C);
  if (nlr_push(&nlr) == 0)
    {
      mp_builtin___import__(MP_ARRAY_SIZE(args), args);
      mp_hal_set_interrupt_char(-1);
      mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
      nlr_pop();
      return 0;
    }

  mp_hal_set_interrupt_char(-1);
  mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_CLEAR_EXCEPTIONS);
  mp_obj_print_exception(&mp_stderr_print, MP_OBJ_FROM_PTR(nlr.ret_val));
  return 1;
}

static int run_repl(void)
{
  int ret;

  for (; ; )
    {
      if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL)
        {
          ret = pyexec_raw_repl();
        }
      else
        {
          ret = pyexec_friendly_repl();
        }

      if (ret != 0)
        {
          return ret & PYEXEC_FORCED_EXIT ? ret & 0xff : 0;
        }
    }
}

/* sys.path: "" (the script's folder once there is one), ".frozen" (the
 * modules in the firmware), then the configured folders or MICROPYPATH's, ~/ being the home directory
 */

static void set_path(void)
{
  const char *path = getenv("MICROPYPATH");
  const char *home = getenv("HOME");

#ifdef CONFIG_LIBC_HOMEDIR
  if (home == NULL)
    {
      home = CONFIG_LIBC_HOMEDIR;
    }
#endif

  if (path == NULL)
    {
      path = CONFIG_INTERPRETERS_MICROPYTHON_PATH;
    }

  mp_sys_path = mp_obj_new_list(0, NULL);
  mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
#if MICROPY_MODULE_FROZEN
  mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__dot_frozen));
#endif
  while (*path != '\0')
    {
      const char *end = strchr(path, ':');
      size_t n = end != NULL ? (size_t)(end - path) : strlen(path);

      if (n > 1 && path[0] == '~' && path[1] == '/' && home != NULL)
        {
          vstr_t v;

          vstr_init(&v, strlen(home) + n);
          vstr_add_str(&v, home);
          vstr_add_strn(&v, path + 1, n - 1);
          mp_obj_list_append(mp_sys_path, mp_obj_new_str_from_vstr(&v));
        }
      else if (n > 0)
        {
          mp_obj_list_append(mp_sys_path, mp_obj_new_str_via_qstr(path, n));
        }

      path += n;
      if (*path == ':')
        {
          path++;
        }
    }
}

static void set_argv(char **argv, int argc, int start)
{
  int i;

  for (i = start; i < argc; i++)
    {
      mp_obj_list_append(mp_sys_argv, mp_obj_new_str_from_cstr(argv[i]));
    }
}

static int usage(const char *prog, int ret)
{
  fprintf(ret ? stderr : stdout,
          "usage: %s [-i] [-v] [-X heapsize=N[K|M]] "
          "[-c CODE | -m MODULE | FILE] [ARGS...]\n", prog);
  return ret;
}

/* The options that come before the interpreter starts */

static int preoptions(int argc, char **argv, size_t *heapsize)
{
  int a;

  *heapsize = (size_t)CONFIG_INTERPRETERS_MICROPYTHON_HEAPSIZE * 1024;
  for (a = 1; a < argc && argv[a][0] == '-'; a++)
    {
      if (strcmp(argv[a], "-c") == 0 || strcmp(argv[a], "-m") == 0)
        {
          break;
        }
      else if (strcmp(argv[a], "-h") == 0 ||
               strcmp(argv[a], "--help") == 0)
        {
          return usage(argv[0], 0) - 1;
        }
      else if (strcmp(argv[a], "--version") == 0)
        {
          printf(MICROPY_BANNER_NAME_AND_VERSION "; "
                 MICROPY_BANNER_MACHINE "\n");
          return -1;
        }
      else if (strcmp(argv[a], "-X") == 0 && a + 1 < argc)
        {
          const char *o = argv[++a];

          if (strncmp(o, "heapsize=", 9) == 0)
            {
              char *end;
              long n = strtol(o + 9, &end, 0);

              if ((*end | 0x20) == 'k')
                {
                  n *= 1024;
                  end++;
                }
              else if ((*end | 0x20) == 'm')
                {
                  n *= 1024 * 1024;
                  end++;
                }

              if (*end != '\0' || n < 16 * 1024)
                {
                  return usage(argv[0], 2);
                }

              *heapsize = n;
            }
          else
            {
              return usage(argv[0], 2);
            }
        }
      else if (strcmp(argv[a], "-X") == 0)
        {
          return usage(argv[0], 2);
        }
    }

  return 0;
}

/* The interpreter, from start to end, on whichever stack it has */

static int interpreter(int argc, char **argv, size_t heapsize)
{
  bool inspect = false;
  bool ran = false;
  char *heap;
  int ret = 0;
  int a;

  heap = malloc(heapsize);
  if (heap == NULL)
    {
      fprintf(stderr, "%s: no memory for a heap of %zu KB\n", argv[0],
              heapsize / 1024);
      return 1;
    }

  gc_init(heap, heap + heapsize);
  mp_init();

  /* NuttX's files at /: the POSIX VFS, current */

  {
    mp_obj_t args[2];

    args[0] = MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_posix, make_new)(
                &mp_type_vfs_posix, 0, 0, NULL);
    args[1] = MP_OBJ_NEW_QSTR(MP_QSTR__slash_);
    mp_vfs_mount(2, args, (mp_map_t *)&mp_const_empty_map);
    MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_mount_table);
  }

  set_path();
  mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_argv), 0);

  for (a = 1; a < argc && !ran; a++)
    {
      if (strcmp(argv[a], "-i") == 0)
        {
          inspect = true;
        }
#if MICROPY_DEBUG_PRINTERS
      else if (strcmp(argv[a], "-v") == 0)
        {
          MP_STATE_VM(mp_verbose_flag)++;
        }
#endif
      else if (strcmp(argv[a], "-X") == 0)
        {
          a++;
        }
      else if (strcmp(argv[a], "-c") == 0)
        {
          vstr_t v;

          if (a + 1 >= argc)
            {
              ret = usage(argv[0], 2);
              break;
            }

          set_argv(argv, a + 1, a);        /* "-c", then the arguments */
          set_argv(argv, argc, a + 2);
          vstr_init(&v, strlen(argv[a + 1]) + 1);
          vstr_add_str(&v, argv[a + 1]);
          ret = exit_code(pyexec_vstr(&v, true));
          vstr_clear(&v);
          ran = true;
        }
      else if (strcmp(argv[a], "-m") == 0)
        {
          if (a + 1 >= argc)
            {
              ret = usage(argv[0], 2);
              break;
            }

          set_argv(argv, argc, a + 1);
          ret = run_module(argv[a + 1]);
          ran = true;
        }
      else if (argv[a][0] == '-')
        {
          ret = usage(argv[0], 2);
          break;
        }
      else
        {
          char *dir = realpath(argv[a], NULL);
          char *slash;

          if (dir == NULL)
            {
              fprintf(stderr, "%s: can't open file '%s': [Errno %d] %s\n",
                      argv[0], argv[a], errno, strerror(errno));
              ret = 2;
              break;
            }

          /* The script's folder first in sys.path */

          slash = strrchr(dir, '/');
          mp_obj_list_store(mp_sys_path, MP_OBJ_NEW_SMALL_INT(0),
                            mp_obj_new_str_via_qstr(dir, slash > dir ?
                                                    slash - dir : 1));
          free(dir);
          set_argv(argv, argc, a);
          ret = exit_code(pyexec_file(argv[a]));
          ran = true;
        }
    }

  if (ret != 2 && (!ran || inspect))
    {
      if (isatty(STDIN_FILENO) || inspect)
        {
          ret = run_repl();
        }
      else
        {
          ret = run_stdin();
        }
    }

  /* sys.atexit's function; the program's windows */

  if (mp_obj_is_callable(MP_STATE_VM(sys_exitfunc)))
    {
      nlr_buf_t nlr;

      if (nlr_push(&nlr) == 0)
        {
          mp_call_function_0(MP_STATE_VM(sys_exitfunc));
          nlr_pop();
        }
      else
        {
          mp_obj_print_exception(&mp_stderr_print,
                                 MP_OBJ_FROM_PTR(nlr.ret_val));
        }
    }

#ifdef CONFIG_INTERPRETERS_MICROPYTHON_FPC
  mp_fpc_deinit();
#endif

#if MICROPY_DEBUG_PRINTERS
  if (MP_STATE_VM(mp_verbose_flag))
    {
      mp_micropython_mem_info(0, NULL);
    }
#endif

  gc_sweep_all();
  mp_deinit();
  free(heap);
  return ret;
}

/* Start the interpreter with the stack it has: the most it may use is
 * what is left below here
 */

static MP_NOINLINE int interpreter_here(int argc, char **argv,
                                        size_t heapsize, size_t stack_size)
{
  volatile char here;
  struct stackinfo_s si;
  size_t avail = stack_size;

  if (nxsched_get_stackinfo(0, &si) == 0 && si.stack_alloc_ptr != NULL)
    {
      uintptr_t base = (uintptr_t)si.stack_base_ptr;
      uintptr_t sp = (uintptr_t)&here;

      if (sp > base && sp - base < si.adj_stack_size + 64)
        {
          avail = sp - base;
        }
    }

  avail = avail > STACK_SPARE + 4096 ? avail - STACK_SPARE : 4096;
  mp_cstack_init_with_sp_here(avail);
  return interpreter(argc, argv, heapsize);
}

#if CONFIG_INTERPRETERS_MICROPYTHON_PSRAM_STACK > 0
static void *interpreter_thread(void *arg)
{
  struct run_s *r = arg;

  r->ret = interpreter_here(r->argc, r->argv, r->heapsize,
                            CONFIG_INTERPRETERS_MICROPYTHON_PSRAM_STACK);
  return NULL;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void gc_collect(void)
{
  gc_collect_start();
  gc_helper_collect_regs_and_stack();
  gc_collect_end();
}

void nlr_jump_fail(void *val)
{
  mp_nuttx_term_restore();
  fprintf(stderr, "python: FATAL: uncaught NLR %p\n", val);
  mp_nuttx_detach();
  exit(1);
}

int main(int argc, FAR char *argv[])
{
  struct mp_nuttx_port_s *p;
  struct run_s r;
  int ret;

  r.argc = argc;
  r.argv = argv;
  r.ret  = 1;
  ret = preoptions(argc, argv, &r.heapsize);
  if (ret != 0)
    {
      return ret < 0 ? 0 : ret;
    }

  /* This task's interpreter: its state */

  p = mp_nuttx_attach(sizeof(mp_state_ctx_t));
  if (p == NULL)
    {
      fprintf(stderr, "%s: too many interpreters running\n", argv[0]);
      return 1;
    }

  term_raw(p);

#if CONFIG_INTERPRETERS_MICROPYTHON_PSRAM_STACK > 0
  {
    pthread_attr_t attr;
    pthread_t thread;
    void *stack;

    stack = malloc(CONFIG_INTERPRETERS_MICROPYTHON_PSRAM_STACK);
    pthread_attr_init(&attr);
    if (stack != NULL &&
        pthread_attr_setstack(&attr, stack,
                              CONFIG_INTERPRETERS_MICROPYTHON_PSRAM_STACK)
        == 0 &&
        pthread_create(&thread, &attr, interpreter_thread, &r) == 0)
      {
        pthread_join(thread, NULL);
      }
    else
      {
        fprintf(stderr, "%s: no thread for the interpreter\n", argv[0]);
      }

    pthread_attr_destroy(&attr);
    free(stack);
  }
#else
  r.ret = interpreter_here(argc, argv, r.heapsize,
                           CONFIG_INTERPRETERS_MICROPYTHON_STACKSIZE);
#endif

  mp_nuttx_term_restore();
  mp_nuttx_detach();
  return r.ret;
}
