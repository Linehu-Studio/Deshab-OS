﻿#requires -Version 5.1
<#
.SYNOPSIS
    Deshab QEMU 自动化测试公共库（tests/qemu/lib/QemuTest.ps1）

.DESCRIPTION
    提供：
      - QEMU 会话管理（启动 / 计时杀进程 / 串口日志收集）
      - 日志断言引擎（必须匹配 / 禁止匹配 / 顺序匹配 / 计数匹配）
      - QEMU monitor TCP 客户端（sendkey / mouse_move / screendump）
      - 场景准备（firstInit.txt 与 AUTOEXEC.BAT 备份-替换-恢复 + SATA 镜像重建）
      - 用例结果报告（PASS/FAIL/MANUAL/SKIP，失败时输出日志尾部上下文）

    兼容 PowerShell 5.1。所有临时文件写入 .build_tmp\tests\。
#>

# ---------------- 路径 ----------------
$script:RepoRoot    = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$script:BuildTmp    = Join-Path $script:RepoRoot '.build_tmp'
$script:TestWorkDir = Join-Path $script:BuildTmp 'tests'
$script:IsoDir      = Join-Path $script:RepoRoot 'ISO'
$script:SystemDir   = Join-Path $script:RepoRoot 'SYSTEM'
# 并发隔离布局（另一代理并发跑 QEMU/build，共享路径一律只读，写入全进私有区）：
#   .build_tmp\tests\img\        私有镜像副本（deshab/nvme/usb，QEMU 只挂副本）
#   .build_tmp\tests\SYSTEM\     SYSTEM 镜像树（robocopy /MIR；场景文件只翻这里）
#   .build_tmp\tests\CODE\...\deaicup.exe   mkfat32 引用的单文件副本
#   .build_tmp\tests\fat32gen\   mkfat32.ps1 副本 + 私有 SATA 镜像输出
$script:SuiteImgDir  = Join-Path $script:TestWorkDir 'img'
$script:MkFat32Dir   = Join-Path $script:TestWorkDir 'fat32gen'
$script:MirrorSystem = Join-Path $script:TestWorkDir 'SYSTEM'
$script:Config      = $null

# 场景备份（模块级，保证只备份一次、恢复幂等）
$script:FirstInitBackup = $null
$script:AutoexecBackup  = $null
$script:FuckBackup      = $null

# ================================================================
#  环境初始化
# ================================================================

function Initialize-QemuTest {
    param([switch]$Build)

    # --- 定位 QEMU ---
    $qemu = $null
    $cmd = Get-Command 'qemu-system-x86_64' -ErrorAction SilentlyContinue
    if ($cmd) { $qemu = $cmd.Source }
    foreach ($c in @('C:\Program Files\qemu\qemu-system-x86_64.exe',
                     'C:\Program Files (x86)\qemu\qemu-system-x86_64.exe',
                     'C:\msys64\mingw64\bin\qemu-system-x86_64.exe',
                     'C:\msys64\ucrt64\bin\qemu-system-x86_64.exe')) {
        if (-not $qemu -and (Test-Path $c)) { $qemu = $c }
    }
    if (-not $qemu) { throw 'qemu-system-x86_64 not found (install QEMU or add to PATH)' }

    # --- 定位 OVMF ---
    $ovmf = $null
    foreach ($c in @('C:\Program Files\qemu\share\edk2-x86_64-code.fd',
                     'C:\Program Files\qemu\share\OVMF_CODE.fd',
                     'C:\msys64\mingw64\share\edk2\ovmf\OVMF_CODE.fd',
                     'C:\msys64\ucrt64\share\edk2\ovmf\OVMF_CODE.fd')) {
        if (Test-Path $c) { $ovmf = $c; break }
    }
    if (-not $ovmf) { throw 'OVMF UEFI firmware not found under QEMU install directory' }

    New-Item -ItemType Directory -Force -Path $script:TestWorkDir | Out-Null
    New-Item -ItemType Directory -Force -Path $script:SuiteImgDir  | Out-Null
    New-Item -ItemType Directory -Force -Path $script:MkFat32Dir   | Out-Null

    $imgSrc  = Join-Path $script:IsoDir   'deshab-dev.img'
    $nvmeSrc = Join-Path $script:BuildTmp 'nvme_test.img'
    $usbSrc  = Join-Path $script:BuildTmp 'usb_test.img'

    if ($Build -or -not (Test-Path $imgSrc)) {
        Write-Host '[init] building images via build.ps1 ...'
        Stop-ResidualQemu
        & powershell -ExecutionPolicy Bypass -File (Join-Path $script:RepoRoot 'build.ps1') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed with exit code $LASTEXITCODE" }
    }
    if (-not (Test-Path $imgSrc)) { throw "image missing: $imgSrc (run with -Build)" }

    # --- 并发隔离：SYSTEM 镜像树 + 私有 SATA 生成器 + 私有镜像副本 ---
    Sync-SuiteTree
    Invoke-MkFat32
    $sata = Join-Path $script:MkFat32Dir 'sata_fat32_dsk.img'
    if (-not (Test-Path $sata)) { throw "private sata image not generated: $sata" }

    $img  = Copy-SuiteImage -Source $imgSrc -FileName 'deshab.img' -Mandatory
    $nvme = Copy-SuiteImage -Source $nvmeSrc -FileName 'nvme_test.img'
    $usb  = Copy-SuiteImage -Source $usbSrc  -FileName 'usb_test.img'

    $script:Config = [pscustomobject]@{
        QemuExe = $qemu
        Ovmf    = $ovmf
        Img     = $img
        SataImg = $sata
        NvmeImg = $nvme
        UsbImg  = $usb
    }
    Write-Host "[init] QEMU: $qemu"
    Write-Host "[init] OVMF: $ovmf"
    Write-Host "[init] IMG:  $img (private copy)"
    return $script:Config
}

