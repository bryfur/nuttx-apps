/****************************************************************************
 * apps/interpreters/wamr/wamr_nuttx_stubs.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * What WAMR's POSIX layer refers to that a NuttX build may leave out:
 * named semaphores (posix_thread.c's os_sem_open() and friends, used only
 * by the legacy pthread library's semaphores).  Without
 * CONFIG_FS_NAMED_SEMAPHORES they fail; a WAMR built as a loadable module
 * then needs nothing of the firmware that is not there.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>

#ifndef CONFIG_FS_NAMED_SEMAPHORES

FAR sem_t *sem_open(FAR const char *name, int oflag, ...)
{
  errno = ENOSYS;
  return SEM_FAILED;
}

int sem_close(FAR sem_t *sem)
{
  errno = ENOSYS;
  return -1;
}

int sem_unlink(FAR const char *name)
{
  errno = ENOSYS;
  return -1;
}

#endif
