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
 * Each python task is an interpreter of its own (the fork's "several
 * interpreters"): its state (MicroPython's and this port's) is found by
 * the stack it runs on, else by its pid.
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/kmalloc.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "py/gc.h"
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
#define SRAM_RESERVE     (32 * 1024)      /* Left to the system */
#define HEAP_HDR         16               /* An area's size, before it (a
                                           * GC block: the area aligned) */
#define HEAP_RESERVE     (256 * 1024)     /* Left in the C heap's largest
                                           * free block by a new area */

/* In the SRAM: what the VM looks at all the time (not the PSRAM, through
 * the XIP cache that the code in the flash uses too)
 */

#ifdef __arm__
#  define SRAM_BSS       \
     __attribute__((section(".sram_bss.mpy,\"aw\",%nobits@")))
#  define GET_SP(sp)     __asm__ ("mov %0, sp" : "=r" (sp))
#else
#  define SRAM_BSS
#  define GET_SP(sp)     ((sp) = (uintptr_t)__builtin_frame_address(0))
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

int mp_nuttx_vm_count SRAM_BSS;

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The interpreters running: each task's state, and the stack its
 * interpreter runs on (once it runs: mp_nuttx_stack()).  The stack pointer
 * says which interpreter is running, whichever thread or CPU it is, and
 * without asking NuttX (by getpid() it was up to 40 % of a function call's
 * time, docs/python.md).
 *
 * Changed under g_interplock, read without it: the count is odd while
 * g_interp changes, and a reader that sees it change (or odd) asks again
 * by the pid.  A stack's range is taken away when its interpreter ends;
 * one left by a task deleted while it ran is taken away when a new
 * interpreter's stack overlaps it (only an interpreter asks).
 */

static struct
{
  uintptr_t base;                      /* The stack, from here */
  uintptr_t size;                      /* this long (0: none yet) */
  struct mp_nuttx_port_s *port;
  pid_t pid;
} g_interp[MAX_INTERPRETERS] SRAM_BSS;

static volatile uint32_t g_interpseq SRAM_BSS;
static pthread_mutex_t g_interplock = PTHREAD_MUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* An interpreter's state (the port's and MicroPython's, 1.4 KB): in the
 * SRAM while the kernel's heap has room for it and SRAM_RESERVE more (a
 * flat build: the kernel's heap is the SRAM on the board), else in the
 * PSRAM.  The VM uses it all the time: from the PSRAM, through the XIP
 * cache, a method call was 40 % slower.
 */

static void *state_alloc(size_t size)
{
#if defined(CONFIG_MM_KERNEL_HEAP) && !defined(CONFIG_BUILD_KERNEL) && \
    !defined(CONFIG_BUILD_PROTECTED)
  if (kmm_mallinfo().mxordblk >= size + SRAM_RESERVE)
    {
      void *p = kmm_zalloc(size);

      if (p != NULL)
        {
          return p;
        }
    }
#endif

  return calloc(1, size);
}

static void state_free(struct mp_nuttx_port_s *p)
{
  if (p == NULL)
    {
      return;
    }

  free(p->fpc);
#if defined(CONFIG_MM_KERNEL_HEAP) && !defined(CONFIG_BUILD_KERNEL) && \
    !defined(CONFIG_BUILD_PROTECTED)
  if (kmm_heapmember(p))
    {
      kmm_free(p);
      return;
    }
#endif

  free(p);
}

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
      /* Ctrl-C; or Esc alone (the last byte read: a key's sequence
       * comes in one piece) while the agent asks for it
       */

      if (p->interrupt_char >= 0 &&
          (buf[i] == p->interrupt_char ||
           (buf[i] == 0x1b && p->interrupt_esc > 0 && i == n - 1)))
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
 * Public Functions: the heap
 ****************************************************************************/

/* The GC heap's areas (MICROPY_GC_SPLIT_HEAP_AUTO): from the C heap, each
 * with its size before it, counted in the interpreter's heap_total (main.c
 * counts the first area, which it allocates and frees itself)
 */

void *mp_nuttx_heap_alloc(size_t size)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  size_t *h = malloc(size + HEAP_HDR);

  if (h == NULL)
    {
      return NULL;
    }

  h[0] = size + HEAP_HDR;
  p->heap_total += size + HEAP_HDR;
  return (char *)h + HEAP_HDR;
}

