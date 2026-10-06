/****************************************************************************
 * apps/devtools/xcc/xcc.ld
 *
 * SPDX-License-Identifier: MIT
 *
 * The relocatable link (ld -r) of one of xcc's programs into a single
 * object: its variables gathered between xcc_marks.c's markers.
 *
 ****************************************************************************/

SECTIONS
{
  .data.xcc :
    {
      *(.xcc.data.0)
      *(.data .data.* .sdata .sdata.*)
      *(.xcc.data.9)
    }

  .bss.xcc :
    {
      *(.xcc.bss.0)
      *(.bss .bss.* .sbss .sbss.* COMMON)
      *(.xcc.bss.9)
    }
}
