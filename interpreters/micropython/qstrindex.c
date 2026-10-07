/****************************************************************************
 * apps/interpreters/micropython/qstrindex.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The firmware's qstrs found by their hash (patches/0005): MicroPython
 * looks for a qstr whenever it makes a str (to share the qstr's text) and
 * whenever it interns a name.  Its own search of the firmware's pools, a
 * binary search on the strings and a linear one on the static pool, reads
 * the flash all over: some 20 misses of the XIP cache a str on the board,
 * more than half the time of str-heavy Python.
 *
 * Those pools never change, so they are indexed once, by the first
 * interpreter that looks (the index is shared): an open-addressed table
 * of the qstrs' hashes and numbers, in the PSRAM (the program's static
 * data).  A lookup reads a slot or two of it, then the one string whose
 * hash matches.
 ****************************************************************************/

#include <nuttx/config.h>

#include <pthread.h>
#include <string.h>

#include "py/qstr.h"

#if MICROPY_QSTR_BYTES_IN_HASH

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define INDEX_BITS  12                    /* 4096 slots: the pc config has
                                           * about 1700 qstrs, with the
                                           * agent's */
#define INDEX_SIZE  (1 << INDEX_BITS)
#define INDEX_MASK  (INDEX_SIZE - 1)

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* A slot: the hash (16 bits) above the qstr's number + 1 (0: empty) */

static uint32_t g_index[INDEX_SIZE];
static volatile int g_state;              /* 0 not yet, 1 ready, -1 too
                                           * many qstrs (search as
                                           * MicroPython does) */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The pool that has qstr q, from the top one down */

static const qstr_pool_t *pool_of(const qstr_pool_t *pool, size_t q)
{
  while (pool != NULL && q < pool->total_prev_len)
    {
      pool = pool->prev;
    }

  return pool;
}

static int build(const qstr_pool_t *top)
{
  const qstr_pool_t *pool;
  size_t n = 0;
  size_t i;

  for (pool = top; pool != NULL; pool = pool->prev)
    {
      n += pool->len;
    }

  /* At most three quarters full; numbers in 16 bits */

  if (n > INDEX_SIZE * 3 / 4 || top->total_prev_len + top->len >= 0xffff ||
      sizeof(qstr_hash_t) > 2)
    {
      return -1;
    }

  for (pool = top; pool != NULL; pool = pool->prev)
    {
      for (i = 0; i < pool->len; i++)
        {
          uint32_t hash = pool->hashes[i];
          uint32_t slot = hash & INDEX_MASK;

          while (g_index[slot] != 0)
            {
              slot = (slot + 1) & INDEX_MASK;
            }

          g_index[slot] = hash << 16 | (pool->total_prev_len + i + 1);
        }
    }

  return 1;
}

/* What MicroPython does (too many qstrs for the index) */

static qstr search(const qstr_pool_t *pool, const char *str, size_t len,
                   size_t hash)
{
  size_t i;

  for (; pool != NULL; pool = pool->prev)
    {
      for (i = 0; i < pool->len; i++)
        {
          if (pool->hashes[i] == hash && pool->lengths[i] == len &&
              memcmp(pool->qstrs[i], str, len) == 0)
            {
              return pool->total_prev_len + i;
            }
        }
    }

  return MP_QSTRnull;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* The qstr with this text in the firmware's pools (top and those before
 * it), or MP_QSTRnull (MICROPY_PORT_QSTR_FIND_CONST, patches/0005)
 */

qstr mp_nuttx_qstr_find_const(const qstr_pool_t *top, const char *str,
                              size_t len, size_t hash)
{
  uint32_t slot;
  uint32_t e;

  if (g_state == 0)
    {
      pthread_mutex_lock(&g_lock);
      if (g_state == 0)
        {
          int state = build(top);

          __atomic_thread_fence(__ATOMIC_SEQ_CST);
          g_state = state;
        }

      pthread_mutex_unlock(&g_lock);
    }

  if (g_state < 0)
    {
      return search(top, str, len, hash);
    }

  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  for (slot = hash & INDEX_MASK; (e = g_index[slot]) != 0;
       slot = (slot + 1) & INDEX_MASK)
    {
      if ((e >> 16) == hash)
        {
          size_t q = (e & 0xffff) - 1;
          const qstr_pool_t *pool = pool_of(top, q);
          size_t at = q - pool->total_prev_len;

          if (pool->lengths[at] == len &&
              memcmp(pool->qstrs[at], str, len) == 0)
            {
              return q;
            }
        }
    }

  return MP_QSTRnull;
}

#endif /* MICROPY_QSTR_BYTES_IN_HASH */