# ================================================================
#  并发隔离：共享文件只读复制到套件私有区
# ================================================================

# share-tolerant 复制：源可能被并发代理的 QEMU/打包器短暂占用，重试至多 60s。
# 目标不变（mtime+size 相同）时跳过，避免每用例重复拷 768MB。
function Copy-SuiteImage {
    param(
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][string]$FileName,
        [switch]$Mandatory
    )
    if (-not (Test-Path $Source)) {
        if ($Mandatory) { throw "image missing: $Source (run with -Build)" }
        return $null
    }
    $dest = Join-Path $script:SuiteImgDir $FileName
    $srcItem = Get-Item $Source
    if ((Test-Path $dest)) {
        $dstItem = Get-Item $dest
        if ($dstItem.Length -eq $srcItem.Length -and $dstItem.LastWriteTimeUtc -eq $srcItem.LastWriteTimeUtc) {
            return $dest
        }
    }
    $deadline = (Get-Date).AddSeconds(60)
    while ($true) {
        $in = $null; $out = $null
        try {
            $in  = [System.IO.File]::Open($Source, 'Open', 'Read', [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
            $out = [System.IO.File]::Open($dest, 'Create', 'Write', [System.IO.FileShare]::None)
            $in.CopyTo($out, 1048576)
            $out.Close(); $in.Close()
            [System.IO.File]::SetLastWriteTimeUtc($dest, $srcItem.LastWriteTimeUtc)
            Write-Host "[init] private copy: $FileName ($([math]::Round($srcItem.Length/1MB))MB)"
            return $dest
        } catch [System.IO.IOException] {
            if ($out) { try { $out.Close() } catch {} }
            if ($in)  { try { $in.Close() } catch {} }
            if ((Get-Date) -gt $deadline) {
                throw "cannot copy $Source (locked by concurrent process for >60s)"
            }
            Start-Sleep -Seconds 2
        } catch {
            if ($out) { try { $out.Close() } catch {} }
            if ($in)  { try { $in.Close() } catch {} }
            throw
        }
    }
}

# SYSTEM 镜像树 + deaicup.exe 单文件 + mkfat32.ps1 生成器副本。
# 场景翻转（firstInit/AUTOEXEC）只作用于镜像树，共享工作树绝不写入。
function Sync-SuiteTree {
    & robocopy $script:SystemDir $script:MirrorSystem /MIR /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy SYSTEM mirror failed (rc=$LASTEXITCODE)" }

    $deaSrc = Join-Path $script:RepoRoot 'CODE\Deaicup Software\pe\target\x86_64-pc-windows-msvc\release\deaicup.exe'
    if (Test-Path $deaSrc) {
        $deaDst = Join-Path $script:TestWorkDir 'CODE\Deaicup Software\pe\target\x86_64-pc-windows-msvc\release\deaicup.exe'
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $deaDst) | Out-Null
        $srcItem = Get-Item $deaSrc
        $need = $true
        if (Test-Path $deaDst) {
            $dstItem = Get-Item $deaDst
            if ($dstItem.Length -eq $srcItem.Length -and $dstItem.LastWriteTimeUtc -eq $srcItem.LastWriteTimeUtc) { $need = $false }
        }
        if ($need) { Copy-Item $deaSrc $deaDst -Force }
    }

    $mkSrc = Join-Path $script:BuildTmp 'mkfat32.ps1'
    if (-not (Test-Path $mkSrc)) { throw "mkfat32.ps1 not found: $mkSrc (run build.ps1 once)" }
    Copy-Item $mkSrc (Join-Path $script:MkFat32Dir 'mkfat32.ps1') -Force
}

# ================================================================
#  进程管理
# ================================================================

