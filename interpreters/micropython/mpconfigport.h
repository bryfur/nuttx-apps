/****************************************************************************
 * apps/interpreters/micropython/mpconfigport.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MicroPython's configuration for NuttX: based on the unix port's
 * (extra features, a POSIX file system), with what a board of 520 KB of
 * SRAM and a few MB of PSRAM wants.  Kconfig chooses the float, the
 * native code emitters and the sizes.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <sys/types.h>
#include <alloca.h>
#include <limits.h>

/* The level: the unix port's standard variant, and t-strings (PEP 750),
 * less what NuttX has not
 */

#define MICROPY_CONFIG_ROM_LEVEL      (MICROPY_CONFIG_ROM_LEVEL_FULL_FEATURES)

/* The heap: garbage collected, from the C heap (the PSRAM on the board) */

#define MICROPY_ENABLE_GC             (1)
#define MICROPY_ENABLE_FINALISER      (1)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE (256)
#define MICROPY_MEM_STATS             (0)
#define MICROPY_GC_ALLOC_THRESHOLD    (1)

/* The compiler, the REPL */

#define MICROPY_ENABLE_COMPILER       (1)
#define MICROPY_PERSISTENT_CODE_LOAD  (1)
#define MICROPY_HELPER_REPL           (1)
#define MICROPY_REPL_AUTO_INDENT      (1)
#define MICROPY_REPL_EMACS_KEYS       (1)
#define MICROPY_REPL_EMACS_WORDS_MOVE (1)
#define MICROPY_REPL_EMACS_EXTRA_WORDS_MOVE (1)
#define MICROPY_USE_READLINE_HISTORY  (1)
#define MICROPY_READLINE_HISTORY_SIZE (20)  /* A literal: qstr extraction */
#define MICROPY_KBD_EXCEPTION         (1)
#define MICROPY_ENABLE_SCHEDULER      (1)
#define MICROPY_OPT_COMPUTED_GOTO     (1)
#define MICROPY_ERROR_REPORTING       (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_WARNINGS              (1)
#define MICROPY_PY_STR_BYTES_CMP_WARN (1)
#define MICROPY_PYEXEC_ENABLE_EXIT_CODE_HANDLING (1)
#define MICROPY_MODULE_OVERRIDE_MAIN_IMPORT (1)
#define MICROPY_STACK_CHECK           (1)
#define MICROPY_STACK_CHECK_MARGIN    (1024)
#define MICROPY_ASYNC_KBD_INTR        (0)

/* Numbers */

#define MICROPY_LONGINT_IMPL          (MICROPY_LONGINT_IMPL_MPZ)
#ifdef CONFIG_INTERPRETERS_MICROPYTHON_DOUBLE
#  define MICROPY_FLOAT_IMPL          (MICROPY_FLOAT_IMPL_DOUBLE)
#else
#  define MICROPY_FLOAT_IMPL          (MICROPY_FLOAT_IMPL_FLOAT)
#endif
#define MICROPY_FLOAT_USE_NATIVE_FLT16 (0)

/* Native code: Thumb-2 (ARMv7-M, ARMv8-M mainline: the Cortex-M33).  The
 * code goes in the GC heap; after it is written, the pipeline is flushed
 * (the RP2350's XIP cache is shared by data and instructions).
 */

#ifdef CONFIG_INTERPRETERS_MICROPYTHON_NATIVE
#  define MICROPY_EMIT_THUMB          (1)
#  define MICROPY_EMIT_THUMB_ARMV7M   (1)
#  define MICROPY_EMIT_INLINE_THUMB   (1)
#  define MICROPY_EMIT_INLINE_THUMB_FLOAT (1)
#  define MICROPY_MAKE_POINTER_CALLABLE(p) ((void *)((mp_uint_t)(p) | 1))
#  define MP_HAL_CLEAN_DCACHE(addr, size) \
     __asm__ volatile ("dsb\n\tisb" ::: "memory")
#else
#  define MICROPY_EMIT_X64            (0)
#  define MICROPY_EMIT_X86            (0)
#  define MICROPY_EMIT_THUMB          (0)
#  define MICROPY_EMIT_ARM            (0)
#  define MICROPY_EMIT_INLINE_THUMB   (0)
#endif

/* The agent's Python, frozen into the firmware as source (mkfrozen.py),
 * imported from ".frozen" in sys.path
 */

