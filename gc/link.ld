/*
 * Chibi-Robo! GBA Link - layout of the section injected into main.dol.
 * 0x80672300 is the retail __ArenaLo; tools/patch_dol.py moves ArenaLo up to
 * __cl_end so the game's heap starts after us. Everything (including .bss)
 * is emitted as initialised bytes so the DOL loader zero-fills nothing.
 */
OUTPUT_ARCH(powerpc:common)
ENTRY(cl_frame_hook)
INCLUDE game_symbols.ld

SECTIONS
{
    . = 0x80672300;
    __cl_start = .;
    /* fixed address for outside tools: cl_mailbox == 0x80672300 */
    .mailbox : { KEEP(*(.mailbox)) . = ALIGN(32); }
    .text : {
        KEEP(*(.text.hook))
        *(.text .text.*)
    }
    .rodata : { *(.rodata .rodata.* .sdata2 .sdata2.* .sbss2 .sbss2.*) }
    .data : {
        *(.data .data.* .sdata .sdata.*)
        *(.sbss .sbss.* .bss .bss.* COMMON)
        BYTE(0)
        . = ALIGN(32);
    }
    __cl_end = .;
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.gnu.attributes) *(.note*) }
}
