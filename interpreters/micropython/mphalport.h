/****************************************************************************
 * apps/interpreters/micropython/mphalport.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MicroPython's hardware abstraction for NuttX (mphalport.c): the
 * terminal, the clocks, waiting, Ctrl-C.
 ****************************************************************************/

#ifndef __APPS_INTERPRETERS_MICROPYTHON_MPHALPORT_H
#define __APPS_INTERPRETERS_MICROPYTHON_MPHALPORT_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#ifndef CHAR_CTRL_C
#  define CHAR_CTRL_C (3)
#endif

#ifndef CHAR_CTRL_D
#  define CHAR_CTRL_D (4)
#endif

/* The terminal stays raw (no line discipline, no echo) all the while the
 * interpreter runs (main.c), so that Ctrl-C can be seen while Python
 * runs; readline edits and echoes.  No switching.
 */

#define MICROPY_HAL_HAS_STDIO_MODE_SWITCH (0)

/* Ctrl-C interrupts while this is CHAR_CTRL_C (shared/runtime) */

void mp_hal_set_interrupt_char(int c);

/* Waiting: for up to ms (-1: a while) with an eye on the input for Ctrl-C;
 * and looking at the input now (both in mphalport.c)
 */

void mp_nuttx_wfe(int ms);
void mp_nuttx_poll_input(void);

#define MICROPY_INTERNAL_WFE(ms)     mp_nuttx_wfe(ms)
#define MICROPY_INTERNAL_EVENT_HOOK  mp_nuttx_poll_input()

/* Input: bytes waiting (read while looking for Ctrl-C), else the terminal */

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags);

#define mp_hal_ticks_cpu()           mp_hal_ticks_us()

void mp_hal_delay_us(mp_uint_t us);
void mp_hal_get_random(size_t n, uint8_t *buf);

/* PEP 475: system calls are retried after EINTR, Ctrl-C permitting */

#define MP_HAL_RETRY_SYSCALL(ret, syscall, raise) \
  { \
    for (; ; ) \
      { \
        ret = syscall; \
        if (ret == -1) \
          { \
            int err = errno; \
            if (err == EINTR) \
              { \
                mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS); \
                continue; \
              } \
            raise; \
          } \
        break; \
      } \
  }

#define RAISE_ERRNO(err_flag, error_val) \
  { \
    if (err_flag == -1) \
      { \
        mp_raise_OSError(error_val); \
      } \
  }

/* main.c: the terminal back as it was (an exit, a fatal error) */

void mp_nuttx_term_restore(void);

#endif /* __APPS_INTERPRETERS_MICROPYTHON_MPHALPORT_H */
