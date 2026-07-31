# Deshab xHCI USB 驱动设计文档

## 1. 概述

xHCI (eXtensible Host Controller Interface) 是 USB 3.0+ 的标准主机控制器接口规范，统一支持 USB 1.1/2.0/3.x 设备，取代 UHCI/OHCI/EHCI。本驱动实现 xHCI 控制器驱动 + USB 大容量存储设备支持，使 Deshab 内核可通过 USB 存储设备（U 盘）读写数据。

### 1.1 驱动定位

```text
驱动名称:  xhci
驱动类:    DKM_CLASS_STORAGE (6)
加载阶段:  stage1 (boot)
依赖:      pci, irq
提供:      block
输出文件:  SYSTEM/driver/block/xhci.drv
```

### 1.2 实现范围

- xHCI 控制器发现、复位、启动
- Command Ring / Event Ring / ERST 管理
- USB 设备端口检测、重置、枚举
- 设备描述符 / 配置描述符读取
- Bulk-Only Transport (BOT) 大容量存储协议
- SCSI Transparent Command Set: INQUIRY / READ CAPACITY / READ(10) / WRITE(10)
- 块设备注册 (kernel_api.block)

### 1.3 不在范围内

- USB 3.x LPM (Link Power Management)
- USB 2.0 TT (Transaction Translator)
- Isochronous / Interrupt 传输
- 多设备并发（首版只枚举第一个 MSC 设备）
- USB Hub（假设设备直连控制器端口）
- xHCI Stream IDs
- MSI/MSI-X 中断（首版使用同步轮询）

---

## 2. xHCI 架构概述

### 2.1 寄存器布局

```text
MMIO Base
├── Capability Registers (0x00 - CAPLEN)
│   ├── HCSPARAMS1: MaxSlots, MaxIntrs, MaxPorts
│   ├── HCSPARAMS2: Scratchpad, MaxERST, IST, SPB
│   ├── HCCPARAMS1: ContextSize, ExtCapPtr
│   ├── DBOFF: Doorbell Register Offset
│   └── RTSOFF: Runtime Register Space Offset
│
├── Operational Registers (CAPLEN - 0x3FF)
│   ├── USBCMD: RS, RST, INTE
│   ├── USBSTS: HCH, EINT, CNR
│   ├── CRCR: Command Ring Control
│   ├── DCBAAP: Device Context Base Array Ptr
│   └── CONFIG: MaxSlotsEn
│
├── Port Registers (0x400 - 0x7FF)
│   └── PORTSC[n]: CCS, PED, PR, PLS, Speed
│
├── Runtime Registers (RTSOFF - )
│   ├── MFINDEX: Microframe Counter
│   └── IR[0]: IMAN, IMOD, ERSTSZ, ERSTBA, ERDP
│
└── Doorbell Registers (DBOFF - )
    └── DB[n]: Doorbell for Slot n
```

### 2.2 数据结构关系

```text
DCBAA (Device Context Base Address Array)
├── [0]: Scratchpad Buffer Array Phys
├── [1]: Device Context 1 Phys
├── [2]: Device Context 2 Phys
└── ...

Device Context
├── Slot Context (32/64 bytes)
└── Endpoint Context[0-31] (32/64 bytes each)
    └── Transfer Ring Dequeue Pointer

Command Ring (Circular)
├── TRB[0]: Enable Slot Command
├── TRB[1]: Address Device Command
├── ...
└── TRB[N-1]: Link TRB → TRB[0]

Event Ring (Circular)
├── TRB[0]: Command Completion Event
├── TRB[1]: Transfer Event
├── TRB[2]: Port Status Change Event
├── ...
└── TRB[N-1]: Link TRB → TRB[0]

Transfer Ring (per Endpoint, Circular)
├── TRB[0]: Setup Stage / Normal
├── TRB[1]: Data Stage / Normal
├── TRB[2]: Status Stage / Normal
├── ...
└── TRB[N-1]: Link TRB → TRB[0]
```

---

## 3. TRB 格式

### 3.1 通用 TRB (16 字节)

```text
Offset  Size  Field
0x00    8     Parameter (数据指针 / 命令参数)
0x08    4     Status (传输长度 / 完成码)
0x0C    4     Control (TRB Type[15:10], Cycle Bit[0], Flags)
```

