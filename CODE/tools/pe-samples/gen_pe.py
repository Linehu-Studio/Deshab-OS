#!/usr/bin/env python3
"""
gen_pe.py — Deshab PE 兼容层测试 PE 程序生成器（无需 MinGW/MSVC）

用途: 当宿主机无 MinGW/MSVC 时，用纯 Python 构造最小 PE32+ 和 PE32 二进制，
     验证 pe_loader 解析与 pe_shim/x86emu32 执行路径。

构造的 PE 特征:
  - PE32+ (64位): machine=0x8664, entry=main, image_base=0x140000000
  - PE32  (32位): machine=0x014c, entry=main, image_base=0x00400000
  - 单 section (.text), 4KB 对齐
  - IAT 仅 2 项: msvcrt.dll!printf + kernel32.dll!ExitProcess
  - main 函数: 调用 printf("Hello from PE32+/PE32!\\r\\n") → 调用 ExitProcess(0)

输出:
  hello64.exe  - PE32+ 二进制 (供 HELLO64 EXE 测试用)
  hello32.exe  - PE32  二进制 (供 HELLO32 EXE 测试用)

用法:
  python gen_pe.py
  # 生成 hello64.exe / hello32.exe 到当前目录
  # 之后复制到 SYSTEM/system/deshab64/tools/ 并重新运行 build.bat
"""

import struct
import sys
import os

# ============================================================================
# PE32+ (64位) 构造
# ============================================================================

IMAGE_DOS_SIGNATURE = 0x5A4D       # 'MZ'
IMAGE_NT_SIGNATURE = 0x00004550    # 'PE\0\0'
IMAGE_FILE_MACHINE_I386 = 0x014c
IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_MACHINE_I386_CHARACTERISTICS = 0x0102  # EXECUTABLE_IMAGE | 32BIT_MACHINE
IMAGE_FILE_MACHINE_AMD64_CHARACTERISTICS = 0x0022  # EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE
IMAGE_SUBSYSTEM_WINDOWS_CUI = 3
IMAGE_SCN_CNT_CODE = 0x20
IMAGE_SCN_MEM_EXECUTE = 0x20000000
IMAGE_SCN_MEM_READ = 0x40000000
IMAGE_DIRECTORY_ENTRY_IMPORT = 1


