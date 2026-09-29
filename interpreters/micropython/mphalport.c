/****************************************************************************
 * apps/interpreters/micropython/mphalport.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MicroPython's hardware abstraction for NuttX: the terminal, the clocks,
 * waiting, and Ctrl-C.
 *
 * NuttX's terminals send no SIGINT (CONFIG_TTY_SIGINT is off, and a
 * pseudo-terminal's Ctrl-C is a byte like any other), so Ctrl-C is found
 * by looking at the input: every few ms while Python runs (the VM's loop
 * hook), and whenever it waits (sleep, the fpc module's events, select).
 * What else was typed meanwhile is kept for later reads (input(),
 * sys.stdin), in order.  The terminal is raw all along (main.c) so that
 * bytes come as they are typed.
 *
 * Each python task is an interpreter of its own (patches/0002): its state
 * (MicroPython's and this port's) is found by its pid.
 ****************************************************************************/

#include <nuttx/config.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "shared/runtime/pyexec.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RING_SIZE        256              /* Typed ahead */
#define POLL_MS          20               /* Looking for Ctrl-C */
#define VM_COUNT         256              /* Loop hooks between clock reads */
#define MAX_INTERPRETERS 4

/****************************************************************************
 * Public Data
 ****************************************************************************/

int mp_nuttx_vm_count = VM_COUNT;

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The interpreters running: each task's state */

static struct
{
  pid_t pid;
  struct mp_nuttx_port_s *port;
} g_interp[MAX_INTERPRETERS];

static pthread_mutex_t g_interplock = PTHREAD_MUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* What the input has now (without waiting) into the ring; Ctrl-C, while
 * it interrupts, schedules KeyboardInterrupt and goes no further
 */

static void input_take(int timeout)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  struct pollfd pfd;
  uint8_t buf[64];
  ssize_t n;
  ssize_t i;

  if (p->eof)
    {
      if (timeout > 0)
        {
          usleep(timeout * 1000);
        }

      return;
    }

  pfd.fd      = STDIN_FILENO;
  pfd.events  = POLLIN;
  pfd.revents = 0;
  if (poll(&pfd, 1, timeout) <= 0)
    {
      return;
    }

  if ((pfd.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
    {
      return;
    }

  n = RING_SIZE - p->count;
  if (n > (ssize_t)sizeof(buf))
    {
      n = sizeof(buf);
    }

  if (n == 0)
    {
      /* Full: nobody reads.  Look for Ctrl-C only, a byte at a time. */

      n = 1;
    }

  n = read(STDIN_FILENO, buf, n);
  if (n <= 0)
    {
      if (n == 0 || (errno != EINTR && errno != EAGAIN))
        {
          p->eof = true;
        }

      return;
    }

  for (i = 0; i < n; i++)
    {
      if (p->interrupt_char >= 0 && buf[i] == p->interrupt_char)
        {
          /* What was typed before it goes too, as in a terminal */

          p->count = 0;
          mp_sched_keyboard_interrupt();
          continue;
        }

      if (p->count < RING_SIZE)
        {
          p->ring[(p->head + p->count) % RING_SIZE] = buf[i];
          p->count++;
        }
    }
}

/****************************************************************************
 * Public Functions: input, output
 ****************************************************************************/

void mp_nuttx_poll_input(void)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  p->lastpoll = mp_hal_ticks_ms();
  input_take(0);
}

void mp_nuttx_vm_hook(void)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();

  mp_nuttx_vm_count = VM_COUNT;
  if (mp_hal_ticks_ms() - p->lastpoll >= POLL_MS)
    {
#ifdef CONFIG_ARCH_SIM
      /* The simulator's clock, timers and input move only when its idle
       * thread runs: a Python loop that never waits would stop them all
       * (and never see Ctrl-C)
       */

      usleep(1000);
#endif
      mp_nuttx_poll_input();
    }
}

int mp_hal_stdin_rx_chr(void)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  int c;

  while (p->count == 0)
    {
      if (p->eof)
        {
          return CHAR_CTRL_D;
        }

      /* Wait for a byte (a while at a time: a pending exception, from a
       * signal or a scheduled callback, is raised meanwhile)
       */

      mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
      input_take(500);
    }

  c = p->ring[p->head];
  p->head = (p->head + 1) % RING_SIZE;
  p->count--;
  return c == '\n' ? '\r' : c;
}

uintptr_t mp_hal_stdio_poll(uintptr_t poll_flags)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  uintptr_t ret = 0;

  if (poll_flags & MP_STREAM_POLL_RD)
    {
      if (p->count == 0)
        {
          input_take(0);
        }

      if (p->count > 0 || p->eof)
        {
          ret |= MP_STREAM_POLL_RD;
        }
    }

  if (poll_flags & MP_STREAM_POLL_WR)
    {
      ret |= MP_STREAM_POLL_WR;
    }

  return ret;
}

