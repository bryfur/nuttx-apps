/****************************************************************************
 * apps/devtools/xcc/xcc_marks.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The start and end of a program's variables (xcc.ld places these
 * sections around them): xcc_nuttx.c's snapshot.  XCC_MARK names the
 * program (wcc, cc1...): its markers are <XCC_MARK>_data_start...
 *
 ****************************************************************************/

#define STR_(x) #x
#define STR(x) STR_(x)
#define MARK(name) STR(XCC_MARK) "_" name

__asm__(
  "  .section .xcc.data.0,\"aw\",%progbits\n"
  "  .globl " MARK("data_start") "\n"
  MARK("data_start") ":\n"
  "  .section .xcc.data.9,\"aw\",%progbits\n"
  "  .globl " MARK("data_end") "\n"
  MARK("data_end") ":\n"
  "  .section .xcc.bss.0,\"aw\",%nobits\n"
  "  .globl " MARK("bss_start") "\n"
  MARK("bss_start") ":\n"
  "  .section .xcc.bss.9,\"aw\",%nobits\n"
  "  .globl " MARK("bss_end") "\n"
  MARK("bss_end") ":\n"
  "  .text\n");
