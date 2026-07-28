ENTRY(dsk_entry)

PHDRS
{
    text    PT_LOAD    FLAGS(5);
    rodata  PT_LOAD    FLAGS(4);
    data    PT_LOAD    FLAGS(6);
    dynamic PT_DYNAMIC FLAGS(4);
}

SECTIONS
{
    . = 0x1000;
    .text : ALIGN(4K) { *(.text*) } :text
    .rodata : ALIGN(4K) { *(.rodata*) } :rodata
    .data : ALIGN(4K) {
        *(.data*)
        *(.data.rel.ro*)
        *(COMMON)
    } :data
    .rela.dyn : ALIGN(8) { *(.rela.dyn*) } :data
    .dynamic : ALIGN(8) { *(.dynamic*) } :data :dynamic
    .bss : ALIGN(4K) {
        *(.bss*)
    } :data
}