### 3.2 Cycle Bit 机制

- **Producer Cycle State (PCS)**: 写入方维护的 cycle 值
- **Consumer Cycle State (CCS)**: 读取方维护的 cycle 值
- TRB 有效条件: `TRB.CycleBit == Ring.CCS`
- 环绕时翻转 Cycle State，实现无锁生产者-消费者同步

### 3.3 关键 TRB 类型

| Type | 名称 | 用途 |
|------|------|------|
| 0x01 | Normal | Bulk/Interrupt 数据传输 |
| 0x02 | Setup Stage | Control 传输 Setup 阶段 |
| 0x03 | Data Stage | Control 传输 Data 阶段 |
| 0x04 | Status Stage | Control 传输 Status 阶段 |
| 0x06 | Link | 环链接，指向下一段 |
| 0x09 | Enable Slot | 分配设备 Slot |
| 0x0A | Disable Slot | 释放设备 Slot |
| 0x0B | Address Device | 设置 USB 设备地址 |
| 0x0C | Configure Endpoint | 配置设备端点 |
| 0x17 | No-Op | 空操作 |

### 3.4 事件 TRB 类型

| Type | 名称 | 用途 |
|------|------|------|
| 0x01 | Transfer Event | 传输完成通知 |
| 0x02 | Command Completion | 命令完成通知 |
| 0x04 | Port Status Change | 端口状态变化 |

---

## 4. USB 设备枚举流程

```text
1. 检测端口连接 (PORTSC.CCS = 1)
2. 端口重置 (PORTSC.PR = 1 → 等待 PED = 1)
3. 读取端口速度 (PORTSC.Speed)
4. Enable Slot → 获得 Slot ID
5. 分配 Input Context + Transfer Ring (EP0)
6. Address Device (BSR=0) → 设备获得 USB 地址
7. Control Transfer (EP0): GET_DESCRIPTOR(Device)
8. 重新 Address Device (使用真实 MaxPacketSize)
9. Control Transfer (EP0): GET_DESCRIPTOR(Configuration)
10. Configure Endpoint (Bulk IN/OUT)
11. 设备就绪，可进行 Bulk 传输
```

---

## 5. BOT (Bulk-Only Transport) 协议

### 5.1 CBW (Command Block Wrapper, 31 字节)

```text
Offset  Size  Field
0x00    4     Signature: 0x43425355 ("USBC")
0x04    4     Tag: 递增标签（用于匹配 CSW）
0x08    4     Data Transfer Length: 期望数据字节数
0x0C    1     Flags: 0x80=Device-to-Host (IN), 0x00=Host-to-Device (OUT)
0x0D    1     LUN: 逻辑单元号
0x0E    1     CB Length: 命令块长度 (1-16)
0x0F    16    CB: SCSI 命令块
```

### 5.2 CSW (Command Status Wrapper, 13 字节)

```text
Offset  Size  Field
0x00    4     Signature: 0x53425355 ("USBS")
0x04    4     Tag: 匹配 CBW Tag
0x08    4     Data Residue: 未传输字节数
0x0C    1     Status: 0=OK, 1=Failed, 2=Phase Error
```

### 5.3 传输流程

```text
OUT (Bulk EP): CBW →
IN/OUT (Bulk EP): Data (如有) →
IN (Bulk EP): CSW
```

---

## 6. SCSI 命令集

### 6.1 INQUIRY (0x12)

```text
CB: [0x12, 0x00, 0x00, 0x00, allocation_length, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
Response: 36 字节标准 INQUIRY 数据
  - Byte 0: Peripheral Device Type (0x00 = Direct Access Block)
  - Bytes 8-15: Vendor Identification
  - Bytes 16-31: Product Identification
  - Bytes 32-35: Product Revision Level
```

### 6.2 READ CAPACITY(10) (0x25)

```text
CB: [0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
Response: 8 字节
  - Bytes 0-3: Maximum LBA (big-endian)
  - Bytes 4-7: Block Size (big-endian, typically 512)
```

### 6.3 READ(10) (0x28)

```text
CB: [0x28, 0x00, LBA[3], LBA[2], LBA[1], LBA[0], 0x00, TransferLength[1], TransferLength[0], 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
Direction: Device-to-Host (IN)
Data: TransferLength × BlockSize 字节
```

