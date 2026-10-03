/****************************************************************************
 * apps/graphics/deskapps/sheet_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's application as a program of its own (desk_program.c):
 * its file, built on its own; its argument a file to open.
 ****************************************************************************/

#include "desk_sheet.c"

int main(int argc, FAR char *argv[])
{
  g_openpath = argc > 1 ? argv[1] : NULL;
  return desk_program_main(APP_SHEET, &g_app_sheet, argc, argv);
}