static void write_all(int fd, const char *str, size_t len)
{
  while (len > 0)
    {
      ssize_t n = write(fd, str, len);

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return;
        }

      str += n;
      len -= n;
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len)
{
  write_all(STDOUT_FILENO, str, len);
  return len;
}

/* The terminal turns \n into \r\n (OPOST, ONLCR): cooked is plain */

void mp_hal_stdout_tx_strn_cooked(const char *str, size_t len)
{
  write_all(STDOUT_FILENO, str, len);
}

void mp_hal_stdout_tx_str(const char *str)
{
  write_all(STDOUT_FILENO, str, strlen(str));
}

static void stderr_print_strn(void *env, const char *str, size_t len)
{
  (void)env;
  write_all(STDERR_FILENO, str, len);
}

const mp_print_t mp_stderr_print =
{
  NULL, stderr_print_strn
};

/****************************************************************************
 * Public Functions: time
 ****************************************************************************/

mp_uint_t mp_hal_ticks_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (mp_uint_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

mp_uint_t mp_hal_ticks_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (mp_uint_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

uint64_t mp_hal_time_ns(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Waiting: on the input, so that Ctrl-C ends it at once */

void mp_hal_delay_ms(mp_uint_t ms)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  mp_uint_t start = mp_hal_ticks_ms();

  for (; ; )
    {
      mp_uint_t done = mp_hal_ticks_ms() - start;

      mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
      if (done >= ms)
        {
          break;
        }

      p->lastpoll = mp_hal_ticks_ms();
      input_take((int)(ms - done < 100 ? ms - done : 100));
    }
}

void mp_hal_delay_us(mp_uint_t us)
{
  if (us >= 10000)
    {
      mp_hal_delay_ms(us / 1000);
    }
  else
    {
      usleep(us);
    }
}

void mp_nuttx_wfe(int ms)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  p->lastpoll = mp_hal_ticks_ms();
  input_take(ms < 0 || ms > POLL_MS ? POLL_MS : ms);
}

/****************************************************************************
 * Public Functions: the rest
 ****************************************************************************/

void mp_hal_get_random(size_t n, uint8_t *buf)
{
  arc4random_buf(buf, n);
}

unsigned long mp_nuttx_random_seed(void)
{
  unsigned long r;

  arc4random_buf(&r, sizeof(r));
  return r ^ mp_hal_ticks_us();
}

void mp_hal_set_interrupt_char(int c)
{
  mp_nuttx_port()->interrupt_char = c;
}

/* The interpreters: each task's state, found by its pid.  Called before
 * the task uses any (mp_nuttx_port() being const, it is not called again
 * in a function that has called it).
 */

struct mp_nuttx_port_s *mp_nuttx_port(void)
{
  pid_t pid = getpid();
  int i;

  for (i = 0; i < MAX_INTERPRETERS; i++)
    {
      if (g_interp[i].pid == pid)
        {
          return g_interp[i].port;
        }
    }

  return NULL;
}

/* A new interpreter for this task: its state (the port's, then
 * MicroPython's, all zero), or NULL if there are as many as there can be
 */

void *mp_nuttx_attach(size_t ctxsize)
{
  struct mp_nuttx_port_s *p = NULL;
  pid_t pid = getpid();
  int i;

  pthread_mutex_lock(&g_interplock);
  for (i = 0; i < MAX_INTERPRETERS; i++)
    {
      /* Free, or left by a task that went without a word (deleted) */

      if (g_interp[i].pid == 0 || g_interp[i].pid == pid ||
          kill(g_interp[i].pid, 0) < 0)
        {
          free(g_interp[i].port);
          p = calloc(1, MP_NUTTX_PORT_SIZE + ctxsize);
          g_interp[i].port = p;
          g_interp[i].pid  = p != NULL ? pid : 0;
          break;
        }
    }

  pthread_mutex_unlock(&g_interplock);
  if (p != NULL)
    {
      p->interrupt_char   = -1;
      p->pyexec_mode = PYEXEC_MODE_FRIENDLY_REPL;
    }

  return p;
}

void mp_nuttx_detach(void)
{
  pid_t pid = getpid();
  int i;

  pthread_mutex_lock(&g_interplock);
  for (i = 0; i < MAX_INTERPRETERS; i++)
    {
      if (g_interp[i].pid == pid)
        {
          free(g_interp[i].port);
          g_interp[i].port = NULL;
          g_interp[i].pid  = 0;
        }
    }

  pthread_mutex_unlock(&g_interplock);
}
