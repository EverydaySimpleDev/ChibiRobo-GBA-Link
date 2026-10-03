/* Embeds the GBA multiboot image (built by ../gba) into the injected section. */
    .section .rodata.gba_rom, "a"
    .balign 32
    .global cl_gba_rom
cl_gba_rom:
    .incbin GBA_ROM_PATH
    .global cl_gba_rom_end
cl_gba_rom_end:
    .balign 32  /* zero padding: the uploader rounds the size up to 8 bytes */
