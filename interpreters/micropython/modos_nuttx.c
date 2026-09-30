/****************************************************************************
 * apps/interpreters/micropython/modos_nuttx.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The port's part of the os module (included by extmod/modos.c through
 * MICROPY_PY_OS_INCLUDEFILE): the environment and errno, as the unix
 * port has them.  The files (listdir, stat, remove, rename, mkdir, chdir,
 * getcwd ...) are the VFS's: NuttX's, through the POSIX VFS at /.
 ****************************************************************************/

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"

static mp_obj_t mp_os_getenv(size_t n_args, const mp_obj_t *args)
{
  const char *s = getenv(mp_obj_str_get_str(args[0]));

  if (s == NULL)
    {
      return n_args == 2 ? args[1] : mp_const_none;
    }

  return mp_obj_new_str_from_cstr(s);
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_os_getenv_obj, 1, 2,
                                           mp_os_getenv);

static mp_obj_t mp_os_putenv(mp_obj_t key_in, mp_obj_t value_in)
{
  if (setenv(mp_obj_str_get_str(key_in), mp_obj_str_get_str(value_in),
             1) < 0)
    {
      mp_raise_OSError(errno);
    }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_2(mp_os_putenv_obj, mp_os_putenv);

static mp_obj_t mp_os_unsetenv(mp_obj_t key_in)
{
  if (unsetenv(mp_obj_str_get_str(key_in)) < 0)
    {
      mp_raise_OSError(errno);
    }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_unsetenv_obj, mp_os_unsetenv);

static mp_obj_t mp_os_errno(size_t n_args, const mp_obj_t *args)
{
  if (n_args == 0)
    {
      return MP_OBJ_NEW_SMALL_INT(errno);
    }

  errno = mp_obj_get_int(args[0]);
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_os_errno_obj, 0, 1,
                                           mp_os_errno);
