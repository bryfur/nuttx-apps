/* apps/devtools/xcc/nuttx/crt0.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The start of an xcc-built NuttX module (e_entry: the loader calls it as
 * main): the constructors, main, exit.
 */

extern int main(int argc, char *argv[]);
extern void exit(int status);
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

void _start(int argc, char *argv[])
{
  for (void (**pp)(void) = __init_array_start; pp < __init_array_end; ++pp)
    (*pp)();
  exit(main(argc, argv));
}