# 清理本套件历史会话残留的 QEMU（Start-QemuSession 以 -name deshabtest-... 标记）。
# 只按 deshabtest- 标记精确匹配，绝不动其他代理/人工启动的 QEMU（并发隔离要求：
# 另一代理可能正挂着本仓库镜像跑调试，全局或按路径匹配都会误杀）。
function Stop-ResidualQemu {
    $procs = Get-CimInstance Win32_Process -Filter "Name='qemu-system-x86_64.exe'" -ErrorAction SilentlyContinue
    foreach ($p in $procs) {
        if ($p.CommandLine -and $p.CommandLine.Contains('deshabtest-')) {
            Write-Host "[qemu] killing residual suite qemu pid=$($p.ProcessId)"
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }
}

# monitor 端口：45501-45508 按用例分配，deaicup-e2e 固定 45454
# （避开人工调试 4444 与其他套件 456xx/457xx）。
# -Preferred 传入用例固定端口；被占用（如 TIME_WAIT）时扫描段内其余端口，
# 最后兜底 45509-45599。段内端口被他人占用时只换端口，绝不杀占用进程。
function Get-FreeMonitorPort {
    param([int]$Preferred = 0)
    $candidates = New-Object System.Collections.ArrayList
    if (($Preferred -ge 45501 -and $Preferred -le 45508) -or $Preferred -eq 45454) { [void]$candidates.Add($Preferred) }
    foreach ($p in 45501..45508) { if ($p -ne $Preferred) { [void]$candidates.Add($p) } }
    foreach ($p in 45509..45599) { [void]$candidates.Add($p) }
    foreach ($port in $candidates) {
        $listener = $null
        try {
            $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $port)
            $listener.Start()
            $listener.Stop()
            return $port
        } catch {
            if ($listener) { try { $listener.Stop() } catch {} }
        }
    }
    throw 'no free TCP port available for QEMU monitor'
}

# ================================================================
#  QEMU 会话
# ================================================================

function Start-QemuSession {
    param(
        [Parameter(Mandatory)][string]$Name,
        [string]$Accel = 'whpx',
        [switch]$NoSata,
        [switch]$NoNvme,
        [switch]$NoUsb,
        [switch]$NoNet,
        [string]$NetModel = 'e1000',
        [switch]$NoMonitor,
        [int]$MonitorPort = 0,
        [switch]$NoSnapshot,
        [string]$Cpu = '',
        [string[]]$ExtraArgs = @()
    )
    if (-not $script:Config) { throw 'call Initialize-QemuTest first' }
    $cfg = $script:Config

    $tag = "deshabtest-$Name-" + [guid]::NewGuid().ToString('N').Substring(0, 8)
    $stamp = Get-Date -Format 'HHmmss'
    $log = Join-Path $script:TestWorkDir "$Name-$stamp.log"
    $bat = Join-Path $script:TestWorkDir "$Name-$stamp.bat"

    # WHPX 不支持 nested VMX：默认关闭 guest VMX CPUID（与 ISO/run_qemu.bat 一致）。
    # -Cpu 可显式覆盖（如 'max' 用于探测 nested VMX 可用性）。
    $cpu = 'qemu64'
    if ($Accel -eq 'whpx') { $cpu = 'qemu64,-vmx' }
    if ($Cpu -ne '') { $cpu = $Cpu }

    $qargs = @(
        '-accel', $Accel,
        '-machine', 'q35',
        '-m', '2G',
        '-cpu', $cpu,
        '-serial', 'stdio',
        '-display', 'none',
        '-no-reboot',
        '-name', $tag,
        '-drive', ('if=pflash,format=raw,readonly=on,file="{0}"' -f $cfg.Ovmf),
        # 已知坑4：主盘必须显式 virtio-blk-pci + bootindex=1。
        #   `-drive if=virtio` 自动插件时，OVMF 只在 virtio-blk 恰好落到
        #   PCI 0x3（无其他 PCI 设备）才会为其自动创建启动项；附加 NVMe/
        #   USB/e1000 任一设备后 virtio 挪到 0x4/0x5，OVMF 启动项枚举整个
        #   失效，直接掉 EFI Internal Shell（ESP/BOOTX64.EFI 均正常也没用）。
        #   bootindex=1 经 fw_cfg bootorder 强制 OVMF 首选本盘，与设备组合无关。
        '-drive', ('if=none,id=osdisk,format=raw,file="{0}"' -f $cfg.Img),
        '-device', 'virtio-blk-pci,drive=osdisk,bootindex=1'
    )

    # -snapshot 默认开：镜像只读 + 写入落临时 overlay，避免镜像写锁与并发代理冲突。
    if (-not $NoSnapshot) { $qargs += '-snapshot' }

    $port = 0
    if (-not $NoMonitor) {
        $port = Get-FreeMonitorPort -Preferred $MonitorPort
        $qargs += @('-monitor', "tcp:127.0.0.1:$port,server,nowait")
    }
    if (-not $NoSata) {
        $qargs += @('-drive', ('id=sata0,format=raw,file="{0}",if=none' -f $cfg.SataImg),
                    '-device', 'ide-hd,drive=sata0,bus=ide.0')
    }
    if (-not $NoNvme -and $cfg.NvmeImg) {
        $qargs += @('-drive', ('if=none,id=nvme0,file="{0}",format=raw' -f $cfg.NvmeImg),
                    '-device', 'nvme,drive=nvme0,serial=deadbeef')
    }
    if (-not $NoUsb -and $cfg.UsbImg) {
        $qargs += @('-device', 'qemu-xhci,id=xhci0',
                    '-drive', ('id=usb0,if=none,file="{0}",format=raw' -f $cfg.UsbImg),
                    '-device', 'usb-storage,bus=xhci0.0,drive=usb0')
    }
    if (-not $NoNet) {
        $qargs += @('-netdev', 'user,id=net0')
        if ($NetModel -eq 'e1000') {
            $qargs += @('-device', 'e1000,netdev=net0,mac=52:54:00:12:34:56')
        } elseif ($NetModel -eq 'virtio-net-pci') {
            $qargs += @('-device', 'virtio-net-pci,netdev=net0')
        }
    } else {
        # 已知坑3：不显式禁网时 QEMU 会自动补一块默认 e1000 网卡（q35 默认型号），
        #          -NoNet 场景必须用 -nic none 把默认网络整个关掉。
        $qargs += @('-nic', 'none')
    }
    foreach ($x in $ExtraArgs) { $qargs += $x }
    $qargs += @('-boot', 'menu=on')

    # 已知坑1：Start-Process 下 -serial file: 不生效；用 cmd 批处理重定向 stdio。
    # 已知坑2：-snapshot 会在 %TEMP% 创建临时 overlay，Windows 用户名含非 ASCII
    #          （如中文）时 QEMU 创建失败；把 TEMP/TMP 重定向到仓库内 ASCII 目录。
    $qemuTmp = Join-Path $script:BuildTmp 'qemu-tmp'
    New-Item -ItemType Directory -Force -Path $qemuTmp | Out-Null
    $cmdline = '"' + $cfg.QemuExe + '" ' + ($qargs -join ' ')
    $batContent = "@echo off`r`nset `"TEMP=$qemuTmp`"`r`nset `"TMP=$qemuTmp`"`r`n$cmdline > `"$log`" 2>&1`r`n"
    [System.IO.File]::WriteAllText($bat, $batContent)

    Stop-ResidualQemu
    Start-Process -FilePath $bat -WindowStyle Hidden

    $session = [pscustomobject]@{
        Name        = $Name
        Tag         = $tag
        BatPath     = $bat
        LogPath     = $log
        MonitorPort = $port
        Accel       = $Accel
        StartTime   = Get-Date
    }

    # 早夭自愈：WHPX 分区被强杀后偶发重建异常，guest 启动即三重故障
    # （实测日志仅一行 "Ignoring request for interrupt vector 0"）。
    # 启动后观察 3s：进程已死且日志近乎为空 → 同一 bat 重开一次
    # （tag/monitor 端口不变，旧进程已死无冲突；bat 重定向会重建日志）。
    Start-Sleep -Seconds 3
    if (Test-QemuSessionExited $session) {
        $logSize = 0
        if (Test-Path $log) { $logSize = (Get-Item $log).Length }
        if ($logSize -lt 4096) {
            Write-Host "[qemu] session '$Name' died at launch (log ${logSize}B, WHPX startup flake), relaunching once"
            Start-Sleep -Milliseconds 800
            Start-Process -FilePath $bat -WindowStyle Hidden
        } else {
            Write-Host "[qemu] session '$Name' exited early (log ${logSize}B)"
        }
    }

    Write-Host "[qemu] session '$Name' started (monitor=$port, log=$log)"
    return $session
}

