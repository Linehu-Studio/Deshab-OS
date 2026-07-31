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
    /* .bss 必须分离为独立 NOBITS 段放在 .dynamic 之后，
     * 否则 BSS 占文件空间导致 .elf 膨胀（f32_disk/f32_data 等大缓冲），
     * 与 DSK linker.md 教训一致。仍归 :data PT_LOAD，phdr filesz<memsz。 */
    .bss : ALIGN(4K) {
        *(.bss*)
    } :data
}
