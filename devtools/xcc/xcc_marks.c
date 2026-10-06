/****************************************************************************
 * apps/devtools/xcc/xcc_marks.c
 *
 * SPDX-License-Identifier: MIT
 *
 * The start and end of xcc's variables (xcc.ld places these sections
 * around them): xcc_nuttx.c's snapshot.
 *
 ****************************************************************************/

__asm__(
  "  .section .xcc.data.0,\"aw\",%progbits\n"
  "  .globl xcc_data_start\n"
  "xcc_data_start:\n"
  "  .section .xcc.data.9,\"aw\",%progbits\n"
  "  .globl xcc_data_end\n"
  "xcc_data_end:\n"
  "  .section .xcc.bss.0,\"aw\",%nobits\n"
  "  .globl xcc_bss_start\n"
  "xcc_bss_start:\n"
  "  .section .xcc.bss.9,\"aw\",%nobits\n"
  "  .globl xcc_bss_end\n"
  "xcc_bss_end:\n"
  "  .text\n");
