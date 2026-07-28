/* hello32.c — PE32 (32-bit) 测试程序
 *
 * 用途: 验证 Deshab PE 兼容层 x86emu32 解释器对 PE32 的加载与执行
 *
 * 调用的 shim API (pe_shim.c 已实现):
 *   - msvcrt.dll!printf
 *   - kernel32.dll!ExitProcess
 *
 * 编译 (MinGW-w64 32位):
 *   i686-w64-mingw32-gcc -o hello32.exe hello32.c -nostartfiles \
 *       -Wl,--entry,_main -Wl,--image-base,0x00400000
 *
 * 编译 (MSVC 32位):
 *   cl /Fe:hello32.exe hello32.c /link /ENTRY:_main /BASE:0x00400000
 *
 * 或直接使用 gen_pe.py 生成等价 PE32 二进制
 */

#include <windows.h>
#include <stdio.h>

int __cdecl main(void) {
    printf("Hello from PE32!\r\n");
    printf("Deshab x86emu32 interpreter works.\r\n");
    ExitProcess(0);
    return 0;
}
