ENTRY(_start)

PHDRS
{
    text   PT_LOAD FLAGS(5);    /* R-X */
    rodata PT_LOAD FLAGS(4);    /* R-- */
    data   PT_LOAD FLAGS(7);    /* RWX (SAS-R0) */
}

SECTIONS
{
    . = 0xffffffff80000000;

    .text : ALIGN(4K) {
        *(.text*)
    } :text

    .rodata : ALIGN(4K) {
        *(.limine_requests_start*)
        *(.limine_requests*)
        *(.limine_requests_end*)
        *(.rodata*)
    } :rodata

    .data : ALIGN(4K) {
        *(.data*)
        *(COMMON)
    } :data

    .bss : ALIGN(4K) {
        *(.bss*)
    } :data
}