### 6.4 WRITE(10) (0x2A)

```text
CB: [0x2A, 0x00, LBA[3], LBA[2], LBA[1], LBA[0], 0x00, TransferLength[1], TransferLength[0], 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]
Direction: Host-to-Device (OUT)
Data: TransferLength × BlockSize 字节
```

---

## 7. Context 格式

### 7.1 Slot Context (32 或 64 字节)

```text
DW0: Route String[23:0], Speed[29:27], MTT[31:30]
DW1: Max Packet Size[15:0], Device Address[31:24]
DW2: TT Hub Slot ID[7:0], TT Port Number[15:8], TTT[19:18], Rsv[23:20], Context Entries[31:27]
DW3: Exit Latency[19:0], Slot State[31:27]
```

### 7.2 Endpoint Context (32 或 64 字节)

```text
DW1: EP Type[3:0], Max Packet Size[31:16]
DW2: Max ESIT Payload Lo
DW4: Average TRB Length[15:0], Max Burst[31:24]
DW6: Dequeue Pointer Low (Transfer Ring)
DW7: Dequeue Pointer High
```

### 7.3 Input Context

```text
Offset 0:   Input Control Context (32/64 bytes) - 位图指示哪些 Context 有效
Offset 32/64: Slot Context
Offset 64/128: Endpoint 0 Context
Offset 96/192: Endpoint 1 Context (Bulk OUT)
Offset 128/256: Endpoint 2 Context (Bulk IN)
```

---

## 8. USB 描述符格式

### 8.1 设备描述符 (18 字节)

```text
Offset  Size  Field
0       1     bLength: 18
1       1     bDescriptorType: 1
2       2     bcdUSB: USB 版本 (0x0200 = USB 2.0)
4       1     bDeviceClass: 设备类 (0x08 = Mass Storage)
5       1     bDeviceSubClass
6       1     bDeviceProtocol
7       1     bMaxPacketSize0: EP0 最大包大小 (8/16/32/64)
8       2     idVendor
10      2     idProduct
12      2     bcdDevice
14      1     iManufacturer
15      1     iProduct
16      1     iSerialNumber
17      1     bNumConfigurations
```

### 8.2 配置描述符 (9 字节) + 接口描述符 (9 字节) + 端点描述符 (7 字节×N)

```text
配置描述符:
  bLength=9, bDescriptorType=2, wTotalLength, bNumInterfaces,
  bConfigurationValue, bmAttributes, bMaxPower

接口描述符:
  bLength=9, bDescriptorType=4, bInterfaceNumber, bAlternateSetting,
  bNumEndpoints, bInterfaceClass(0x08=MSC), bInterfaceSubClass(0x06=SCSI),
  bInterfaceProtocol(0x50=BOT)

端点描述符:
  bLength=7, bDescriptorType=5, bEndpointAddress(0x81=Bulk IN, 0x02=Bulk OUT),
  bmAttributes(0x02=Bulk), wMaxPacketSize, bInterval
```

---

## 9. Control Transfer Setup Packet (8 字节)

```text
Offset  Size  Field
0       1     bmRequestType: 数据方向 + 类型 + 接收者
1       1     bRequest: 请求码
2       2     wValue: 参数
4       2     wIndex: 索引
6       2     wLength: 数据长度

GET_DESCRIPTOR(Device):
  bmRequestType = 0x80 (Device-to-Host, Standard, Device)
  bRequest = 0x06 (GET_DESCRIPTOR)
  wValue = 0x0100 (Device Descriptor, Index 0)
  wIndex = 0x0000
  wLength = 0x0012 (18 bytes)

GET_DESCRIPTOR(Configuration):
  bmRequestType = 0x80
  bRequest = 0x06
  wValue = 0x0200 (Configuration Descriptor, Index 0)
  wIndex = 0x0000
  wLength = 0x00FF (255 bytes, 足够包含完整配置)
```

---

## 10. USB Legacy Support (BIOS Handoff)

### 10.1 Extended Capability

