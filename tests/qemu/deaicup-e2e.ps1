#requires -Version 5.1
<#
.SYNOPSIS
    deaicup-e2e - Deaicup GUI PE end-to-end test (docs/RE/test-cases/pe-compat.md)

.DESCRIPTION
    AUTOEXEC runs `pe DEAICUP.EXE` (Rust/egui original Win32 API PE64 GUI app),
    then the script drives the app through the QEMU monitor and asserts on the
    serial log plus screendump pixel analysis:

      1. boot chain: 9-anchor ordered chain
         (cmdline -> native run -> mainCRTStartup -> RegisterClassExW ->
          CreateWindowExW -> CreateDIBSection -> Context::default ->
          BitBlt first frame -> bitblt ok)
      2. screendump pixel analysis of the rendered GUI frame:
         not-all-black, distinct color threshold, GUI window rectangle
         detection (window body color / count / bounding box / fill ratio)
      3. mouse: AUX init + mouse_move -> mpkt (WM_MOUSEMOVE),
         mouse_button -> LDOWN/LUP edges (WM_LBUTTONDOWN/WM_LBUTTONUP),
         cursor-region pixel diff between two screendumps
      4. keyboard: sendkey -> KEYDOWN/CHAR/KEYUP (WM_KEYDOWN/WM_CHAR/WM_KEYUP)
      5. Esc -> WM_CLOSE -> ExitProcess(0) -> [cmd] PE run end clean exit chain
         (also regresses BUG-006 long-run return chain and BUG-005 noise)

    Forbidden patterns over the whole session:
      [PANIC] / [IDT] exception / Unimplemented handler

    WHPX note: native PE32+ execution enables SSE via CR4 and WHPX may kill
    QEMU with "Unexpected VP exit code 4" (platform limitation, not a guest
    crash). If the first-frame anchor is missed under the requested
    accelerator the session is rerun once with -Accel tcg
    (pass -Accel tcg to skip WHPX directly).

    NOTE: keep this file ASCII-only. PS 5.1 reads BOM-less UTF-8 as GBK;
    CJK comment bytes can swallow newlines and merge code into comments.

    Exit code: 0 = all checks PASS, 1 = any FAIL.