void mp_nuttx_heap_free(void *ptr)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  size_t *h = (size_t *)((char *)ptr - HEAP_HDR);

  p->heap_total -= h[0];
  free(h);
}

/* The largest new area the heap may have: what the interpreter's most
 * leaves, and what the C heap has in one piece (less HEAP_RESERVE, for
 * the rest of the system)
 */

size_t gc_get_max_new_split(void)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  struct mallinfo mi = mallinfo();
  size_t room = p->heap_max > p->heap_total + HEAP_HDR ?
                p->heap_max - p->heap_total - HEAP_HDR : 0;
  size_t piece = (size_t)mi.mxordblk > HEAP_RESERVE + HEAP_HDR ?
                 (size_t)mi.mxordblk - HEAP_RESERVE - HEAP_HDR : 0;

  return room < piece ? room : piece;
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

/* The interpreters: each task's state, found by the stack it runs on,
 * else by its pid.  Called before the task uses any (mp_nuttx_port() being
 * const, it is not called again in a function that has called it).
 */

static void interp_begin(void)
{
  g_interpseq++;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void interp_end(void)
{
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  g_interpseq++;
}

static __attribute__((noinline)) struct mp_nuttx_port_s *port_by_pid(void)
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

struct mp_nuttx_port_s *mp_nuttx_port(void)
{
  uint32_t seq = g_interpseq;
  uintptr_t sp;
  int i;

  GET_SP(sp);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  for (i = 0; i < MAX_INTERPRETERS; i++)
    {
      if (sp - g_interp[i].base < g_interp[i].size)
        {
          struct mp_nuttx_port_s *p = g_interp[i].port;

          __atomic_thread_fence(__ATOMIC_ACQUIRE);
          if (g_interpseq == seq && (seq & 1) == 0)
            {
              return p;
            }

          break;
        }
    }

  return port_by_pid();
}

/* The stack this task's interpreter runs on (base NULL: it no longer
 * does), for mp_nuttx_port()
 */

void mp_nuttx_stack(void *base, size_t size)
{
  uintptr_t b = (uintptr_t)base;
  pid_t pid = getpid();
  int i;

  pthread_mutex_lock(&g_interplock);
  interp_begin();
  for (i = 0; i < MAX_INTERPRETERS; i++)
    {
      if (g_interp[i].pid == pid)
        {
          g_interp[i].base = b;
          g_interp[i].size = base != NULL ? size : 0;
        }
      else if (base != NULL && g_interp[i].base < b + size &&
               b < g_interp[i].base + g_interp[i].size)
        {
          /* Left by a task that is gone: it was where this stack is */

          g_interp[i].size = 0;
        }
    }

  interp_end();
  pthread_mutex_unlock(&g_interplock);
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

  /* The stacks of tasks that went without a word (deleted) are theirs no
   * longer: this task's may be where one was
   */

  interp_begin();
  for (i = 0; i < MAX_INTERPRETERS; i++)
    {
      if (g_interp[i].pid != 0 &&
          (g_interp[i].pid == pid || kill(g_interp[i].pid, 0) < 0))
        {
          g_interp[i].size = 0;
        }
    }

  interp_end();
  for (i = 0; i < MAX_INTERPRETERS; i++)
    {
      /* Free, or left by a task that went without a word (deleted) */

      if (g_interp[i].pid == 0 || g_interp[i].pid == pid ||
          kill(g_interp[i].pid, 0) < 0)
        {
          state_free(g_interp[i].port);
          p = state_alloc(MP_NUTTX_PORT_SIZE + ctxsize);
          interp_begin();
          g_interp[i].size = 0;
          g_interp[i].port = p;
          g_interp[i].pid  = p != NULL ? pid : 0;
          interp_end();
          break;
        }
    }

  pthread_mutex_unlock(&g_interplock);
  if (p != NULL)
    {
      p->interrupt_char   = -1;
      p->interrupt_esc    = -1;
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
          struct mp_nuttx_port_s *p = g_interp[i].port;

          interp_begin();
          g_interp[i].size = 0;
          g_interp[i].port = NULL;
          g_interp[i].pid  = 0;
          interp_end();
          state_free(p);
        }
    }

  pthread_mutex_unlock(&g_interplock);
}
