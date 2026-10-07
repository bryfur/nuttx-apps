/****************************************************************************
 * apps/testing/hang/hang_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * hang MODE [SECONDS]: hang the system on purpose, to test the watchdog.
 *
 *   irq   spin with this CPU's interrupts off (up_irq_save)
 *   crit  spin inside enter_critical_section() (on SMP the other CPU stops
 *         too, at its next critical section)
 *   lock  spin with pre-emption off on this CPU (sched_lock)
 *   prio  a thread per CPU spinning at the highest priority (FIFO)
 *
 * Forever by default (the watchdog must reset the chip, and the next boot
 * say "watchdog" in /proc/chip); with SECONDS it comes back after them (a
 * hang shorter than the watchdog's timeout must not reset).
 ****************************************************************************/

#include <nuttx/config.h>

#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_SMP_NCPUS
#  define CONFIG_SMP_NCPUS 1
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static long g_seconds;                   /* < 0: forever */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Busy for g_seconds (or forever), whatever the interrupts: up_udelay()
 * does not need the tick
 */

static void spin(void)
{
  long ms;

  for (ms = 0; g_seconds < 0 || ms < g_seconds * 1000; ms++)
    {
      up_udelay(1000);
    }
}

static FAR void *prio_thread(FAR void *arg)
{
  spin();
  return NULL;
}

static int hang_prio(void)
{
  pthread_t threads[CONFIG_SMP_NCPUS];
  pthread_attr_t attr;
  struct sched_param sp;
  int i;

  pthread_attr_init(&attr);
  pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
  sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
  pthread_attr_setschedparam(&attr, &sp);

  for (i = 0; i < CONFIG_SMP_NCPUS; i++)
    {
#ifdef CONFIG_SMP
      cpu_set_t cpus;

      CPU_ZERO(&cpus);
      CPU_SET(i, &cpus);
      pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);
#endif
      if (pthread_create(&threads[i], &attr, prio_thread, NULL) != 0)
        {
          fprintf(stderr, "hang: pthread_create failed\n");
          return EXIT_FAILURE;
        }
    }

  for (i = 0; i < CONFIG_SMP_NCPUS; i++)
    {
      pthread_join(threads[i], NULL);
    }

  return EXIT_SUCCESS;
}

static void usage(void)
{
  fprintf(stderr,
          "usage: hang irq|crit|lock|prio [SECONDS]\n"
          "  irq   spin with this CPU's interrupts off\n"
          "  crit  spin inside a critical section\n"
          "  lock  spin with pre-emption off\n"
          "  prio  a thread per CPU at the highest priority\n"
          "forever unless SECONDS: the watchdog should reset the chip\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *mode;
  irqstate_t flags;

  if (argc < 2 || argc > 3)
    {
      usage();
      return EXIT_FAILURE;
    }

  mode      = argv[1];
  g_seconds = argc == 3 ? strtol(argv[2], NULL, 10) : -1;

  if (g_seconds < 0)
    {
      printf("hang: %s, forever, from CPU%d\n", mode, up_cpu_index());
    }
  else
    {
      printf("hang: %s for %ld s, from CPU%d\n", mode, g_seconds,
             up_cpu_index());
    }

  fflush(stdout);
  usleep(200 * 1000);                    /* Let the console have it */

  if (strcmp(mode, "irq") == 0)
    {
      flags = up_irq_save();
      spin();
      up_irq_restore(flags);
    }
  else if (strcmp(mode, "crit") == 0)
    {
      flags = enter_critical_section();
      spin();
      leave_critical_section(flags);
    }
  else if (strcmp(mode, "lock") == 0)
    {
      sched_lock();
      spin();
      sched_unlock();
    }
  else if (strcmp(mode, "prio") == 0)
    {
      if (hang_prio() != EXIT_SUCCESS)
        {
          return EXIT_FAILURE;
        }
    }
  else
    {
      usage();
      return EXIT_FAILURE;
    }

  printf("hang: back\n");
  return EXIT_SUCCESS;
}
