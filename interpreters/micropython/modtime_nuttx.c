/****************************************************************************
 * apps/interpreters/micropython/modtime_nuttx.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The port's part of the time module (included by extmod/modtime.c
 * through MICROPY_PY_TIME_INCLUDEFILE): the wall clock, and sleep(),
 * which, like everything that waits, watches for Ctrl-C
 * (mp_hal_delay_ms(), mphalport.c).
 ****************************************************************************/

#include <sys/time.h>
#include <time.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/timeutils/timeutils.h"

/* time.time(): a float with doubles (as CPython), else whole seconds (a
 * single-precision float cannot hold today to the second)
 */

static mp_obj_t mp_time_time_get(void)
{
#if MICROPY_FLOAT_IMPL == MICROPY_FLOAT_IMPL_DOUBLE
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return mp_obj_new_float(tv.tv_sec + (mp_float_t)tv.tv_usec / 1000000);
#else
  return mp_obj_new_int_from_ll((long long)time(NULL));
#endif
}

/* localtime() and gmtime(): NuttX keeps UTC */

static void mp_time_localtime_get(timeutils_struct_time_t *tm)
{
  timeutils_seconds_since_epoch_to_struct_time(time(NULL), tm);
}

static void mp_time_sleep(mp_obj_t seconds)
{
#if MICROPY_PY_BUILTINS_FLOAT
  mp_float_t s = mp_obj_get_float(seconds);

  if (s < 0)
    {
      mp_raise_ValueError(MP_ERROR_TEXT("sleep length must be non-negative"));
    }

  mp_hal_delay_ms((mp_uint_t)(s * 1000 + MICROPY_FLOAT_CONST(0.5)));
#else
  mp_hal_delay_ms(1000 * mp_obj_get_int(seconds));
#endif
}
