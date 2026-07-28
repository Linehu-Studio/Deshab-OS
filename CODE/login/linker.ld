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
    /* 防篡改：导出 .text 段范围供运行时自校验 */
    __text_start = .;
    .text : ALIGN(4K) { *(.text*) } :text
    __text_end = .;

    .rodata : ALIGN(4K) { *(.rodata*) } :rodata

    /* 防篡改：独立的 .hashseg 段存放预期 SHA256（32 字节）。
     * 由构建后补丁脚本 patch_login_hash.exe 写入。
     * 初始填充 0x00，运行时若发现全 0 则跳过校验（开发期便利）。 */
    .hashseg : ALIGN(16) {
        __expected_hash = .;
        . = . + 32;
        __expected_hash_end = .;
    } :rodata

    .data : ALIGN(4K) {
        *(.data*)
        *(.data.rel.ro*)
        *(COMMON)
    } :data
    .bss : ALIGN(4K) {
        *(.bss*)
    } :data
    .rela.dyn : ALIGN(8) { *(.rela.dyn*) } :data
    .dynamic : ALIGN(8) { *(.dynamic*) } :data :dynamic
}
