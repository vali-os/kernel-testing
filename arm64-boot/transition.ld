ENTRY(SeedStart)
SECTIONS
{
    . = 0x40080000;
    .text : { *(.text) *(.text.*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.eh_frame*) }
}