```text
HCCPARAMS1[31:16] → ExtCapPtr (以 32-bit 字为单位的偏移)

ExtCap 结构:
  DW0: ID[7:0], Next[15:8] (下一能力偏移，0=结束)
  
  ID=1: USB Legacy Support Capability
    DW0: ID=1, Next
    DW1: USBLEGSUP (BIOS/OS Ownership Semaphore)
      bit[0-15]: BIOS Semaphore (BIOS 写 1 占用)
      bit[16-31]: OS Semaphore (OS 写 1 请求所有权)
    DW2: USBLEGCTLSTS (Legacy Control)
```

### 10.2 Handoff 流程

```text
1. 读取 USBLEGSUP
2. 若 BIOS Semaphore (bit[0]) = 1:
   a. 写 USBLEGSUP: OS Semaphore (bit[16]) = 1
   b. 等待 BIOS Semaphore 清除 (超时 1s)
   c. 写 USBLEGCTLSTS: 清除所有 SMI 触发位
```

---

## 11. 错误处理与安全不变式

### 11.1 安全不变式

1. **不阻塞启动**：任何 xHCI 初始化步骤失败，只记日志，驱动返回 0（成功），不注册块设备
2. **超时保护**：所有硬件等待使用 TSC deadline，无无限循环
3. **Cache 一致性**：写入 DMA 缓冲区后执行 `wbinvd`
4. **DMA 缓冲区**：所有 DMA 内存通过 `kernel_api.dma->alloc_pages` 分配（低 4G，4K 对齐）
5. **端口重置安全**：重置超时后跳过该端口，不阻塞其他端口枚举

### 11.2 Completion Code 映射

| Completion Code | 含义 | 处理 |
|----------------|------|------|
| 0 | Success | 正常完成 |
| 1 | USB Transaction Error | 重试或报错 |
| 2 | Babble Detected | 报错 |
| 3 | USB Ring Stall | 端点 Stall，需 Clear Feature |
| 4 | Stop | 停止 |
| 5 | Short Packet | 部分完成，检查 residue |
| 13 | Event Data | 含事件数据 |
| 22 | Stream Protocol Error | 报错 |

---

## 12. 性能考虑

1. **TRB Ring 大小**：Command Ring 256 TRB，Event Ring 256 TRB，Transfer Ring 256 TRB
2. **Bulk 传输最大包**：USB 2.0 High-Speed = 512 字节，USB 3.0 Super-Speed = 1024 字节
3. **SCSI 传输粒度**：每次 READ(10)/WRITE(10) 最多 16 扇区 (8KB)，与 AHCI 一致
4. **轮询 vs 中断**：首版使用 Event Ring 同步轮询 + TSC deadline，后续升级 MSI-X 中断
5. **多 TRB 链式传输**：大数据传输拆分为多个 Normal TRB，最后一次设置 IOC

---

## 13. QEMU 测试配置

```text
# xHCI 控制器 + USB 大容量存储
-device qemu-xhci,id=xhci0
-device usb-storage,bus=xhci0.0,drive=usb0
-drive id=usb0,if=none,file=usb_test.img,format=raw

# USB 测试镜像创建 (FAT32, 64MB)
# Windows:
# fsutil file createnew usb_test.img 67108864
# 格式化为 FAT32 (需管理员权限)
```

---

## 14. 与现有驱动的对比

| 特性 | AHCI | NVMe | xHCI |
|------|------|------|------|
| PCI Class | 01/06/01 | 01/08/02 | 0C/03/30 |
| 命令模型 | Port CI 命令 | SQ/CQ Doorbell | TRB Ring + Doorbell |
| 中断 | IRQ (PIC) | 轮询 | 轮询 (首版) |
| 数据传输 | PRDT DMA | PRP DMA | USB Bulk Transfer |
| 协议层 | ATA FIS | NVMe 命令 | USB + BOT + SCSI |
| 复杂度 | ~700 行 | ~800 行 | ~1500 行 |
| 块设备名 | ahci0 | nvme0 | usb0 |

---

## 参考资料

1. Intel xHCI Specification Rev 1.2 (2019): https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/extensible-host-controler-interface-usb-xhci.pdf
2. USB Mass Storage Class Bulk-Only Transport Rev 1.0
3. SCSI Primary Commands (SPC-4) / SCSI Block Commands (SBC-3)
4. USB 2.0 Specification
5. USB 3.1 Specification