def build_pe32_plus():
    """构造 PE32+ (64位) 测试程序。"""
    image_base = 0x140000000
    section_align = 0x1000
    file_align = 0x200

    # DOS header (64 bytes) + e_lfanew
    dos_header = bytearray(64)
    struct.pack_into('<H', dos_header, 0, IMAGE_DOS_SIGNATURE)  # e_magic
    dos_header[2:60] = b'\x00' * 58
    struct.pack_into('<I', dos_header, 60, 64)  # e_lfanew = 64 (PE header at offset 64)

    # PE signature
    pe_sig = struct.pack('<I', IMAGE_NT_SIGNATURE)

    # COFF file header (20 bytes)
    num_sections = 1
    time_stamp = 0
    sym_table_ptr = 0
    num_symbols = 0
    optional_header_size = 240  # PE32+ optional header size
    characteristics = IMAGE_FILE_MACHINE_AMD64_CHARACTERISTICS
    coff_header = struct.pack('<HHIIIHH',
        IMAGE_FILE_MACHINE_AMD64,
        num_sections,
        time_stamp,
        sym_table_ptr,
        num_symbols,
        optional_header_size,
        characteristics)

    # PE32+ Optional header (240 bytes)
    # 布局：
    #   Magic (2) = 0x20b (PE32+)
    #   MajorLinkerVersion (1) = 0
    #   MinorLinkerVersion (1) = 0
    #   SizeOfCode (4) = .text 大小
    #   SizeOfInitializedData (4) = 0
    #   SizeOfUninitializedData (4) = 0
    #   AddressOfEntryPoint (4) = main RVA
    #   BaseOfCode (4) = 0x1000
    #   ImageBase (8) = 0x140000000
    #   SectionAlignment (4) = 0x1000
    #   FileAlignment (4) = 0x200
    #   MajorOperatingSystemVersion (2) = 6
    #   MinorOperatingSystemVersion (2) = 0
    #   MajorImageVersion (2) = 0
    #   MinorImageVersion (2) = 0
    #   MajorSubsystemVersion (2) = 6
    #   MinorSubsystemVersion (2) = 0
    #   Win32VersionValue (4) = 0
    #   SizeOfImage (4) = 0x3000
    #   SizeOfHeaders (4) = 0x200
    #   CheckSum (4) = 0
    #   Subsystem (2) = IMAGE_SUBSYSTEM_WINDOWS_CUI = 3
    #   DllCharacteristics (2) = 0
    #   SizeOfStackReserve (8) = 0x100000
    #   SizeOfStackCommit (8) = 0x1000
    #   SizeOfHeapReserve (8) = 0x100000
    #   SizeOfHeapCommit (8) = 0x1000
    #   LoaderFlags (4) = 0
    #   NumberOfRvaAndSizes (4) = 16
    #   DataDirectory[16] (8*16 = 128 bytes)

    # .text 段机器码（x86_64）：
    #   main:
    #     lea rcx, [rip + msg]          ; 48 8D 0D XX XX XX XX  (printf arg1 = msg)
    #     call [rip + printf_iat]        ; FF 15 XX XX XX XX     (call IAT[printf])
    #     xor ecx, ecx                  ; 31 C9                  (ExitProcess arg1 = 0)
    #     call [rip + exitproc_iat]     ; FF 15 XX XX XX XX     (call IAT[ExitProcess])
    #     ret                           ; C3
    #   msg: "Hello from PE32+!\r\n\0"
    msg = b"Hello from PE32+!\r\n\0"
    # 占位用，后面填正确偏移
    code = bytearray()
    code += b'\x48\x8D\x0D' + b'\x00\x00\x00\x00'  # lea rcx, [rip+0]  (placeholder)
    code += b'\xFF\x15' + b'\x00\x00\x00\x00'       # call [rip+0]      (placeholder)
    code += b'\x31\xC9'                              # xor ecx, ecx
    code += b'\xFF\x15' + b'\x00\x00\x00\x00'       # call [rip+0]      (placeholder)
    code += b'\xC3'                                  # ret
    code += msg

    code_rva = 0x1000  # .text RVA
    main_rva = code_rva
    printf_iat_rva = code_rva + 0x200  # IAT 在 .text 后（同一 section 内，简化）
    # 注意：每个 import descriptor 的 thunk 数组必须 NULL 终止。
    # printf thunk 占 0x200..0x207，0x208..0x20F 为 NULL 终止符，
    # ExitProcess thunk 放 0x210（此前放 0x208 导致 desc0 的 thunk 数组
    # 无 NULL 终止、加载器把 ExitProcess 误配到 msvcrt.dll）。
    exitproc_iat_rva = printf_iat_rva + 16

    # 修正 lea rcx, [rip + msg]: msg 在 code[19] 之后（lea 7字节 + call 6 + xor 2 + call 6 + ret 1 = 22）
    msg_offset_in_code = 22
    lea_rip_offset = msg_offset_in_code - len(code) + len(code)  # 偏移=22-0=22（lea 后下一指令的 RIP）
    # lea rcx, [rip + disp32]: disp32 = msg - (next_instr_rip)
    # next_instr_rip = main_rva + 7
    # msg = main_rva + 22
    # disp32 = 22 - 7 = 15
    struct.pack_into('<i', code, 3, 15)
    # 修正 call [rip + printf_iat]: printf IAT 在 printf_iat_rva, call 后下一指令 RIP = main_rva + 13
    # disp32 = printf_iat_rva - (main_rva + 13) = 0x200 - 13 = 499
    # 注意：call 指令在 code[7..12]（FF 15 + disp32），disp32 位于偏移 9；
    # 此前误写偏移 10 → disp 变为 0x1F300 且覆盖 xor ecx,ecx 的 0x31 → 三重故障。
    struct.pack_into('<i', code, 9, (printf_iat_rva - code_rva) - 13)
    # 修正 call [rip + exitproc]: ExitProcess IAT 在 exitproc_iat_rva, call 后下一指令 RIP = main_rva + 21
    # disp32 = exitproc_iat_rva - (main_rva + 21)
    struct.pack_into('<i', code, 17, (exitproc_iat_rva - code_rva) - 21)

    # IAT 8字节对齐填充
    while len(code) < 0x200:
        code += b'\x00'
    # 注意：以下 RVA 都必须带 code_rva(0x1000) 偏置——此前写成裸缓冲区偏移
    # (0x300/0x320/0x350/0x360/0x400)，导致 import 数据目录指向 headers 区、
    # rva_to_offset 解析失败、IAT 永远不被 shim 修补，call [IAT] 跳到 0x300
    # → #PF → DSK 阶段异常无法投递 → 三重故障（QEMU -d int 实测定案）。
    printf_name_rva = code_rva + 0x300    # 0x1300
    exitproc_name_rva = code_rva + 0x320  # 0x1320
    msvcrt_dll_rva = code_rva + 0x350     # 0x1350
    kernel32_dll_rva = code_rva + 0x360   # 0x1360
    import_desc_rva = code_rva + 0x400    # 0x1400

    # IAT 内容（指向 import 函数名 RVA），每个 descriptor 的 thunk 数组 NULL 终止
    iat_data = bytearray()
    iat_data += struct.pack('<Q', printf_name_rva)    # printf function name RVA (hint+name 表)
    iat_data += struct.pack('<Q', 0)                  # desc0 thunk 数组 NULL 终止
    iat_data += struct.pack('<Q', exitproc_name_rva)  # ExitProcess function name RVA
    iat_data += struct.pack('<Q', 0)                  # desc1 thunk 数组 NULL 终止
    code += iat_data

    # Import descriptor 数据（在 IAT 后）
    while len(code) < 0x300:
        code += b'\x00'

    # Hint/Name 表
    # "msvcrt.dll\0" + printf name "printf\0"
    # "kernel32.dll\0" + ExitProcess name "ExitProcess\0"
    printf_hint_name = struct.pack('<H', 0) + b'printf\0'    # hint=0
    exitproc_hint_name = struct.pack('<H', 0) + b'ExitProcess\0'
    while len(code) < printf_name_rva - code_rva:
        code += b'\x00'
    code += printf_hint_name
    while len(code) < exitproc_name_rva - code_rva:
        code += b'\x00'
    code += exitproc_hint_name

    # DLL 名（放在 import descriptor 之后）
    while len(code) < msvcrt_dll_rva - code_rva:
        code += b'\x00'
    code += b'msvcrt.dll\0'
    while len(code) < kernel32_dll_rva - code_rva:
        code += b'\x00'
    code += b'kernel32.dll\0'

    # Import descriptor (5 * 4 + 4 = 24 bytes for original thunk 这里简化)
    while len(code) < import_desc_rva - code_rva:
        code += b'\x00'
    import_desc = struct.pack('<IIIII',
        printf_iat_rva,   # OriginalFirstThunk
        0,                  # TimeDateStamp
        0,                  # ForwarderChain
        msvcrt_dll_rva,    # Name
        printf_iat_rva)     # FirstThunk
    code += import_desc
    import_desc2 = struct.pack('<IIIII',
        exitproc_iat_rva,
        0, 0,
        kernel32_dll_rva,
        exitproc_iat_rva)
    code += import_desc2
    # NULL 终止 import desc 数组
    code += b'\x00' * 20

    # 对齐到 file_align
    while len(code) % file_align != 0:
        code += b'\x00'

    size_of_code = len(code)
    size_of_image = code_rva + size_of_code
    # 修正 size_of_image 到 section_align
    while size_of_image % section_align != 0:
        size_of_image += 1

    opt_header = bytearray(240)
    struct.pack_into('<H', opt_header, 0, 0x20b)         # Magic = PE32+
    opt_header[2] = 0                                     # MajorLinkerVersion
    opt_header[3] = 0                                     # MinorLinkerVersion
    struct.pack_into('<I', opt_header, 4, size_of_code)  # SizeOfCode
    struct.pack_into('<I', opt_header, 8, 0)             # SizeOfInitializedData
    struct.pack_into('<I', opt_header, 12, 0)            # SizeOfUninitializedData
    struct.pack_into('<I', opt_header, 16, main_rva)     # AddressOfEntryPoint
    struct.pack_into('<I', opt_header, 20, code_rva)      # BaseOfCode
    struct.pack_into('<Q', opt_header, 24, image_base)   # ImageBase
    struct.pack_into('<I', opt_header, 32, section_align)
    struct.pack_into('<I', opt_header, 36, file_align)
    struct.pack_into('<H', opt_header, 40, 6)             # MajorOperatingSystemVersion
    struct.pack_into('<H', opt_header, 42, 0)
    struct.pack_into('<H', opt_header, 44, 0)             # MajorImageVersion
    struct.pack_into('<H', opt_header, 46, 0)
    struct.pack_into('<H', opt_header, 48, 6)             # MajorSubsystemVersion
    struct.pack_into('<H', opt_header, 50, 0)
    struct.pack_into('<I', opt_header, 52, 0)             # Win32VersionValue
    struct.pack_into('<I', opt_header, 56, size_of_image)
    struct.pack_into('<I', opt_header, 60, 0x200)         # SizeOfHeaders
    struct.pack_into('<I', opt_header, 64, 0)             # CheckSum
    struct.pack_into('<H', opt_header, 68, IMAGE_SUBSYSTEM_WINDOWS_CUI)  # Subsystem
    struct.pack_into('<H', opt_header, 70, 0)             # DllCharacteristics
    struct.pack_into('<Q', opt_header, 72, 0x100000)      # SizeOfStackReserve
    struct.pack_into('<Q', opt_header, 80, 0x1000)        # SizeOfStackCommit
    struct.pack_into('<Q', opt_header, 88, 0x100000)      # SizeOfHeapReserve
    struct.pack_into('<Q', opt_header, 96, 0x1000)        # SizeOfHeapCommit
    struct.pack_into('<I', opt_header, 104, 0)            # LoaderFlags
    struct.pack_into('<I', opt_header, 108, 16)           # NumberOfRvaAndSizes

    # DataDirectory[16] = 16 entries * 8 bytes = 128 bytes
    data_dir_offset = 112
    # DataDirectory[1] = Import (RVA, Size)
    struct.pack_into('<II', opt_header, data_dir_offset + 8, import_desc_rva, 48)

    # Section header (.text)
    section_header = bytearray(40)
    section_header[0:8] = b'.text\0\0\0'                  # Name
    struct.pack_into('<I', section_header, 8, size_of_code)  # VirtualSize
    struct.pack_into('<I', section_header, 12, code_rva)  # VirtualAddress
    struct.pack_into('<I', section_header, 16, size_of_code)  # SizeOfRawData
    struct.pack_into('<I', section_header, 20, file_align)  # PointerToRawData
    struct.pack_into('<I', section_header, 24, 0)          # PointerToRelocations
    struct.pack_into('<I', section_header, 28, 0)          # PointerToLinenumbers
    struct.pack_into('<H', section_header, 32, 0)         # NumberOfRelocations
    struct.pack_into('<H', section_header, 34, 0)         # NumberOfLinenumbers
    struct.pack_into('<I', section_header, 36,
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ)

    # 组装文件
    headers = bytearray()
    headers += dos_header
    headers += pe_sig
    headers += coff_header
    headers += opt_header
    headers += section_header

    # 头部对齐到 file_align
    while len(headers) % file_align != 0:
        headers += b'\x00'

    return bytes(headers) + bytes(code)