function Test-QemuSessionExited {
    param([Parameter(Mandatory)]$Session)
    $procs = Get-CimInstance Win32_Process -Filter "Name='qemu-system-x86_64.exe'" -ErrorAction SilentlyContinue
    foreach ($p in $procs) {
        if ($p.CommandLine -and $p.CommandLine.Contains($Session.Tag)) { return $false }
    }
    return $true
}

function Stop-QemuSession {
    param([Parameter(Mandatory)]$Session)
    $procs = Get-CimInstance Win32_Process -Filter "Name='qemu-system-x86_64.exe'" -ErrorAction SilentlyContinue
    foreach ($p in $procs) {
        if ($p.CommandLine -and $p.CommandLine.Contains($Session.Tag)) {
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }
    Start-Sleep -Milliseconds 500
}

# ================================================================
#  串口日志
# ================================================================

function Read-QemuLogText {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path $Path)) { return '' }
    $fs = $null
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
        $text = $sr.ReadToEnd()
        $sr.Close()
        return $text
    } catch {
        return ''
    } finally {
        if ($fs) { $fs.Close() }
    }
}

# 轮询串口日志直到任一 pattern 出现 / QEMU 退出 / 超时。返回匹配到的 pattern 或 $null。
function Wait-QemuLog {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][string[]]$Patterns,
        [int]$TimeoutSeconds = 90,
        [int]$PollMs = 1000
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $text = Read-QemuLogText $Session.LogPath
        foreach ($p in $Patterns) {
            if ($text.Contains($p)) { return $p }
        }
        if (Test-QemuSessionExited $Session) {
            Write-Host '[qemu] session exited unexpectedly'
            return $null
        }
        Start-Sleep -Milliseconds $PollMs
    }
    Write-Host "[qemu] wait timeout after ${TimeoutSeconds}s (patterns: $($Patterns -join ' | '))"
    return $null
}