#ifdef CONFIG_INTERPRETERS_MICROPYTHON_AGENT
#  define MICROPY_MODULE_FROZEN_STR   (1)
#endif

/* Files: NuttX's, through the POSIX VFS mounted at / */

#define MICROPY_VFS                   (1)
#define MICROPY_VFS_POSIX             (1)
#define MICROPY_VFS_POSIX_STDIO       (0)     /* sys.std*: sys_stdio_mphal.c
                                               * (patches/0001) */
#define MICROPY_READER_VFS            (1)
#define MICROPY_READER_POSIX          (1)
#define MICROPY_HELPER_LEXER_UNIX     (1)
#define MICROPY_VFS_FAT               (0)
#define MICROPY_VFS_LFS1              (0)
#define MICROPY_VFS_LFS2              (0)
#define MICROPY_VFS_ROM               (0)
#define MICROPY_ALLOC_PATH_MAX        (PATH_MAX)
#define MICROPY_STREAMS_POSIX_API     (1)
#define _DIRENT_HAVE_D_TYPE           (1)
#define _DIRENT_HAVE_D_INO            (1)

/* Modules */

#define MICROPY_PY_SYS_PLATFORM       "nuttx"
#define MICROPY_PY_SYS_PATH_ARGV_DEFAULTS (0)
#define MICROPY_PY_SYS_EXECUTABLE     (0)
#define MICROPY_PY_SYS_ATEXIT         (1)
#define MICROPY_PY_SYS_EXC_INFO       (1)
#define MICROPY_PY_SYS_STDFILES       (1)
#define MICROPY_PY_SYS_STDIO_BUFFER   (1)
#define MICROPY_PY_SYS_SETTRACE       (0)
#define MICROPY_PY_BUILTINS_INPUT     (1)
#define MICROPY_PY_BUILTINS_HELP      (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT nuttx_help_text
#define MICROPY_PY_BUILTINS_HELP_MODULES (1)
#define MICROPY_PY_MICROPYTHON_MEM_INFO (1)
#define MICROPY_PY_GC_COLLECT_RETVAL  (1)
#define MICROPY_PY_OS                 (1)
#define MICROPY_PY_OS_INCLUDEFILE     "modos_nuttx.c"
#define MICROPY_PY_OS_ERRNO           (1)
#define MICROPY_PY_OS_GETENV_PUTENV_UNSETENV (1)
#define MICROPY_PY_OS_SYSTEM          (0)
#define MICROPY_PY_OS_URANDOM         (1)
#define MICROPY_PY_OS_UNAME           (1)
#define MICROPY_PY_OS_SEP             (1)
#define MICROPY_PY_OS_DUPTERM         (0)
#define MICROPY_PY_TIME               (1)
#define MICROPY_PY_TIME_TIME_TIME_NS  (1)
#define MICROPY_PY_TIME_CUSTOM_SLEEP  (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_INCLUDEFILE   "modtime_nuttx.c"
#define MICROPY_EPOCH_IS_1970         (1)
#define MICROPY_TIMESTAMP_IMPL        (MICROPY_TIMESTAMP_IMPL_TIME_T)
#define MICROPY_PY_SELECT             (1)
#define MICROPY_PY_SELECT_POSIX_OPTIMISATIONS (1)
#define MICROPY_PY_SELECT_SELECT      (0)
#define MICROPY_SELECT_REMAINING_TIME (1)
#define MICROPY_PY_RANDOM             (1)
#define MICROPY_PY_RANDOM_EXTRA_FUNCS (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC (mp_nuttx_random_seed())
#define MICROPY_PY_JSON               (1)
#define MICROPY_PY_RE                 (1)
#define MICROPY_PY_HEAPQ              (1)
#define MICROPY_PY_BINASCII           (1)
#define MICROPY_PY_UCTYPES            (1)
#define MICROPY_PY_PLATFORM           (1)
#define MICROPY_PY_VFS                (1)
#define MICROPY_PY_ERRNO              (1)
#define MICROPY_PY_HASHLIB            (0)
#define MICROPY_PY_CRYPTOLIB          (0)
#define MICROPY_PY_DEFLATE            (0)
#define MICROPY_PY_ASYNCIO            (0)
#define MICROPY_PY_MACHINE            (0)
#define MICROPY_PY_NETWORK            (0)
#define MICROPY_PY_SOCKET             (0)
#define MICROPY_PY_SSL                (0)
#define MICROPY_PY_WEBSOCKET          (0)
#define MICROPY_PY_WEBREPL            (0)
#define MICROPY_PY_FRAMEBUF           (0)
#define MICROPY_PY_BTREE              (0)
#define MICROPY_PY_BLUETOOTH          (0)
#define MICROPY_PY_THREAD             (0)
#define MICROPY_PY_LWIP               (0)
#define MICROPY_PY_ONEWIRE            (0)
#define MICROPY_PY_OPENAMP            (0)

