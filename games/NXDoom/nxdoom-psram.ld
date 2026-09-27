/* The game's objects linked into one (ld -r, CONFIG_GAMES_NXDOOM_RESTART):
 * its data and bss each one block, which i_main.c restores at every start.
 * CONFIG_GAMES_NXDOOM_STATE_PSRAM: in the sections a board's linker script
 * puts in external RAM.
 */

SECTIONS
{
  .psram_data.nxdoom :
  {
    __nxdoom_data_start = .;
    *(.data .data.*)
    . = ALIGN(8);
    __nxdoom_data_end = .;
  }

  .psram_bss.nxdoom (NOLOAD) :
  {
    __nxdoom_bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(8);
    __nxdoom_bss_end = .;
  }
}
