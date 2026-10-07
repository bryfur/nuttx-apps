/****************************************************************************
 * apps/interpreters/micropython/modhashlib_nuttx.c
 *
 * SPDX-License-Identifier: MIT
 *
 * hashlib with sha256 (extmod's hashlib is off: MICROPY_PY_HASHLIB 0):
 * through /dev/crypto when it has SHA-256 (on the board the RP2350's
 * accelerator, docs/hardware-accel.md), else NuttX's crypto/sha2.c.
 * As MicroPython's: sha256([data]), update(data), digest(); and
 * hexdigest() as CPython's.  A hash's /dev/crypto session is closed by
 * digest() or, if it is never asked for, by the garbage collector.
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <crypto/sha2.h>
#ifdef CONFIG_CRYPTO_CRYPTODEV
#  include <crypto/cryptodev.h>
#endif

#include "py/runtime.h"
#include "py/objstr.h"

typedef struct _mp_obj_sha256_t
{
  mp_obj_base_t base;
  bool final;
  int fd;               /* The /dev/crypto session's, or -1 */
  uint32_t ses;
  SHA2_CTX sw;
} mp_obj_sha256_t;

static const mp_obj_type_t mp_type_sha256;

static void sha256_close(mp_obj_sha256_t *self)
{
#ifdef CONFIG_CRYPTO_CRYPTODEV
  if (self->fd >= 0)
    {
      ioctl(self->fd, CIOCFSESSION, &self->ses);
      close(self->fd);
      self->fd = -1;
    }
#endif
}

static void sha256_open(mp_obj_sha256_t *self)
{
  self->fd = -1;
#ifdef CONFIG_CRYPTO_CRYPTODEV
  int fd = open("/dev/crypto", O_RDWR);
  int cfd;

  if (fd >= 0)
    {
      if (ioctl(fd, CRIOGET, &cfd) == 0)
        {
          struct session_op ses;

          memset(&ses, 0, sizeof(ses));
          ses.mac = CRYPTO_SHA2_256;
          if (ioctl(cfd, CIOCGSESSION, &ses) == 0)
            {
              self->fd = cfd;
              self->ses = ses.ses;
            }
          else
            {
              close(cfd);
            }
        }

      close(fd);
    }
#endif

  sha256init(&self->sw);
}

static mp_obj_t sha256_update(mp_obj_t self_in, mp_obj_t arg)
{
  mp_obj_sha256_t *self = MP_OBJ_TO_PTR(self_in);
  mp_buffer_info_t buf;

  if (self->final)
    {
      mp_raise_ValueError(MP_ERROR_TEXT("hash is final"));
    }

  mp_get_buffer_raise(arg, &buf, MP_BUFFER_READ);
#ifdef CONFIG_CRYPTO_CRYPTODEV
  if (self->fd >= 0)
    {
      struct crypt_op cop;

      memset(&cop, 0, sizeof(cop));
      cop.ses = self->ses;
      cop.op = COP_ENCRYPT;
      cop.flags = COP_FLAG_UPDATE;
      cop.src = (caddr_t)buf.buf;
      cop.len = buf.len;
      if (ioctl(self->fd, CIOCCRYPT, &cop) < 0)
        {
          mp_raise_OSError(errno);
        }

      return mp_const_none;
    }
#endif

  sha256update(&self->sw, buf.buf, buf.len);
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_2(sha256_update_obj, sha256_update);

static void sha256_get(mp_obj_sha256_t *self, uint8_t *out)
{
  if (self->final)
    {
      mp_raise_ValueError(MP_ERROR_TEXT("hash is final"));
    }

  self->final = true;
#ifdef CONFIG_CRYPTO_CRYPTODEV
  if (self->fd >= 0)
    {
      struct crypt_op cop;
      int ret;

      memset(&cop, 0, sizeof(cop));
      cop.ses = self->ses;
      cop.op = COP_ENCRYPT;
      cop.mac = (caddr_t)out;
      ret = ioctl(self->fd, CIOCCRYPT, &cop);
      sha256_close(self);
      if (ret < 0)
        {
          mp_raise_OSError(errno);
        }

      return;
    }
#endif

  sha256final(out, &self->sw);
}

static mp_obj_t sha256_digest(mp_obj_t self_in)
{
  vstr_t vstr;

  vstr_init_len(&vstr, 32);
  sha256_get(MP_OBJ_TO_PTR(self_in), (uint8_t *)vstr.buf);
  return mp_obj_new_bytes_from_vstr(&vstr);
}

static MP_DEFINE_CONST_FUN_OBJ_1(sha256_digest_obj, sha256_digest);

static mp_obj_t sha256_hexdigest(mp_obj_t self_in)
{
  static const char digits[] = "0123456789abcdef";
  uint8_t d[32];
  vstr_t vstr;
  int i;

  sha256_get(MP_OBJ_TO_PTR(self_in), d);
  vstr_init_len(&vstr, 64);
  for (i = 0; i < 32; i++)
    {
      vstr.buf[2 * i] = digits[d[i] >> 4];
      vstr.buf[2 * i + 1] = digits[d[i] & 15];
    }

  return mp_obj_new_str_from_vstr(&vstr);
}

static MP_DEFINE_CONST_FUN_OBJ_1(sha256_hexdigest_obj, sha256_hexdigest);

static mp_obj_t sha256_del(mp_obj_t self_in)
{
  sha256_close(MP_OBJ_TO_PTR(self_in));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(sha256_del_obj, sha256_del);

static mp_obj_t sha256_make_new(const mp_obj_type_t *type, size_t n_args,
                                size_t n_kw, const mp_obj_t *args)
{
  mp_obj_sha256_t *o;

  mp_arg_check_num(n_args, n_kw, 0, 1, false);
  o = mp_obj_malloc_with_finaliser(mp_obj_sha256_t, type);
  o->final = false;
  sha256_open(o);
  if (n_args == 1)
    {
      sha256_update(MP_OBJ_FROM_PTR(o), args[0]);
    }

  return MP_OBJ_FROM_PTR(o);
}

static const mp_rom_map_elem_t sha256_locals_dict_table[] =
{
  { MP_ROM_QSTR(MP_QSTR_update), MP_ROM_PTR(&sha256_update_obj) },
  { MP_ROM_QSTR(MP_QSTR_digest), MP_ROM_PTR(&sha256_digest_obj) },
  { MP_ROM_QSTR(MP_QSTR_hexdigest), MP_ROM_PTR(&sha256_hexdigest_obj) },
  { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&sha256_del_obj) },
};

static MP_DEFINE_CONST_DICT(sha256_locals_dict, sha256_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
  mp_type_sha256,
  MP_QSTR_sha256,
  MP_TYPE_FLAG_NONE,
  make_new, sha256_make_new,
  locals_dict, &sha256_locals_dict
);

static const mp_rom_map_elem_t mp_module_hashlib_globals_table[] =
{
  { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_hashlib) },
  { MP_ROM_QSTR(MP_QSTR_sha256), MP_ROM_PTR(&mp_type_sha256) },
};

static MP_DEFINE_CONST_DICT(mp_module_hashlib_globals,
                            mp_module_hashlib_globals_table);

const mp_obj_module_t mp_module_hashlib =
{
  .base = { &mp_type_module },
  .globals = (mp_obj_dict_t *)&mp_module_hashlib_globals,
};

MP_REGISTER_MODULE(MP_QSTR_hashlib, mp_module_hashlib);
