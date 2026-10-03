/****************************************************************************
 * apps/graphics/deskapps/monitor_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * System Monitor, the desktop's application, as a program of its own
 * (desk_program.c): its file, built on its own.
 ****************************************************************************/

#include "desk_monitor.c"

int main(int argc, FAR char *argv[])
{
  return desk_program_main(APP_MONITOR, &g_app_monitor, argc, argv);
}