def build_pe32():
    """构造 PE32 (32位) 测试程序（x86emu32 解释器执行）。

    与 build_pe32_plus 的区别：
      - Machine = 0x014c (i386), Characteristics = 0x0102
      - Optional Header magic = 0x10b, 大小 224 字节
      - 有 BaseOfData 字段；ImageBase 为 4 字节
      - Stack/Heap Reserve/Commit 为 4 字节
      - IAT thunk 为 4 字节（struct '<I'）
      - 机器码为 x86-32，使用 call [abs] 绝对寻址（FF 25 disp32）
    """
    image_base = 0x00400000
    section_align = 0x1000
    file_align = 0x200

    # DOS header (64 bytes) + e_lfanew
    dos_header = bytearray(64)
    struct.pack_into('<H', dos_header, 0, IMAGE_DOS_SIGNATURE)
    dos_header[2:60] = b'\x00' * 58
    struct.pack_into('<I', dos_header, 60, 64)  # e_lfanew

    pe_sig = struct.pack('<I', IMAGE_NT_SIGNATURE)

    # COFF file header (20 bytes) — PE32 optional header = 224 字节
    num_sections = 1
    optional_header_size = 224
    characteristics = IMAGE_FILE_MACHINE_I386_CHARACTERISTICS  # 0x0102
    coff_header = struct.pack('<HHIIIHH',
        IMAGE_FILE_MACHINE_I386,
        num_sections,
        0, 0, 0,
        optional_header_size,
        characteristics)

    # .text 段机器码（x86-32）：
    #   main:
    #     push msg_addr          ; 68 XX XX XX XX   (5)  printf 参数
    #     call [printf_iat_abs]  ; FF 25 XX XX XX XX (6)  通过 IAT 调用
    #     add esp, 4             ; 83 C4 04          (3)  cdecl 清栈
    #     push 0                 ; 6A 00             (2)  ExitProcess 参数
    #     call [exit_iat_abs]    ; FF 25 XX XX XX XX (6)  通过 IAT 调用
    #     ret                    ; C3                (1)
    #   msg: "Hello from PE32!\r\n\0"
    msg = b"Hello from PE32!\r\n\0"
    code_rva = 0x1000
    main_rva = code_rva
    printf_iat_rva = code_rva + 0x200       # 0x1200
    # 注意：每个 import descriptor 的 thunk 数组必须 NULL 终止。
    # printf thunk 占 0x1200，0x1204 为 NULL 终止符，ExitProcess thunk 放 0x1208
    # （此前放 0x1204 导致 desc0 thunk 数组无 NULL 终止，
    #  加载器把 ExitProcess 误配到 msvcrt.dll → "[PE32] unimplemented import"）。
    exitproc_iat_rva = printf_iat_rva + 8    # 0x1208 (4-byte thunk + NULL 间隔)
    printf_name_rva = 0x1300
    exitproc_name_rva = 0x1320
    msvcrt_dll_rva = 0x1350
    kernel32_dll_rva = 0x1360
    import_desc_rva = 0x1400

    msg_offset = 5 + 6 + 3 + 2 + 6 + 1     # = 23
    msg_addr = image_base + code_rva + msg_offset      # 0x401017
    printf_iat_abs = image_base + printf_iat_rva        # 0x401200
    exitproc_iat_abs = image_base + exitproc_iat_rva    # 0x401204

    code = bytearray()
    code += b'\x68' + struct.pack('<I', msg_addr)            # push msg_addr
    code += b'\xFF\x25' + struct.pack('<I', printf_iat_abs)  # call [printf_iat]
    code += b'\x83\xC4\x04'                                   # add esp, 4
    code += b'\x6A\x00'                                       # push 0
    code += b'\xFF\x25' + struct.pack('<I', exitproc_iat_abs)  # call [exitproc_iat]
    code += b'\xC3'                                           # ret
    code += msg

    # 对齐到 IAT (0x200)
    while len(code) < 0x200:
        code += b'\x00'
    # IAT (4-byte thunks，初始值 = hint/name RVA)，每个 descriptor 的 thunk 数组 NULL 终止
    code += struct.pack('<I', printf_name_rva)
    code += struct.pack('<I', 0)                 # desc0 thunk 数组 NULL 终止
    code += struct.pack('<I', exitproc_name_rva)
    code += struct.pack('<I', 0)                 # desc1 thunk 数组 NULL 终止

    # 对齐到 hint/name 表 (0x300)
    while len(code) < printf_name_rva - code_rva:
        code += b'\x00'
    code += struct.pack('<H', 0) + b'printf\0'       # hint=0
    while len(code) < exitproc_name_rva - code_rva:
        code += b'\x00'
    code += struct.pack('<H', 0) + b'ExitProcess\0'

    # DLL 名
    while len(code) < msvcrt_dll_rva - code_rva:
        code += b'\x00'
    code += b'msvcrt.dll\0'
    while len(code) < kernel32_dll_rva - code_rva:
        code += b'\x00'
    code += b'kernel32.dll\0'

    # Import descriptors
    while len(code) < import_desc_rva - code_rva:
        code += b'\x00'
    code += struct.pack('<IIIII',
        printf_iat_rva,     # OriginalFirstThunk (INT)
        0, 0,
        msvcrt_dll_rva,     # Name
        printf_iat_rva)      # FirstThunk (IAT)
    code += struct.pack('<IIIII',
        exitproc_iat_rva,   # OriginalFirstThunk
        0, 0,
        kernel32_dll_rva,   # Name
        exitproc_iat_rva)   # FirstThunk
    code += b'\x00' * 20    # NULL 终止

    # 对齐到 file_align
    while len(code) % file_align != 0:
        code += b'\x00'

    size_of_code = len(code)
    size_of_image = code_rva + size_of_code
    while size_of_image % section_align != 0:
        size_of_image += 1

    # PE32 Optional Header (224 字节)
    opt_header = bytearray(224)
    struct.pack_into('<H', opt_header, 0, 0x10b)          # Magic = PE32
    opt_header[2] = 0                                     # MajorLinkerVersion
    opt_header[3] = 0                                     # MinorLinkerVersion
    struct.pack_into('<I', opt_header, 4, size_of_code)   # SizeOfCode
    struct.pack_into('<I', opt_header, 8, 0)              # SizeOfInitializedData
    struct.pack_into('<I', opt_header, 12, 0)             # SizeOfUninitializedData
    struct.pack_into('<I', opt_header, 16, main_rva)      # AddressOfEntryPoint
    struct.pack_into('<I', opt_header, 20, code_rva)      # BaseOfCode
    struct.pack_into('<I', opt_header, 24, 0)             # BaseOfData (PE32 only)
    struct.pack_into('<I', opt_header, 28, image_base)    # ImageBase (32-bit)
    struct.pack_into('<I', opt_header, 32, section_align)
    struct.pack_into('<I', opt_header, 36, file_align)
    struct.pack_into('<H', opt_header, 40, 6)             # MajorOperatingSystemVersion
    struct.pack_into('<H', opt_header, 42, 0)
    struct.pack_into('<H', opt_header, 44, 0)             # MajorImageVersion
    struct.pack_into('<H', opt_header, 46, 0)
    struct.pack_into('<H', opt_header, 48, 6)             # MajorSubsystemVersion
    struct.pack_into('<H', opt_header, 50, 0)
    struct.pack_into('<I', opt_header, 52, 0)             # Win32VersionValue
    struct.pack_into('<I', opt_header, 56, size_of_image)
    struct.pack_into('<I', opt_header, 60, 0x200)         # SizeOfHeaders
    struct.pack_into('<I', opt_header, 64, 0)             # CheckSum
    struct.pack_into('<H', opt_header, 68, IMAGE_SUBSYSTEM_WINDOWS_CUI)  # Subsystem
    struct.pack_into('<H', opt_header, 70, 0)             # DllCharacteristics
    struct.pack_into('<I', opt_header, 72, 0x100000)      # SizeOfStackReserve (4字节)
    struct.pack_into('<I', opt_header, 76, 0x1000)        # SizeOfStackCommit
    struct.pack_into('<I', opt_header, 80, 0x100000)      # SizeOfHeapReserve
    struct.pack_into('<I', opt_header, 84, 0x1000)        # SizeOfHeapCommit
    struct.pack_into('<I', opt_header, 88, 0)             # LoaderFlags
    struct.pack_into('<I', opt_header, 92, 16)            # NumberOfRvaAndSizes

    # DataDirectory[1] = Import (RVA, Size)
    data_dir_offset = 96
    struct.pack_into('<II', opt_header, data_dir_offset + 8, import_desc_rva, 60)

    # Section header (.text)
    section_header = bytearray(40)
    section_header[0:8] = b'.text\0\0\0'
    struct.pack_into('<I', section_header, 8, size_of_code)   # VirtualSize
    struct.pack_into('<I', section_header, 12, code_rva)       # VirtualAddress
    struct.pack_into('<I', section_header, 16, size_of_code)  # SizeOfRawData
    struct.pack_into('<I', section_header, 20, file_align)    # PointerToRawData
    struct.pack_into('<I', section_header, 24, 0)
    struct.pack_into('<I', section_header, 28, 0)
    struct.pack_into('<H', section_header, 32, 0)
    struct.pack_into('<H', section_header, 34, 0)
    struct.pack_into('<I', section_header, 36,
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ)

    # 组装文件
    headers = bytearray()
    headers += dos_header
    headers += pe_sig
    headers += coff_header
    headers += opt_header
    headers += section_header
    while len(headers) % file_align != 0:
        headers += b'\x00'

    return bytes(headers) + bytes(code)