/* The machine */

#ifdef CONFIG_ARCH_SIM
#  define MICROPY_HW_BOARD_NAME       "the NuttX simulator"
#  define MICROPY_HW_MCU_NAME         "sim"
#else
#  define MICROPY_HW_BOARD_NAME       "FPC"
#  define MICROPY_HW_MCU_NAME         "RP2350"
#endif

#define MICROPY_BANNER_MACHINE        MICROPY_HW_BOARD_NAME " with " \
                                      MICROPY_HW_MCU_NAME

/* Types */

typedef long mp_off_t;

#define MP_STATE_PORT                 MP_STATE_VM

#define MP_SSIZE_MAX                  SSIZE_MAX

/* Output for errors and debugging goes to stderr */

extern const struct _mp_print_t mp_stderr_print;
#define MICROPY_DEBUG_PRINTER         (&mp_stderr_print)
#define MICROPY_ERROR_PRINTER         (&mp_stderr_print)

/* Several interpreters at once (patches/0002): each python task has its
 * own state, found by its pid (mphalport.c: mp_nuttx_port()).  First what
 * the port keeps for each (and MicroPython's few static variables), then
 * MicroPython's state.  mp_nuttx_port() is const: a function calls it once
 * however many times it uses the state.
 */

#include <stdbool.h>
#include <termios.h>

struct mp_nuttx_port_s
{
  /* MicroPython's static variables, one set each (patches/0002) */

  uint32_t readline[16];               /* shared/readline's readline_t */
  int      pyexec_mode;                /* pyexec_mode_kind */
  struct
  {
    uint32_t pad;
    uint32_t n;
    uint32_t d;
    uint8_t  dat;
    bool     seeded;
  } random;

  /* The terminal and Ctrl-C (mphalport.c, main.c) */

  int      interrupt_char;
  int      interrupt_esc;              /* A lone Esc interrupts too (the
                                        * agent, while it works): -1 not */
  uint8_t  ring[256];                  /* Typed ahead */
  int16_t  head;
  int16_t  count;
  bool     eof;                        /* The input has ended */
  bool     rawterm;                    /* termios is the terminal's */
  uint32_t lastpoll;
  struct termios termios;

  /* The fpc module's windows (modfpc.c) */

  void    *fpc;
};

#define MP_NUTTX_PORT_SIZE \
  ((sizeof(struct mp_nuttx_port_s) + 15) & ~(size_t)15)

struct mp_nuttx_port_s *mp_nuttx_port(void) __attribute__((const));

#define MICROPY_PORT_STATE_CTX \
  (*(struct _mp_state_ctx_t *)((char *)mp_nuttx_port() + MP_NUTTX_PORT_SIZE))
#define MICROPY_PORT_READLINE_STATE       (mp_nuttx_port()->readline)
#define MICROPY_PORT_READLINE_STATE_SIZE  (sizeof(uint32_t) * 16)
#define MICROPY_PORT_PYEXEC_MODE_KIND     (mp_nuttx_port()->pyexec_mode)
#define MICROPY_PORT_RANDOM_STATE         (mp_nuttx_port()->random)

/* Ctrl-C: the input is looked at for it now and then while Python runs
 * (mphalport.c), there being no SIGINT from NuttX's terminals.  The count
 * is only a throttle: shared.
 */

void mp_nuttx_vm_hook(void);
extern int mp_nuttx_vm_count;

#define MICROPY_VM_HOOK_LOOP \
  if (--mp_nuttx_vm_count <= 0) { mp_nuttx_vm_hook(); }
#define MICROPY_VM_HOOK_RETURN        MICROPY_VM_HOOK_LOOP

unsigned long mp_nuttx_random_seed(void);

extern const char nuttx_help_text[];
