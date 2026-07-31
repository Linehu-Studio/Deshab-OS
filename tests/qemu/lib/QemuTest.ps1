#requires -Version 5.1
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
$script:Config      = $null

# 场景备份（模块级，保证只备份一次、恢复幂等）
$script:FirstInitBackup = $null
$script:AutoexecBackup  = $null

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

    $img  = Join-Path $script:IsoDir   'deshab.img'
    $sata = Join-Path $script:BuildTmp 'sata_fat32_dsk.img'
    $nvme = Join-Path $script:BuildTmp 'nvme_test.img'
    $usb  = Join-Path $script:BuildTmp 'usb_test.img'

    if ($Build -or -not (Test-Path $img)) {
        Write-Host '[init] building images via build.ps1 ...'
        Stop-ResidualQemu
        & powershell -ExecutionPolicy Bypass -File (Join-Path $script:RepoRoot 'build.ps1') | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed with exit code $LASTEXITCODE" }
    }
    if (-not (Test-Path $img))  { throw "image missing: $img (run with -Build)" }
    if (-not (Test-Path $sata)) {
        Write-Host '[init] sata_fat32_dsk.img missing, regenerating via mkfat32.ps1 ...'
        Invoke-MkFat32
    }
    if (-not (Test-Path $sata)) { throw "sata image missing: $sata" }

    $nvmeOk = $null; if (Test-Path $nvme) { $nvmeOk = $nvme }
    $usbOk  = $null; if (Test-Path $usb)  { $usbOk  = $usb }

    $script:Config = [pscustomobject]@{
        QemuExe = $qemu
        Ovmf    = $ovmf
        Img     = $img
        SataImg = $sata
        NvmeImg = $nvmeOk
        UsbImg  = $usbOk
    }
    Write-Host "[init] QEMU: $qemu"
    Write-Host "[init] OVMF: $ovmf"
    Write-Host "[init] IMG:  $img"
    return $script:Config
}

# ================================================================
#  进程管理
# ================================================================

# 清理持有本项目镜像的残留 QEMU（deshab.img 被锁会导致 build/启动失败）。
# 只杀命令行中引用本仓库路径的 qemu 进程，避免误伤他人虚拟机。
function Stop-ResidualQemu {
    $procs = Get-CimInstance Win32_Process -Filter "Name='qemu-system-x86_64.exe'" -ErrorAction SilentlyContinue
    foreach ($p in $procs) {
        if ($p.CommandLine -and $p.CommandLine.ToLower().Contains('deshab')) {
            Write-Host "[qemu] killing residual qemu pid=$($p.ProcessId)"
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
    }
}

function Get-FreeMonitorPort {
    for ($port = 4444; $port -lt 4544; $port++) {
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
        '-drive', ('format=raw,file="{0}",if=virtio' -f $cfg.Img)
    )

    $port = 0
    if (-not $NoMonitor) {
        $port = Get-FreeMonitorPort
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
    }
    foreach ($x in $ExtraArgs) { $qargs += $x }
    $qargs += @('-boot', 'menu=on')

    # 已知坑：Start-Process 下 -serial file: 不生效；用 cmd 批处理重定向 stdio。
    $cmdline = '"' + $cfg.QemuExe + '" ' + ($qargs -join ' ')
    $batContent = "@echo off`r`n$cmdline > `"$log`" 2>&1`r`n"
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

function Invoke-MkFat32 {
    $mk = Join-Path $script:BuildTmp 'mkfat32.ps1'
    if (-not (Test-Path $mk)) { throw "mkfat32.ps1 not found: $mk (run build.ps1 once)" }
    & powershell -ExecutionPolicy Bypass -File $mk | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "mkfat32.ps1 failed with exit code $LASTEXITCODE" }
}

# 写 firstInit.txt（格式 "<first>`n<dev_mode>"，LF）并重建 SATA 镜像；原文件自动备份
function Set-DeshabFirstInit {
    param(
        [Parameter(Mandatory)][int]$First,
        [Parameter(Mandatory)][int]$DevMode
    )
    $path = Join-Path $script:SystemDir 'system\user\use\firstInit.txt'
    if ($null -eq $script:FirstInitBackup) {
        $script:FirstInitBackup = [System.IO.File]::ReadAllBytes($path)
    }
    [System.IO.File]::WriteAllText($path, "$First`n$DevMode")
    Invoke-MkFat32
}

function Restore-DeshabFirstInit {
    $path = Join-Path $script:SystemDir 'system\user\use\firstInit.txt'
    if ($null -ne $script:FirstInitBackup) {
        [System.IO.File]::WriteAllBytes($path, $script:FirstInitBackup)
        $script:FirstInitBackup = $null
        Invoke-MkFat32
    }
}

# 替换 AUTOEXEC.BAT 内容（cmd.elf 启动时逐行自动执行）并重建 SATA 镜像；原文件自动备份
function Set-DeshabAutoexec {
    param([Parameter(Mandatory)][string[]]$Lines)
    $path = Join-Path $script:SystemDir 'bin\AUTOEXEC.BAT'
    if ($null -eq $script:AutoexecBackup) {
        $script:AutoexecBackup = [System.IO.File]::ReadAllBytes($path)
    }
    [System.IO.File]::WriteAllText($path, (($Lines -join "`r`n") + "`r`n"))
    Invoke-MkFat32
}

function Restore-DeshabAutoexec {
    $path = Join-Path $script:SystemDir 'bin\AUTOEXEC.BAT'
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
