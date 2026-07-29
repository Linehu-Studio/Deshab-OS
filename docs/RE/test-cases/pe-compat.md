# PE/EXE 兼容层测试矩阵

> 对应计划 Phase B：PE32+ (64位 native) 与 PE32 (32位 x86emu32) 兼容层验证
> 入口：cmd.elf 通过 `ctx->reserved[4]` 取 pe_service

## 状态: ⬜ PENDING

## 测试程序

| 文件 | 类型 | 来源 | FAT32 8.3 名 | 路径 |
|---|---|---|---|---|
| hello64.exe | PE32+ (64位) | MinGW/MSVC 编译或 Python 生成 | `HELLO64 EXE` | SYSTEM/system/deshab64/tools/ |
| hello32.exe | PE32 (32位) | MinGW 交叉编译或 Python 生成 | `HELLO32 EXE` | SYSTEM/system/deshab64/tools/ |

## 测试矩阵

| # | 测试项 | 命令 | 预期输出 | 状态 | Bug# |
|---|---|---|---|---|---|
| B2.1 | PE32+ 加载执行 | `pe HELLO64.EXE` | `Hello from PE32+!` | ⬜ | |
| B2.2 | PE32 加载执行（解释器） | `pe HELLO32.EXE` | `Hello from PE32!` | ⬜ | |
| B2.3 | PE 头解析 | `peinfo HELLO64.EXE` | `machine=0x8664 entry=0x... image_base=0x...` | ⬜ | |
| B2.4 | 文件不存在错误路径 | `pe NONEXIST.EXE` | `Error: file not found` | ⬜ | |
| B2.5 | 无参用法提示 | `pe` | 显示用法 | ⬜ | |
| B2.6 | ExitProcess 返回 | (执行后) | cmd 接管，longjmp 跳回 pe_service_run | ⬜ | |
| B2.7 | PE32 头解析 | `peinfo HELLO32.EXE` | `machine=0x14c entry=0x... image_base=0x00400000` | ⬜ | |

## 关键 ABI 验证

| ABI | 验证方法 | 状态 |
|---|---|---|
| `ctx->reserved[4]` 持 pe_service* | [cmd/main.c:825](../../CODE/cmd/main.c#L825) | ⬜ |
| `PE_SERVICE_MAGIC` 校验 | [cmd/main.c:830](../../CODE/cmd/main.c#L830) | ⬜ |
| `g_pe_svc` 缓存 | [cmd/main.c:108](../../CODE/cmd/main.c#L108) | ⬜ |
| PE32+ IAT 填 ms_abi shim | [pe_loader.c](../../CODE/UTSM/pe/pe_loader.c) | ⬜ |
| PE32 IAT 填合成地址 0x00010000\|idx | [pe_loader.c](../../CODE/UTSM/pe/pe_loader.c) | ⬜ |
| x86emu32 拦截 CALL 合成地址 | [x86emu32.c](../../CODE/UTSM/pe/x86emu32.c) | ⬜ |

## 测试程序源码

### hello64.c (PE32+)

```c
#include <windows.h>
int main(void) {
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "Hello from PE32+!\r\n", 19, NULL, NULL);
    return 0;
}
```

编译：`x86_64-w64-mingw32-gcc -o hello64.exe hello64.c` 或 `cl /Fe:hello64.exe hello64.c`

### hello32.c (PE32)

```c
#include <windows.h>
int main(void) {
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "Hello from PE32!\r\n", 18, NULL, NULL);
    return 0;
}
```

编译：`i686-w64-mingw32-gcc -o hello32.exe hello32.c -Wl,--image-base=0x00400000`

### Fallback: Python 生成器

若无 MinGW/MSVC，使用 [CODE/tools/pe-samples/gen_pe.py](../../CODE/tools/pe-samples/gen_pe.py) 生成最小 PE 程序。

## 已知限制

- pe_shim 实现的 Win32 API 子集有限，测试程序仅使用 WriteFile + ExitProcess
- x86emu32 指令覆盖率参见 [x86emu32-coverage.md](x86emu32-coverage.md)
- TLS / IFUNC / lazy binding 不支持（DKM ABI 约束）