def build_cmd_exe():
    """构造 cmd.exe — PE32+ (64位) 命令行测试程序。

    与 build_pe32_plus() 类似，仅修改消息文本为 "Hello from cmd.exe!"
    """
    image_base = 0x140000000
    section_align = 0x1000
    file_align = 0x200

    dos_header = bytearray(64)
    struct.pack_into('<H', dos_header, 0, IMAGE_DOS_SIGNATURE)
    dos_header[2:60] = b'\x00' * 58
    struct.pack_into('<I', dos_header, 60, 64)

    pe_sig = struct.pack('<I', IMAGE_NT_SIGNATURE)

    num_sections = 1
    optional_header_size = 240
    characteristics = IMAGE_FILE_MACHINE_AMD64_CHARACTERISTICS
    coff_header = struct.pack('<HHIIIHH',
        IMAGE_FILE_MACHINE_AMD64,
        num_sections,
        0, 0, 0,
        optional_header_size,
        characteristics)

    msg = b"Hello from cmd.exe!\r\n\0"
    code = bytearray()
    code += b'\x48\x8D\x0D' + b'\x00\x00\x00\x00'  # lea rcx, [rip+0]
    code += b'\xFF\x15' + b'\x00\x00\x00\x00'       # call [rip+0]
    code += b'\x31\xC9'                              # xor ecx, ecx
    code += b'\xFF\x15' + b'\x00\x00\x00\x00'       # call [rip+0]
    code += b'\xC3'                                  # ret
    code += msg

    code_rva = 0x1000
    main_rva = code_rva
    printf_iat_rva = code_rva + 0x200
    # 同 build_pe32_plus：thunk 数组 NULL 终止，ExitProcess thunk 放 0x210
    exitproc_iat_rva = printf_iat_rva + 16

    msg_offset_in_code = 22
    struct.pack_into('<i', code, 3, 15)
    # call#1 disp32 位于偏移 9（同 build_pe32_plus 的偏移修正说明）
    struct.pack_into('<i', code, 9, (printf_iat_rva - code_rva) - 13)
    struct.pack_into('<i', code, 17, (exitproc_iat_rva - code_rva) - 21)

    # 同 build_pe32_plus 修复：RVA 必须带 code_rva(0x1000) 偏置
    printf_name_rva = code_rva + 0x300    # 0x1300
    exitproc_name_rva = code_rva + 0x320  # 0x1320
    msvcrt_dll_rva = code_rva + 0x350     # 0x1350
    kernel32_dll_rva = code_rva + 0x360   # 0x1360
    import_desc_rva = code_rva + 0x400    # 0x1400

    while len(code) < 0x200:
        code += b'\x00'
    code += struct.pack('<Q', printf_name_rva)
    code += struct.pack('<Q', 0)       # desc0 thunk 数组 NULL 终止
    code += struct.pack('<Q', exitproc_name_rva)
    code += struct.pack('<Q', 0)       # desc1 thunk 数组 NULL 终止

    while len(code) < 0x300:
        code += b'\x00'
    printf_hint_name = struct.pack('<H', 0) + b'printf\0'
    exitproc_hint_name = struct.pack('<H', 0) + b'ExitProcess\0'
    while len(code) < printf_name_rva - code_rva:
        code += b'\x00'
    code += printf_hint_name
    while len(code) < exitproc_name_rva - code_rva:
        code += b'\x00'
    code += exitproc_hint_name

    while len(code) < msvcrt_dll_rva - code_rva:
        code += b'\x00'
    code += b'msvcrt.dll\0'
    while len(code) < kernel32_dll_rva - code_rva:
        code += b'\x00'
    code += b'kernel32.dll\0'

    while len(code) < import_desc_rva - code_rva:
        code += b'\x00'
    code += struct.pack('<IIIII', printf_iat_rva, 0, 0, msvcrt_dll_rva, printf_iat_rva)
    code += struct.pack('<IIIII', exitproc_iat_rva, 0, 0, kernel32_dll_rva, exitproc_iat_rva)
    code += b'\x00' * 20

    while len(code) % file_align != 0:
        code += b'\x00'

    size_of_code = len(code)
    size_of_image = code_rva + size_of_code
    while size_of_image % section_align != 0:
        size_of_image += 1

    opt_header = bytearray(240)
    struct.pack_into('<H', opt_header, 0, 0x20b)
    opt_header[2] = 0
    opt_header[3] = 0
    struct.pack_into('<I', opt_header, 4, size_of_code)
    struct.pack_into('<I', opt_header, 8, 0)
    struct.pack_into('<I', opt_header, 12, 0)
    struct.pack_into('<I', opt_header, 16, main_rva)
    struct.pack_into('<I', opt_header, 20, code_rva)
    struct.pack_into('<Q', opt_header, 24, image_base)
    struct.pack_into('<I', opt_header, 32, section_align)
    struct.pack_into('<I', opt_header, 36, file_align)
    struct.pack_into('<H', opt_header, 40, 6)
    struct.pack_into('<H', opt_header, 42, 0)
    struct.pack_into('<H', opt_header, 44, 0)
    struct.pack_into('<H', opt_header, 46, 0)
    struct.pack_into('<H', opt_header, 48, 6)
    struct.pack_into('<H', opt_header, 50, 0)
    struct.pack_into('<I', opt_header, 52, 0)
    struct.pack_into('<I', opt_header, 56, size_of_image)
    struct.pack_into('<I', opt_header, 60, 0x200)
    struct.pack_into('<I', opt_header, 64, 0)
    struct.pack_into('<H', opt_header, 68, IMAGE_SUBSYSTEM_WINDOWS_CUI)
    struct.pack_into('<H', opt_header, 70, 0)
    struct.pack_into('<Q', opt_header, 72, 0x100000)
    struct.pack_into('<Q', opt_header, 80, 0x1000)
    struct.pack_into('<Q', opt_header, 88, 0x100000)
    struct.pack_into('<Q', opt_header, 96, 0x1000)
    struct.pack_into('<I', opt_header, 104, 0)
    struct.pack_into('<I', opt_header, 108, 16)

    data_dir_offset = 112
    struct.pack_into('<II', opt_header, data_dir_offset + 8, import_desc_rva, 48)

    section_header = bytearray(40)
    section_header[0:8] = b'.text\0\0\0'
    struct.pack_into('<I', section_header, 8, size_of_code)
    struct.pack_into('<I', section_header, 12, code_rva)
    struct.pack_into('<I', section_header, 16, size_of_code)
    struct.pack_into('<I', section_header, 20, file_align)
    struct.pack_into('<I', section_header, 24, 0)
    struct.pack_into('<I', section_header, 28, 0)
    struct.pack_into('<H', section_header, 32, 0)
    struct.pack_into('<H', section_header, 34, 0)
    struct.pack_into('<I', section_header, 36,
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ)

    headers = bytearray()
    headers += dos_header
    headers += pe_sig
    headers += coff_header
    headers += opt_header
    headers += section_header
    while len(headers) % file_align != 0:
        headers += b'\x00'

    return bytes(headers) + bytes(code)


