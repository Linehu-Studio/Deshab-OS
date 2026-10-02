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

    /* U3: @sealed 段标记 —— 用户代码可把静态数据放进 .sealed section
     *（__attribute__((section(".sealed")))），UTSM 侧可遍历
     * [__sealed_start, __sealed_end) 对该区建立封缄段。未标记数据 0 开销。 */
    .sealed : ALIGN(4K) {
        __sealed_start = .;
        *(.sealed)
        *(.sealed.*)
        __sealed_end = .;
    } :data

    .bss : ALIGN(4K) {
        *(.bss*)
    } :data
}
