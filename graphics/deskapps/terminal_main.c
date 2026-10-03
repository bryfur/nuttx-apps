/****************************************************************************
 * apps/graphics/deskapps/terminal_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's application as a program of its own (desk_program.c):
 * its file, built on its own; its argument a builtin to run in its tab
 * rather than NSH (desk_term_run()).
 ****************************************************************************/

#include "desk_term.c"

int main(int argc, FAR char *argv[])
{
  g_runcmd = argc > 1 ? argv[1] : NULL;
  return desk_program_main(APP_TERMINAL, &g_app_terminal, argc, argv);
}
