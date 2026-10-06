/****************************************************************************
 * apps/devtools/xcc/wcc_main.c
 *
 * SPDX-License-Identifier: MIT
 *
 * wcc: xcc's C to WebAssembly compiler (xcc/src/wcc/wcc.c, its main()
 * renamed), run as if it were a process of its own.
 *
 ****************************************************************************/

#include "xcc_nuttx.h"

int xcc_wcc_main(int argc, char *argv[]);

XCC_BLOB(wcc);

int main(int argc, char *argv[])
{
  return xcc_run(&wcc_blob, xcc_wcc_main, argc, argv);
}