#>
param(
    [switch]$Build,
    [string]$Accel = 'whpx',
    [int]$MonitorPort = 45454
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\QemuTest.ps1')

Initialize-QemuTest -Build:$Build
$result = New-TestCaseResult 'deaicup-e2e'

# ================================================================
# PPM helpers. WARNING: [byte] -shl N truncates back to byte in
# PS 5.1, always cast to [int] before shifting (the scratch
# analyze script lost R/G channels that way).
# ================================================================
function Read-PpmFile {
    param([Parameter(Mandatory)][string]$Path)
    $b = [System.IO.File]::ReadAllBytes($Path)
    $p = 0
    $tok = @()
    while ($tok.Count -lt 4) {
        while ($p -lt $b.Length) {
            $c = $b[$p]
            if ($c -eq 0x23) { while ($p -lt $b.Length -and $b[$p] -ne 0x0A) { $p++ } }
            elseif ($c -le 0x20) { $p++ }
            else { break }
        }
        $s = $p
        while ($p -lt $b.Length -and $b[$p] -gt 0x20) { $p++ }
        $tok += [Text.Encoding]::ASCII.GetString($b, $s, $p - $s)
    }
    $p++
    if ($tok[0] -ne 'P6') { throw "not a P6 ppm: $($tok[0])" }
    return @{ data = $b; off = $p; w = [int]$tok[1]; h = [int]$tok[2] }
}

# Pass 1: histogram + non-black count. Pass 2: window rectangle.
# Window body color = most common color with mid-dark luminance
# (excludes black console background and bright text/cursor/highlight).
function Get-PpmStats {
    param([Parameter(Mandatory)]$F)
    $npix = $F.w * $F.h
    $colors = @{}
    $nonBlack = 0
    for ($i = 0; $i -lt $npix; $i++) {
        $o = $F.off + $i * 3
        $r = [int]$F.data[$o]; $g = [int]$F.data[$o + 1]; $b = [int]$F.data[$o + 2]
        $key = ($r -shl 16) -bor ($g -shl 8) -bor $b
        if ($colors.ContainsKey($key)) { $colors[$key]++ } else { $colors[$key] = 1 }
        if ($r -ge 8 -or $g -ge 8 -or $b -ge 8) { $nonBlack++ }
    }
    $winKey = -1; $winCount = 0
    foreach ($e in ($colors.GetEnumerator() | Sort-Object Value -Descending)) {
        $k = [int]$e.Key
        $r = ($k -shr 16) -band 0xFF; $g = ($k -shr 8) -band 0xFF; $b = $k -band 0xFF
        $lum = 0.299 * $r + 0.587 * $g + 0.114 * $b
        if ($lum -ge 15 -and $lum -le 90) { $winKey = $k; $winCount = [int]$e.Value; break }
    }
    # Window rectangle: the GUI window body is nearly solid window-color, while
    # anti-aliased console text rows only contain a sparse sprinkle of it.
    # So: per-row window-color count -> longest contiguous run of "solid" rows
    # (>= 50% of the max row count) -> x extent + fill ratio inside that run.
    $box = $null
    if ($winKey -ge 0) {
        $rowCnt   = New-Object int[] $F.h
        $rowFirst = New-Object int[] $F.h
        $rowLast  = New-Object int[] $F.h
        for ($y = 0; $y -lt $F.h; $y++) {
            $rowOff = $F.off + $y * $F.w * 3
            $cnt = 0; $fx = -1; $lx = -1
            for ($x = 0; $x -lt $F.w; $x++) {
                $o = $rowOff + $x * 3
                $key = ([int]$F.data[$o] -shl 16) -bor ([int]$F.data[$o + 1] -shl 8) -bor [int]$F.data[$o + 2]
                if ($key -eq $winKey) {
                    $cnt++
                    if ($fx -lt 0) { $fx = $x }
                    $lx = $x
                }
            }
            $rowCnt[$y] = $cnt; $rowFirst[$y] = $fx; $rowLast[$y] = $lx
        }
        $maxRow = 0
        for ($y = 0; $y -lt $F.h; $y++) { if ($rowCnt[$y] -gt $maxRow) { $maxRow = $rowCnt[$y] } }
        $rowThresh = [int]($maxRow * 0.5)
        $bestY0 = -1; $bestY1 = -1; $curY0 = -1
        for ($y = 0; $y -le $F.h; $y++) {
            $inRun = ($y -lt $F.h) -and ($rowCnt[$y] -ge $rowThresh)
            if ($inRun -and $curY0 -lt 0) { $curY0 = $y }
            if (-not $inRun -and $curY0 -ge 0) {
                if (($y - 1 - $curY0) -gt ($bestY1 - $bestY0)) { $bestY0 = $curY0; $bestY1 = $y - 1 }
                $curY0 = -1
            }
        }
        if ($bestY0 -ge 0) {
            $minX = $F.w; $maxX = -1; $inside = 0
            for ($y = $bestY0; $y -le $bestY1; $y++) {
                if ($rowCnt[$y] -gt 0) {
                    if ($rowFirst[$y] -lt $minX) { $minX = $rowFirst[$y] }
                    if ($rowLast[$y] -gt $maxX) { $maxX = $rowLast[$y] }
                    $inside += $rowCnt[$y]
                }
            }
            $bw = $maxX - $minX + 1; $bh = $bestY1 - $bestY0 + 1
            $box = @{ X0 = $minX; Y0 = $bestY0; X1 = $maxX; Y1 = $bestY1; W = $bw; H = $bh
                      Fill = [math]::Round($inside / [double]($bw * $bh), 4) }
        }
    }
    return @{ Colors = $colors.Count; NonBlack = $nonBlack; Pixels = $npix
              WinKey = $winKey; WinCount = $winCount; WinBox = $box }
}

# Sampled diff (every 2nd pixel) between two frames over a region.
# A pixel counts as changed when |dR|+|dG|+|dB| > 30.
function Compare-PpmRegion {
    param([Parameter(Mandatory)]$A, [Parameter(Mandatory)]$B,
          [int]$X0, [int]$Y0, [int]$X1, [int]$Y1)
    $changed = 0
    for ($y = $Y0; $y -le $Y1; $y += 2) {
        $rowA = $A.off + $y * $A.w * 3
        $rowB = $B.off + $y * $B.w * 3
        for ($x = $X0; $x -le $X1; $x += 2) {
            $oa = $rowA + $x * 3
            $ob = $rowB + $x * 3
            $d = [Math]::Abs([int]$A.data[$oa] - [int]$B.data[$ob]) +
                 [Math]::Abs([int]$A.data[$oa + 1] - [int]$B.data[$ob + 1]) +
                 [Math]::Abs([int]$A.data[$oa + 2] - [int]$B.data[$ob + 2])
            if ($d -gt 30) { $changed++ }
        }
    }
    return $changed
}

function Convert-PpmToPng {
    param([Parameter(Mandatory)][string]$PpmPath, [Parameter(Mandatory)][string]$PngPath)
    Add-Type -AssemblyName System.Drawing
    $bytes = [System.IO.File]::ReadAllBytes($PpmPath)
    # ASCII decode: 1 byte == 1 char, so string index == byte offset.
    # QEMU screendump header is "P6\n<w> <h>\n255\n" + single whitespace byte.
    $hdr = [System.Text.Encoding]::ASCII.GetString($bytes, 0, [Math]::Min(64, $bytes.Length))
    $m = [regex]::Match($hdr, '^P6\s+(\d+)\s+(\d+)\s+(\d+)\s')
    if (-not $m.Success) { throw 'bad ppm header' }
    $w = [int]$m.Groups[1].Value; $h = [int]$m.Groups[2].Value; $max = [int]$m.Groups[3].Value
    if ($max -ne 255) { throw "unsupported maxval $max" }
    $pos = $m.Length
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $rect = New-Object System.Drawing.Rectangle 0, 0, $w, $h
    $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bmp.PixelFormat)
    try {
        $rowBytes = $w * 3
        $rgb = New-Object byte[] ($rowBytes)
        for ($y = 0; $y -lt $h; $y++) {
            $srcOff = $pos + $y * $rowBytes
            for ($x = 0; $x -lt $w; $x++) {
                $rgb[$x*3+0] = $bytes[$srcOff + $x*3 + 2]  # B
                $rgb[$x*3+1] = $bytes[$srcOff + $x*3 + 1]  # G
                $rgb[$x*3+2] = $bytes[$srcOff + $x*3 + 0]  # R
            }
            [System.Runtime.InteropServices.Marshal]::Copy($rgb, 0, [IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $rowBytes)
        }
    } finally { $bmp.UnlockBits($data) }
    $bmp.Save($PngPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

$script:UsedAccel = $Accel
$s = $null
try {
    Set-DeshabFirstInit -First 1 -DevMode 0
    Set-DeshabAutoexec -Lines @('pe DEAICUP.EXE')

    $s = Start-QemuSession -Name "deaicup-$Accel" -Accel $Accel -MonitorPort $MonitorPort
    $result.LogPath = $s.LogPath

    # ---- 1. boot chain + first frame (WHPX miss -> TCG rerun once) ----
    $waitFirst = 150
    if ($Accel -eq 'tcg') { $waitFirst = 360 }   # TCG full emulation is much slower
    $hit = Wait-QemuLog -Session $s -Patterns @('[shim] BitBlt first frame on screen', '[PANIC]', '[IDT] exception') -TimeoutSeconds $waitFirst

    if ($hit -ne '[shim] BitBlt first frame on screen' -and $Accel -eq 'whpx') {
        $why = 'first-frame anchor missed under WHPX'
        $early = Read-QemuLogText $s.LogPath
        if ($early.Contains('WHPX: Unexpected VP exit')) { $why = 'WHPX terminated QEMU at native PE execution (Unexpected VP exit, platform limitation)' }
        Add-TestNote $result "$why; rerunning once with TCG"
        Stop-QemuSession $s
        $s = $null
        $script:UsedAccel = 'tcg'
        $s = Start-QemuSession -Name 'deaicup-tcg' -Accel 'tcg' -MonitorPort $MonitorPort
        $result.LogPath = $s.LogPath
        $hit = Wait-QemuLog -Session $s -Patterns @('[shim] BitBlt first frame on screen', '[PANIC]', '[IDT] exception') -TimeoutSeconds 360
    }

    $log = Read-QemuLogText $s.LogPath
    if ($hit -ne '[shim] BitBlt first frame on screen') {
        Add-TestCheck $result 'first frame: BitBlt first frame on screen' 'FAIL' "hit=$hit"
        throw 'first-frame anchor missing'
    }
    Add-TestCheck $result 'first frame: BitBlt first frame on screen' 'PASS'
    Assert-QemuLog $result $log -Ordered @(
        '[cmd] cmdline: DEAICUP.EXE',
        '[PE] running PE32+ natively',
        '[deaicup] mainCRTStartup',
        '[shim] RegisterClassExW ok',
        '[shim] CreateWindowExW ok',
        '[shim] CreateDIBSection ok',
        '[dc] Context::default ok',
        '[shim] BitBlt first frame on screen',
        '[dc] bitblt ok'
    ) -MustNotContain @('[PANIC]', '[IDT] exception', 'Unimplemented handler')

    # ---- 2. screendump A: pixel analysis of the rendered GUI frame ----
    # QEMU human monitor parses backslashes as escapes inside quoted strings
    # (\C etc. illegal -> screendump silently fails); OutPath must use '/'.
    Start-Sleep -Seconds 2
    $ppmA = ($script:TestWorkDir -replace '\\', '/') + '/deaicup-frame-a.ppm'
    $ppmB = ($script:TestWorkDir -replace '\\', '/') + '/deaicup-frame-b.ppm'
    $png  = Join-Path $script:TestWorkDir 'deaicup-frame.png'
    $fa = $null
    $statsA = $null
    if (Get-QemuScreenshot -Session $s -OutPath $ppmA) {
        try {
            Convert-PpmToPng -PpmPath $ppmA -PngPath $png
            Add-TestCheck $result "screenshot: evidence frame -> $png" 'PASS'
        } catch {
            Add-TestCheck $result 'screenshot: PPM->PNG convert' 'FAIL' $_.Exception.Message
        }
        $fa = Read-PpmFile $ppmA
        $statsA = Get-PpmStats $fa
        $boxDesc = 'none'
        if ($statsA.WinBox) {
            $boxDesc = ("({0},{1})-({2},{3}) {4}x{5} fill={6:P1}" -f $statsA.WinBox.X0, $statsA.WinBox.Y0,
                        $statsA.WinBox.X1, $statsA.WinBox.Y1, $statsA.WinBox.W, $statsA.WinBox.H, $statsA.WinBox.Fill)
        }
        Add-TestNote $result ("frame {0}x{1}: distinctColors={2} nonBlack={3}/{4} winColor=#{5:X6} winCount={6} winBox={7}" -f
                              $fa.w, $fa.h, $statsA.Colors, $statsA.NonBlack, $statsA.Pixels, $statsA.WinKey, $statsA.WinCount, $boxDesc)

        if ($statsA.NonBlack -ge 100000) {
            Add-TestCheck $result "pixels: not all black (nonBlack=$($statsA.NonBlack) >= 100000)" 'PASS'
        } else {
            Add-TestCheck $result 'pixels: not all black' 'FAIL' "nonBlack=$($statsA.NonBlack) (< 100000)"
        }
        if ($statsA.Colors -ge 32) {
            Add-TestCheck $result "pixels: distinct colors >= 32 ($($statsA.Colors))" 'PASS'
        } else {
            Add-TestCheck $result 'pixels: distinct colors >= 32' 'FAIL' "only $($statsA.Colors)"
        }
        $wb = $statsA.WinBox
        if ($statsA.WinKey -ge 0 -and $statsA.WinCount -ge 300000 -and $wb -and
            $wb.W -ge 600 -and $wb.W -le 900 -and $wb.H -ge 450 -and $wb.H -le 700 -and $wb.Fill -ge 0.75) {
            Add-TestCheck $result "pixels: GUI window rect ($($wb.W)x$($wb.H) fill=$($wb.Fill.ToString('P1')))" 'PASS'
        } else {
            Add-TestCheck $result 'pixels: GUI window rect' 'FAIL' ("winKey=#{0:X6} count={1} box={2}" -f $statsA.WinKey, $statsA.WinCount, $boxDesc)
        }
    } else {
        Add-TestCheck $result 'screenshot: screendump A' 'FAIL' 'ppm not produced'
    }

    # ---- 3. mouse: mouse_move -> WM_MOUSEMOVE (mpkt) + cursor pixel diff ----
    $mouseInitOk = $log.Contains('[shim] mouse init ok (AUX streaming, polled)')
    Send-QemuMouseMove -Session $s -Dx 120 -Dy 60
    Start-Sleep -Milliseconds 700
    Send-QemuMouseMove -Session $s -Dx 40 -Dy 20
    $hitMv = Wait-QemuLog -Session $s -Patterns @('[shim] mpkt dx=', '[PANIC]', '[IDT] exception') -TimeoutSeconds 60
    if ($mouseInitOk -and $hitMv -eq '[shim] mpkt dx=') {
        Add-TestCheck $result 'mouse: AUX init + mouse_move -> WM_MOUSEMOVE (mpkt)' 'PASS'
    } else {
        Add-TestCheck $result 'mouse: AUX init + mouse_move -> WM_MOUSEMOVE (mpkt)' 'FAIL' "init=$mouseInitOk hit=$hitMv"
    }

    Start-Sleep -Seconds 2
    $cursorDiff = -1
    if ($fa -and (Get-QemuScreenshot -Session $s -OutPath $ppmB)) {
        $fb = Read-PpmFile $ppmB
        $x0 = 4; $y0 = 4; $x1 = $fa.w - 5; $y1 = $fa.h - 5
        if ($statsA.WinBox) {
            $x0 = $statsA.WinBox.X0 + 4; $y0 = $statsA.WinBox.Y0 + 4
            $x1 = $statsA.WinBox.X1 - 4; $y1 = $statsA.WinBox.Y1 - 4
        }
        $cursorDiff = Compare-PpmRegion -A $fa -B $fb -X0 $x0 -Y0 $y0 -X1 $x1 -Y1 $y1
        Add-TestNote $result "cursor-region diff (sampled, region ($x0,$y0)-($x1,$y1)): $cursorDiff px"
    }
    if ($cursorDiff -ge 8 -and $cursorDiff -le 60000) {
        Add-TestCheck $result "mouse: cursor-region pixel diff after move ($cursorDiff px)" 'PASS'
    } else {
        Add-TestCheck $result 'mouse: cursor-region pixel diff after move' 'FAIL' "diff=$cursorDiff (expect 8..60000)"
    }

    # ---- 4. mouse buttons: mouse_button -> WM_LBUTTONDOWN/UP edges ----
    [void](Invoke-QemuMonitor -Session $s -Command 'mouse_button 1' -SettleMs 500)
    [void](Invoke-QemuMonitor -Session $s -Command 'mouse_button 0' -SettleMs 500)
    $hitUp = Wait-QemuLog -Session $s -Patterns @('[shim] LUP', '[PANIC]', '[IDT] exception') -TimeoutSeconds 60
    $log = Read-QemuLogText $s.LogPath
    if ($hitUp -eq '[shim] LUP' -and $log.Contains('[shim] LDOWN lp=')) {
        Add-TestCheck $result 'mouse: mouse_button -> WM_LBUTTONDOWN/UP edges' 'PASS'
    } else {
        Add-TestCheck $result 'mouse: mouse_button -> WM_LBUTTONDOWN/UP edges' 'FAIL' "hit=$hitUp ldown=$($log.Contains('[shim] LDOWN lp='))"
    }

    # ---- 5. keyboard: sendkey -> WM_KEYDOWN / WM_CHAR / WM_KEYUP ----
    Send-QemuKeys -Session $s -Keys @('a')
    $hitK = Wait-QemuLog -Session $s -Patterns @('[shim] CHAR ch=', '[PANIC]', '[IDT] exception') -TimeoutSeconds 60
    $log = Read-QemuLogText $s.LogPath
    if ($hitK -eq '[shim] CHAR ch=' -and $log.Contains('[shim] KEYDOWN vk=') -and $log.Contains('[shim] KEYUP vk=')) {
        Add-TestCheck $result 'keyboard: sendkey -> WM_KEYDOWN/WM_CHAR/WM_KEYUP' 'PASS'
    } else {
        Add-TestCheck $result 'keyboard: sendkey -> WM_KEYDOWN/WM_CHAR/WM_KEYUP' 'FAIL' "hit=$hitK"
    }

    # ---- 6. Esc -> WM_CLOSE -> ExitProcess(0) -> cmd continues ----
    Send-QemuKeys -Session $s -Keys @('esc')
    $hitQ = Wait-QemuLog -Session $s -Patterns @('[cmd] PE run end', '[PANIC]', '[IDT] exception') -TimeoutSeconds 90
    $log = Read-QemuLogText $s.LogPath
    if ($hitQ -eq '[cmd] PE run end') {
        Add-TestCheck $result 'exit chain: Esc -> WM_CLOSE -> ExitProcess(0) -> cmd continues' 'PASS'
    } else {
        Add-TestCheck $result 'exit chain: Esc -> WM_CLOSE -> ExitProcess(0) -> cmd continues' 'FAIL' "hit=$hitQ"
    }
    Assert-QemuLog $result $log -Ordered @(
        '[deaicup] quit, ExitProcess(0)',
        '[PE] ExitProcess code=',
        '[cmd] PE run end'
    ) -MustNotContain @('[PANIC]', '[IDT] exception', 'Unimplemented handler')

    Add-TestNote $result "accelerator: $script:UsedAccel"
} finally {
    if ($s) { Stop-QemuSession $s }
    Restore-DeshabScenario
}

Write-TestCaseResult $result
if ($result.Status -eq 'FAIL') { exit 1 } else { exit 0 }