def build_fault64():
    """构造 fault64.exe — PE32+ 故意 #PF 验收样本（BUG-20260801-005）。

    用途: 验证 DSK 诊断 IDT 的异常投递。机器码:
        movabs rax, 0x400000000000   ; 48 B8 <8B>
        mov    dword [rax], 1        ; C7 00 01 00 00 00
        jmp    $                     ; EB FE (兜底:万一未 fault 原地死循环)
    0x400000000000 是低位规范地址(bit47=0)且远超任何已映射区,
    写入必然 #PF(err=0x2, supervisor write non-present, cr2=0x400000000000)。
    预期串口: "[IDT] exception" + vector=0xe + cr2=0x0000400000000000。
    无导入表(pe_loader 对空 import directory 直接跳过)。
    """
    image_base = 0x140000000
    section_align = 0x1000
    file_align = 0x200

    dos_header = bytearray(64)
    struct.pack_into('<H', dos_header, 0, IMAGE_DOS_SIGNATURE)
    struct.pack_into('<I', dos_header, 60, 64)

    pe_sig = struct.pack('<I', IMAGE_NT_SIGNATURE)

    coff_header = struct.pack('<HHIIIHH',
        IMAGE_FILE_MACHINE_AMD64,
        1, 0, 0, 0, 240,
        IMAGE_FILE_MACHINE_AMD64_CHARACTERISTICS)

    code = bytearray()
    code += b'\x48\xB8' + struct.pack('<Q', 0x400000000000)  # movabs rax, 0x400000000000
    code += b'\xC7\x00' + struct.pack('<I', 1)               # mov dword [rax], 1
    code += b'\xEB\xFE'                                       # jmp $

    while len(code) % file_align != 0:
        code += b'\x00'

    code_rva = 0x1000
    main_rva = code_rva
    size_of_code = len(code)
    size_of_image = code_rva + size_of_code
    while size_of_image % section_align != 0:
        size_of_image += 1

    opt_header = bytearray(240)
    struct.pack_into('<H', opt_header, 0, 0x20b)         # Magic = PE32+
    struct.pack_into('<I', opt_header, 4, size_of_code)  # SizeOfCode
    struct.pack_into('<I', opt_header, 16, main_rva)     # AddressOfEntryPoint
    struct.pack_into('<I', opt_header, 20, code_rva)     # BaseOfCode
    struct.pack_into('<Q', opt_header, 24, image_base)   # ImageBase
    struct.pack_into('<I', opt_header, 32, section_align)
    struct.pack_into('<I', opt_header, 36, file_align)
    struct.pack_into('<H', opt_header, 40, 6)            # MajorOperatingSystemVersion
    struct.pack_into('<H', opt_header, 48, 6)            # MajorSubsystemVersion
    struct.pack_into('<I', opt_header, 56, size_of_image)
    struct.pack_into('<I', opt_header, 60, 0x200)        # SizeOfHeaders
    struct.pack_into('<H', opt_header, 68, IMAGE_SUBSYSTEM_WINDOWS_CUI)
    struct.pack_into('<Q', opt_header, 72, 0x100000)     # SizeOfStackReserve
    struct.pack_into('<Q', opt_header, 80, 0x1000)       # SizeOfStackCommit
    struct.pack_into('<Q', opt_header, 88, 0x100000)     # SizeOfHeapReserve
    struct.pack_into('<Q', opt_header, 96, 0x1000)       # SizeOfHeapCommit
    struct.pack_into('<I', opt_header, 108, 16)          # NumberOfRvaAndSizes
    # DataDirectory 全 0（无导入表）

    section_header = bytearray(40)
    section_header[0:8] = b'.text\0\0\0'
    struct.pack_into('<I', section_header, 8, size_of_code)
    struct.pack_into('<I', section_header, 12, code_rva)
    struct.pack_into('<I', section_header, 16, size_of_code)
    struct.pack_into('<I', section_header, 20, file_align)
    struct.pack_into('<I', section_header, 36,
        IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ)

    headers = bytearray()
    headers += dos_header
    headers += pe_sig
    headers += coff_header
    headers += opt_header
    headers += section_header
    while len(headers) % file_align != 0:
        headers += b'\x00'

    return bytes(headers) + bytes(code)


