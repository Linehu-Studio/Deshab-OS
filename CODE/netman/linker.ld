ENTRY(dsk_entry)

PHDRS
{
    text   PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data   PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x1000;
    .text : ALIGN(4K) { *(.text*) } :text
    .rodata : ALIGN(4K) { *(.rodata*) } :rodata
    .data : ALIGN(4K) { *(.data*) *(COMMON) *(.bss*) } :data
}
