/****************************************************************************
 * apps/graphics/deskapps/tetris_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's application as a program of its own (desk_program.c):
 * its file, built on its own.
 ****************************************************************************/

#include "desk_tetris.c"

int main(int argc, FAR char *argv[])
{
  return desk_program_main(APP_TETRIS, &g_app_tetris, argc, argv);
}