# ================================================================
#  QEMU monitor（TCP）
# ================================================================

function Invoke-QemuMonitor {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][string]$Command,
        [int]$SettleMs = 250
    )
    if (-not $Session.MonitorPort) { throw 'session started with -NoMonitor' }
    $client = New-Object System.Net.Sockets.TcpClient
    $client.ReceiveTimeout = 3000
    $client.SendTimeout = 3000
    $client.Connect('127.0.0.1', [int]$Session.MonitorPort)
    try {
        $stream = $client.GetStream()
        $buf = New-Object byte[] 8192
        Start-Sleep -Milliseconds 200
        while ($stream.DataAvailable) { [void]$stream.Read($buf, 0, $buf.Length) }   # banner
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.AutoFlush = $true
        $writer.WriteLine($Command)
        Start-Sleep -Milliseconds $SettleMs
        $resp = ''
        while ($stream.DataAvailable) {
            $n = $stream.Read($buf, 0, $buf.Length)
            $resp += [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
        }
        return $resp
    } finally {
        $client.Close()
    }
}

function ConvertTo-QemuKeyName {
    param([Parameter(Mandatory)][char]$C)
    $i = [int]$C
    if ($i -ge 97 -and $i -le 122) { return [string]$C }                       # a-z
    if ($i -ge 65 -and $i -le 90)  { return 'shift-' + ([string]$C).ToLower() } # A-Z
    if ($i -ge 48 -and $i -le 57)  { return [string]$C }                       # 0-9
    switch ($C) {
        ' '  { return 'spc' }
        '.'  { return 'dot' }
        ','  { return 'comma' }
        '/'  { return 'slash' }
        '\'  { return 'backslash' }
        '-'  { return 'minus' }
        '='  { return 'equal' }
        ';'  { return 'semicolon' }
        ':'  { return 'shift-semicolon' }
        '>'  { return 'shift-dot' }
        '<'  { return 'shift-comma' }
        '_'  { return 'shift-minus' }
        '!'  { return 'shift-1' }
        '"'  { return 'shift-apostrophe' }
        '''' { return 'apostrophe' }
        '('  { return 'shift-9' }
        ')'  { return 'shift-0' }
        '?'  { return 'shift-slash' }
        '*'  { return 'shift-8' }
        '+'  { return 'shift-equal' }
        '@'  { return 'shift-2' }
        '#'  { return 'shift-3' }
        '$'  { return 'shift-4' }
        '%'  { return 'shift-5' }
        '^'  { return 'shift-6' }
        '&'  { return 'shift-7' }
        '|'  { return 'shift-backslash' }
        '~'  { return 'shift-grave_accent' }
    }
    return $null
}

# 通过 monitor 发送一串按键（单 TCP 连接内完成）
function Send-QemuKeys {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][string[]]$Keys,
        [int]$DelayMs = 120
    )
    if (-not $Session.MonitorPort) { throw 'session started with -NoMonitor' }
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', [int]$Session.MonitorPort)
    try {
        $stream = $client.GetStream()
        $buf = New-Object byte[] 4096
        Start-Sleep -Milliseconds 200
        while ($stream.DataAvailable) { [void]$stream.Read($buf, 0, $buf.Length) }
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.AutoFlush = $true
        foreach ($k in $Keys) {
            $writer.WriteLine("sendkey $k")
            Start-Sleep -Milliseconds $DelayMs
        }
    } finally {
        $client.Close()
    }
}

# 输入一段文本（自动映射大小写/符号），可选追加回车
function Send-QemuText {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][string]$Text,
        [switch]$Enter,
        [int]$DelayMs = 90
    )
    $keys = New-Object System.Collections.ArrayList
    foreach ($c in $Text.ToCharArray()) {
        $k = ConvertTo-QemuKeyName $c
        if ($k) { [void]$keys.Add($k) }
    }
    if ($Enter) { [void]$keys.Add('ret') }
    Send-QemuKeys -Session $Session -Keys ($keys.ToArray()) -DelayMs $DelayMs
}

function Send-QemuMouseMove {
    param(
        [Parameter(Mandatory)]$Session,
        [int]$Dx = 0,
        [int]$Dy = 0,
        [string]$Button = ''
    )
    $cmd = "mouse_move $Dx $Dy"
    [void](Invoke-QemuMonitor -Session $Session -Command $cmd)
    if ($Button) {
        [void](Invoke-QemuMonitor -Session $Session -Command "mouse_button $Button")
    }
}

function Get-QemuScreenshot {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][string]$OutPath
    )
    $resp = Invoke-QemuMonitor -Session $Session -Command "screendump `"$OutPath`"" -SettleMs 600
    return (Test-Path $OutPath)
}

# ================================================================
#  场景准备（firstInit.txt / AUTOEXEC.BAT + SATA 镜像重建）
# ================================================================

# 私有 SATA 镜像生成：运行 fat32gen\ 下的 mkfat32.ps1 副本。
# 副本的 PSScriptRoot=fat32gen → 输出 fat32gen\sata_fat32_dsk.img（私有），
# 输入 tests\SYSTEM 镜像树 + tests\CODE\...\deaicup.exe，不读写任何共享路径。
function Invoke-MkFat32 {
    $mk = Join-Path $script:MkFat32Dir 'mkfat32.ps1'
    if (-not (Test-Path $mk)) { throw "mkfat32.ps1 copy not found: $mk (Initialize-QemuTest runs Sync-SuiteTree)" }
    & powershell -ExecutionPolicy Bypass -File $mk | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "mkfat32.ps1 failed with exit code $LASTEXITCODE" }
}

# 写镜像树 firstInit.txt（格式 "<first>`n<dev_mode>"，LF）并重建私有 SATA 镜像；原内容自动备份。
# 只写 .build_tmp\tests\SYSTEM 镜像树，共享 SYSTEM 工作树保持不变（并发代理可能正在翻转它）。
# 已知坑5：DSK 的 dev_mode 是「FUCK dev_mode OR firstInit 第二行」单向合并，
#   firstInit 只能置 1 不能清 0。dev 镜像 FUCK dev_mode=1 时 shell.elf 自动
#   测试后进入交互主循环阻塞启动流，cmd.elf 永远调度不到。因此 -DevMode 0
#   必须同步把镜像树 FUCK [boot] dev_mode 也改 0（仅测试镜像树，不动共享树）。
function Set-DeshabFirstInit {
    param(
        [Parameter(Mandatory)][int]$First,
        [Parameter(Mandatory)][int]$DevMode
    )
    $path = Join-Path $script:MirrorSystem 'system\user\use\firstInit.txt'
    if ($null -eq $script:FirstInitBackup) {
        $script:FirstInitBackup = [System.IO.File]::ReadAllBytes($path)
    }
    [System.IO.File]::WriteAllText($path, "$First`n$DevMode")
    # 同步 FUCK [boot] dev_mode（单向合并导致 firstInit 无法清 dev_mode，见上）
    $fuckPath = Join-Path $script:MirrorSystem 'system\deshab64\FUCK'
    if ($null -eq $script:FuckBackup) {
        $script:FuckBackup = [System.IO.File]::ReadAllBytes($fuckPath)
    }
    $fuckTxt = [System.IO.File]::ReadAllText($fuckPath)
    $fuckTxt = $fuckTxt -replace '(?m)^dev_mode=\d', "dev_mode=$DevMode"
    [System.IO.File]::WriteAllText($fuckPath, $fuckTxt)
    Invoke-MkFat32
}

function Restore-DeshabFirstInit {
    $path = Join-Path $script:MirrorSystem 'system\user\use\firstInit.txt'
    if ($null -ne $script:FirstInitBackup) {
        [System.IO.File]::WriteAllBytes($path, $script:FirstInitBackup)
        $script:FirstInitBackup = $null
        $restored = $true
    }
    $fuckPath = Join-Path $script:MirrorSystem 'system\deshab64\FUCK'
    if ($null -ne $script:FuckBackup) {
        [System.IO.File]::WriteAllBytes($fuckPath, $script:FuckBackup)
        $script:FuckBackup = $null
        $restored = $true
    }
    if ($restored) { Invoke-MkFat32 }
}

# 替换镜像树 AUTOEXEC.BAT 内容（cmd.elf 启动时逐行自动执行）并重建私有 SATA 镜像；原内容自动备份
function Set-DeshabAutoexec {
    param([Parameter(Mandatory)][string[]]$Lines)
    $path = Join-Path $script:MirrorSystem 'bin\AUTOEXEC.BAT'
    if ($null -eq $script:AutoexecBackup) {
        $script:AutoexecBackup = [System.IO.File]::ReadAllBytes($path)
    }
    [System.IO.File]::WriteAllText($path, (($Lines -join "`r`n") + "`r`n"))
    Invoke-MkFat32
}

function Restore-DeshabAutoexec {
    $path = Join-Path $script:MirrorSystem 'bin\AUTOEXEC.BAT'
    if ($null -ne $script:AutoexecBackup) {
        [System.IO.File]::WriteAllBytes($path, $script:AutoexecBackup)
        $script:AutoexecBackup = $null
        Invoke-MkFat32
    }
}

function Restore-DeshabScenario {
    Restore-DeshabFirstInit
    Restore-DeshabAutoexec
}

# ================================================================
#  结果与断言
# ================================================================

function New-TestCaseResult {
    param([Parameter(Mandatory)][string]$Case)
    return [pscustomobject]@{
        Case        = $Case
        Status      = 'PASS'
        StartTime   = Get-Date
        DurationSec = 0.0
        LogPath     = ''
        Checks      = New-Object System.Collections.ArrayList
        Notes       = New-Object System.Collections.ArrayList
    }
}

function Add-TestCheck {
    param(
        [Parameter(Mandatory)]$Result,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Status,   # PASS / FAIL / MANUAL / SKIP
        [string]$Detail = ''
    )
    [void]$Result.Checks.Add([pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail })
    if ($Status -eq 'FAIL') { $Result.Status = 'FAIL' }
}

function Add-TestNote {
    param([Parameter(Mandatory)]$Result, [Parameter(Mandatory)][string]$Note)
    [void]$Result.Notes.Add($Note)
}

# 日志断言引擎：必须包含 / 禁止包含 / 按顺序包含 / 出现次数下限
function Assert-QemuLog {
    param(
        [Parameter(Mandatory)]$Result,
        [Parameter(Mandatory)][string]$LogText,
        [string[]]$MustContain = @(),
        [string[]]$MustNotContain = @(),
        [string[]]$Ordered = @(),
        [string]$CountPattern = '',
        [int]$MinCount = 0
    )
    foreach ($p in $MustContain) {
        if ($LogText.Contains($p)) {
            Add-TestCheck $Result "锚点: $p" 'PASS'
        } else {
            Add-TestCheck $Result "锚点: $p" 'FAIL' 'pattern not found in serial log'
        }
    }
    foreach ($p in $MustNotContain) {
        if ($LogText.Contains($p)) {
            Add-TestCheck $Result "禁止: $p" 'FAIL' 'forbidden pattern found in serial log'
        } else {
            Add-TestCheck $Result "禁止: $p" 'PASS'
        }
    }
    if ($Ordered.Count -gt 0) {
        $pos = 0
        $failAt = ''
        foreach ($p in $Ordered) {
            $idx = $LogText.IndexOf($p, $pos)
            if ($idx -lt 0) { $failAt = $p; break }
            $pos = $idx + $p.Length
        }
        if ($failAt -eq '') {
            Add-TestCheck $Result ("顺序链({0} 锚点)" -f $Ordered.Count) 'PASS' ($Ordered -join ' -> ')
        } else {
            Add-TestCheck $Result ("顺序链({0} 锚点)" -f $Ordered.Count) 'FAIL' "missing/out-of-order: $failAt"
        }
    }
    if ($CountPattern -ne '' -and $MinCount -gt 0) {
        $count = 0
        $pos = 0
        while ($true) {
            $idx = $LogText.IndexOf($CountPattern, $pos)
            if ($idx -lt 0) { break }
            $count++
            $pos = $idx + $CountPattern.Length
        }
        if ($count -ge $MinCount) {
            Add-TestCheck $Result "计数: '$CountPattern' x$count (>= $MinCount)" 'PASS'
        } else {
            Add-TestCheck $Result "计数: '$CountPattern' x$count (>= $MinCount)" 'FAIL' "only $count occurrences"
        }
    }
}

# 输出用例结果；FAIL 时附带日志尾部上下文
function Write-TestCaseResult {
    param([Parameter(Mandatory)]$Result)
    $Result.DurationSec = [math]::Round(((Get-Date) - $Result.StartTime).TotalSeconds, 1)
    Write-Host ''
    Write-Host ("=== [{0}] {1} ({2}s) ===" -f $Result.Case, $Result.Status, $Result.DurationSec)
    foreach ($c in $Result.Checks) {
        $mark = '[PASS]'
        if ($c.Status -eq 'FAIL')   { $mark = '[FAIL]  ' }
        if ($c.Status -eq 'MANUAL') { $mark = '[MANUAL]' }
        if ($c.Status -eq 'SKIP')   { $mark = '[SKIP]  ' }
        Write-Host ("  {0} {1}" -f $mark, $c.Name)
        if ($c.Detail -and $c.Status -ne 'PASS') {
            Write-Host ("         -> {0}" -f $c.Detail)
        }
    }
    foreach ($n in $Result.Notes) { Write-Host "  [note] $n" }
    if ($Result.Status -eq 'FAIL' -and $Result.LogPath -and (Test-Path $Result.LogPath)) {
        Write-Host '  --- serial log tail (25 lines) ---'
        Get-Content $Result.LogPath -Tail 25 | ForEach-Object { Write-Host "  | $_" }
    }
}

# ================================================================
#  FirstInit 设置向导自动填表（首启流程公共助手）
#    账户卡片(3 字段) → 偏好页(5xEnter) → 网络页(3xEnter) → 完成
#    每步等串口锚点再发键；字段验证失败自动重试 3 次。
# ================================================================
function Complete-DeshabFirstInitWizard {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)]$Result,
        [string]$Prefix = 'FirstInit'
    )

    $hit = Wait-QemuLog -Session $Session -Patterns @('[FI] fadein done') -TimeoutSeconds 150
    if (-not $hit) { Add-TestCheck $Result "${Prefix}: 账户卡片就绪" 'FAIL' 'missing [FI] fadein done'; return $false }

    $done = $false
    for ($attempt = 1; $attempt -le 3 -and -not $done; $attempt++) {
        Send-QemuText -Session $Session -Text 'testpc' -Enter
        Start-Sleep -Milliseconds 400
        Send-QemuText -Session $Session -Text 'tester' -Enter
        Start-Sleep -Milliseconds 400
        Send-QemuText -Session $Session -Text 'pass123' -Enter
        $hit = Wait-QemuLog -Session $Session -Patterns @('[FI] all fields done', '[FI] validation failed') -TimeoutSeconds 30
        if ($hit -eq '[FI] all fields done') { $done = $true }
        else { Add-TestNote $Result "${Prefix}: 字段验证失败，重试第 $attempt 次" }
    }
    if (-not $done) { Add-TestCheck $Result "${Prefix}: 账户字段填写" 'FAIL' 'validation did not pass after 3 attempts'; return $false }
    Add-TestCheck $Result "${Prefix}: 账户字段填写" 'PASS'

    Start-Sleep -Milliseconds 600
    Send-QemuKeys -Session $Session -Keys @('ret', 'ret', 'ret', 'ret', 'ret') -DelayMs 350
    $hit = Wait-QemuLog -Session $Session -Patterns @('[FirstInit] entering network setup page') -TimeoutSeconds 40
    if (-not $hit) { Add-TestCheck $Result "${Prefix}: 偏好页" 'FAIL' 'did not reach network setup page'; return $false }
    Add-TestCheck $Result "${Prefix}: 偏好页(5xEnter)" 'PASS'

    Start-Sleep -Milliseconds 400
    Send-QemuKeys -Session $Session -Keys @('ret', 'ret', 'ret') -DelayMs 350
    $hit = Wait-QemuLog -Session $Session -Patterns @('[FirstInit] user setup finished') -TimeoutSeconds 40
    if (-not $hit) { Add-TestCheck $Result "${Prefix}: 网络页" 'FAIL' 'setup did not finish'; return $false }
    Add-TestCheck $Result "${Prefix}: 网络页(3xEnter)" 'PASS'
    return $true
}

# ================================================================
#  极简 HTTP 测试服务器（network-e2e 用）
#    slirp 网关 10.0.2.2 → 主机 127.0.0.1；guest curl 经 slirp 访问主机服务。
#    后台 Job 接收连接，把每个请求首行写入 $LogPath（主机侧端到端证据），
#    回复固定 HTTP/1.0 200 + 标记体后关闭。
# ================================================================
function Start-TestHttpServer {
    param(
        [int]$Port = 8000,
        [Parameter(Mandatory)][string]$LogPath,
        [string]$Marker = 'DESHAB-NET-E2E-MARKER'
    )
    if (Test-Path $LogPath) { Remove-Item $LogPath -Force }
    $job = Start-Job -ScriptBlock {
        param($Port, $LogPath, $Marker)
        $listener = $null
        try {
            $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
            $listener.Start()
            $body = "<html><body><h1>$Marker</h1></body></html>"
            $deadline = (Get-Date).AddMinutes(30)
            while ((Get-Date) -lt $deadline) {
                if (-not $listener.Pending()) { Start-Sleep -Milliseconds 200; continue }
                $client = $listener.AcceptTcpClient()
                try {
                    $stream = $client.GetStream()
                    $stream.ReadTimeout = 5000
                    $buf = New-Object byte[] 4096
                    $req = ''
                    while ($stream.DataAvailable -or $req -eq '') {
                        $n = 0
                        try { $n = $stream.Read($buf, 0, $buf.Length) } catch { break }
                        if ($n -le 0) { break }
                        $req += [System.Text.Encoding]::ASCII.GetString($buf, 0, $n)
                        if ($req.Contains("`r`n`r`n")) { break }
                    }
                    $firstLine = ($req -split "`r`n")[0]
                    [System.IO.File]::AppendAllText($LogPath, "$(Get-Date -Format 'HH:mm:ss') $firstLine`r`n")
                    $resp = "HTTP/1.0 200 OK`r`nContent-Type: text/html`r`nContent-Length: $($body.Length)`r`nConnection: close`r`n`r`n$body"
                    $bytes = [System.Text.Encoding]::ASCII.GetBytes($resp)
                    $stream.Write($bytes, 0, $bytes.Length)
                    $stream.Flush()
                } finally {
                    $client.Close()
                }
            }
        } finally {
            if ($listener) { $listener.Stop() }
        }
    } -ArgumentList $Port, $LogPath, $Marker
    Start-Sleep -Milliseconds 800
    if ($job.State -ne 'Running') {
        $err = Receive-Job $job 2>&1 | Out-String
        throw "http test server failed to start on port ${Port}: $err"
    }
    Write-Host "[http] test server listening on 127.0.0.1:$Port (log=$LogPath)"
    return $job
}

function Stop-TestHttpServer {
    param($Job)
    if ($Job) {
        Stop-Job $Job -ErrorAction SilentlyContinue
        Remove-Job $Job -Force -ErrorAction SilentlyContinue
    }
}
