/****************************************************************************
 * apps/graphics/deskapps/calc_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Calculator, the desktop's application, as a program of its own
 * (desk_program.c): its file, built on its own.
 ****************************************************************************/

#include "desk_calc.c"

int main(int argc, FAR char *argv[])
{
  return desk_program_main(APP_CALC, &g_app_calc, argc, argv);
}
