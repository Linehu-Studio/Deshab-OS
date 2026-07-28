/* hello64.c — PE32+ (64-bit) 测试程序
 *
 * 用途: 验证 Deshab PE 兼容层 pe_service 对 PE32+ 的加载与原生 Ring0 执行
 *
 * 调用的 shim API (pe_shim.c 已实现):
 *   - msvcrt.dll!printf
 *   - kernel32.dll!ExitProcess
 *
 * 编译 (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -o hello64.exe hello64.c -nostartfiles \
 *       -Wl,--entry,main -Wl,--image-base,0x140000000
 *
 * 编译 (MSVC):
 *   cl /Fe:hello64.exe hello64.c /link /ENTRY:main /BASE:0x140000000
 *
 * 或直接使用 gen_pe.py 生成等价 PE32+ 二进制
 */

#include <windows.h>
#include <stdio.h>

int main(void) {
    printf("Hello from PE32+!\r\n");
    printf("Deshab PE compat layer works.\r\n");
    ExitProcess(0);
    return 0;
}
