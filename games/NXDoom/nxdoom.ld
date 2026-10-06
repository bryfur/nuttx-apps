/* CONFIG_GAMES_NXDOOM_RESTART (ld -r): the game's data and bss as one
 * block each, which i_main.c restores at every start
 */

SECTIONS
{
  .data.nxdoom :
  {
    __nxdoom_data_start = .;
    *(.data .data.*)
    . = ALIGN(8);
    __nxdoom_data_end = .;
  }

  .bss.nxdoom (NOLOAD) :
  {
    __nxdoom_bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(8);
    __nxdoom_bss_end = .;
  }
}
