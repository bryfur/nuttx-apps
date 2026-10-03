/****************************************************************************
 * apps/graphics/deskapps/chess_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's application as a program of its own (desk_program.c):
 * its file, built on its own.
 ****************************************************************************/

#include "desk_chess.c"

int main(int argc, FAR char *argv[])
{
  return desk_program_main(APP_CHESS, &g_app_chess, argc, argv);
}
