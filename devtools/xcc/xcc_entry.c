/****************************************************************************
 * apps/devtools/xcc/xcc_entry.c
 *
 * SPDX-License-Identifier: MIT
 *
 * xcc: the native compiler's driver (xcc_main.c, its main() renamed) run
 * as if it were a process of its own; it runs the preprocessor, the
 * compiler, the assembler and the linker the same way.
 *
 ****************************************************************************/

#include "xcc_nuttx.h"

int xcc_xccd_main(int argc, char *argv[]);

XCC_BLOB(xccd);

int main(int argc, char *argv[])
{
  return xcc_run(&xccd_blob, xcc_xccd_main, argc, argv);
}