def main():
    out_dir = os.path.dirname(os.path.abspath(__file__))

    pe32_plus = build_pe32_plus()
    out_path = os.path.join(out_dir, 'hello64.exe')
    with open(out_path, 'wb') as f:
        f.write(pe32_plus)
    print(f"[gen] Generated PE32+: {out_path} ({len(pe32_plus)} bytes)")

    pe32 = build_pe32()
    out_path = os.path.join(out_dir, 'hello32.exe')
    with open(out_path, 'wb') as f:
        f.write(pe32)
    print(f"[gen] Generated PE32:  {out_path} ({len(pe32)} bytes)")

    cmd_exe = build_cmd_exe()
    out_path = os.path.join(out_dir, 'cmd.exe')
    with open(out_path, 'wb') as f:
        f.write(cmd_exe)
    print(f"[gen] Generated cmd.exe: {out_path} ({len(cmd_exe)} bytes)")

    fault64 = build_fault64()
    out_path = os.path.join(out_dir, 'fault64.exe')
    with open(out_path, 'wb') as f:
        f.write(fault64)
    print(f"[gen] Generated fault64.exe: {out_path} ({len(fault64)} bytes)")

    print("[gen] 完成。复制到 SYSTEM/bin/ 并重新运行 build.bat")


if __name__ == '__main__':
    main()
