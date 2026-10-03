/* apps/games/quake/quake.ld (ld -r)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The game's data and bss as one block each, which fpc_quake.c puts back
 * as built at every start, in sections the board's linker script places
 * in the PSRAM (.psram_data.*, .psram_bss.*); .sram_bss.quake (FPCQ_SRAM)
 * goes on to the SRAM's .bss
 */

SECTIONS
{
  .psram_data.quake :
  {
    __quake_data_start = .;
    *(.data .data.*)
    . = ALIGN(8);
    __quake_data_end = .;
  }

  .psram_bss.quake (NOLOAD) :
  {
    __quake_bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(8);
    __quake_bss_end = .;
  }
}
