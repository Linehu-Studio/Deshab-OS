/* realdll64.c — PE32+ 真实 DLL 链验证程序
 *
 * 用途: 验证 Deshab PE 兼容层 pe_dll_manager 对 SYSTEM/lib 真实 DLL 的
 *       加载与解析（含转发导出跟随与 DllMain ATTACH）。
 *
 * 验证路径:
 *   A) 静态导入 msvcrt.dll 中非 shim 函数（strcat/strstr/atoi）：
 *      shim 表未命中 → pe_dll_resolve → 真实 msvcrt.dll
 *      （Win10 msvcrt 把这些函数转发到 ucrtbase，验证转发导出跟随）
 *   B) LoadLibraryA("ucrtbase.dll") + GetProcAddress：
 *      未知 DLL → pe_dll_load 真实加载，句柄=镜像基址，
 *      GetProcAddress 按基址解析 strlen/memcpy 并调用验证。
 *
 * 输出: 全部经 shim printf（串口）。锚点格式 "[REALDLL] ..."。
 *
 * 编译 (MinGW-w64):
 *   x86_64-w64-mingw32-gcc -fno-builtin -O1 -o realdll64.exe realdll64.c \
 *       -nostartfiles -Wl,--entry,main -Wl,--image-base,0x140000000
 */

#include <windows.h>
#include <stdio.h>

/* 显式声明：这些函数不在 shim 表内，强制走真实 DLL 解析。
 * -fno-builtin 阻止 gcc 内联展开，保证产生 IAT 导入调用。 */
__declspec(dllimport) char * __cdecl strcat(char *dst, const char *src);
__declspec(dllimport) char * __cdecl strstr(const char *hay, const char *needle);
__declspec(dllimport) int    __cdecl atoi(const char *s);

typedef unsigned long long (__cdecl *strlen_fn)(const char *);
typedef void *(__cdecl *memcpy_fn)(void *, const void *, unsigned long long);

/* MinGW ABI 对 main 强制插入 __main 调用（CRT 静态构造注册）。
 * freestanding（-nostdlib）无 CRT，提供空 stub。 */
void __main(void) {}

static int g_fails = 0;

static void check(const char *name, int ok) {
    printf("[REALDLL] %s: %s\n", name, ok ? "OK" : "FAIL");
    if (!ok) g_fails++;
}

int main(void) {
    printf("[REALDLL] begin\n");

    /* ---- A1: strcat（真实 DLL，Win10 转发到 ucrtbase） ---- */
    char buf[64];
    buf[0] = '\0';
    strcat(buf, "abc");
    strcat(buf, "def");
    check("A1 strcat", buf[0] == 'a' && buf[1] == 'b' && buf[2] == 'c' &&
                        buf[3] == 'd' && buf[4] == 'e' && buf[5] == 'f' &&
                        buf[6] == '\0');

    /* ---- A2: strstr（真实 DLL） ---- */
    const char *hay = "hello deshab world";
    char *hit = strstr(hay, "deshab");
    check("A2 strstr", hit == hay + 6);

    /* ---- A3: atoi（真实 DLL） ---- */
    check("A3 atoi", atoi("12345") == 12345 && atoi("-42") == -42);

    /* ---- B1: LoadLibraryA 真实 DLL ---- */
    HMODULE ucrt = LoadLibraryA("ucrtbase.dll");
    check("B1 LoadLibraryA(ucrtbase.dll)", ucrt != NULL);

    if (ucrt) {
        /* ---- B2: GetProcAddress strlen + 调用 ---- */
        strlen_fn p_strlen = (strlen_fn)GetProcAddress(ucrt, "strlen");
        check("B2 GetProcAddress(strlen)", p_strlen != NULL);
        if (p_strlen) {
            check("B2 strlen call", p_strlen("hello world") == 11);
        }

        /* ---- B3: GetProcAddress memcpy + 调用 ---- */
        memcpy_fn p_memcpy = (memcpy_fn)GetProcAddress(ucrt, "memcpy");
        check("B3 GetProcAddress(memcpy)", p_memcpy != NULL);
        if (p_memcpy) {
            char dst[16];
            for (int i = 0; i < 16; i++) dst[i] = 0;
            p_memcpy(dst, "0123456789", 10);
            check("B3 memcpy call", dst[0] == '0' && dst[9] == '9' && dst[10] == 0);
        }
    }

    printf("[REALDLL] end fails=%d\n", g_fails);
    ExitProcess(g_fails);
    return g_fails;
}
