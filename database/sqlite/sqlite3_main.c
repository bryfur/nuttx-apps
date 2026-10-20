/****************************************************************************
 * apps/database/sqlite/sqlite3_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FPC: the sqlite3 shell (build/shell.c, SQLite's own) with SQLite shut
 * down first.  In a flat build a program built in keeps its data from one
 * run to the next, so SQLite would still be initialised and the shell's
 * sqlite3_config() calls refused ("attempt to configure SQLite after
 * initialization"); sqlite3_shutdown() of an SQLite never initialised does
 * nothing.
 *
 ****************************************************************************/

#include "build/sqlite3.h"

#pragma push_macro("main")
#undef main
#define main sqlite3_shell_main
#include "build/shell.c"
#undef main
#pragma pop_macro("main")

int main(int argc, char *argv[])
{
  sqlite3_shutdown();
  return sqlite3_shell_main(argc, argv);
}
