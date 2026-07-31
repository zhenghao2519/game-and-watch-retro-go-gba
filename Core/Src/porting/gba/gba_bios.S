/* The GBA BIOS, 16 KB, .incbin'd.
 *
 * If roms/gba_bios/gba_bios.bin exists it is embedded at build time (official
 * BIOS, more accurate behaviour). Otherwise the open-source replacement that
 * gpSP ships is used as a fallback.
 *
 * gpSP's own bios_data.S puts it in .data, which on this device means 16 KB of
 * the 724 KB overlay pool spent on something the core only ever reads. Here it is
 * .rodata, so the linker script can hand it to whichever region has the room.
 *
 * The section name is spelled out rather than left to -fdata-sections, because
 * the linker script places this object by name.
 */
  .section .rodata.open_gba_bios_rom, "a", %progbits
  .align 4
  .globl open_gba_bios_rom
  .type open_gba_bios_rom, %object
open_gba_bios_rom:
  /* GBA_BIOS_BIN is set by Makefile to roms/gba_bios/gba_bios.bin if it
   * exists, otherwise to the open-source fallback. */
  .incbin GBA_BIOS_BIN
  .size open_gba_bios_rom, . - open_gba_bios_rom

  .section .note.GNU-stack, "", %progbits
