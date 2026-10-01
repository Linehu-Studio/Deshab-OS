# Launch visible verify detached from Cursor's wsl pipe (that pipe kills WSL).
$Root = "D:\Code\DEAICUP\Deshab"
$Out = Join-Path $Root ".build_tmp\wsl_verify_detached.out"
$Err = Join-Path $Root ".build_tmp\wsl_verify_detached.err"
New-Item -ItemType Directory -Force -Path (Join-Path $Root ".build_tmp") | Out-Null
"" | Set-Content -Path $Out -Encoding utf8
"" | Set-Content -Path $Err -Encoding utf8
$p = Start-Process -FilePath "wsl.exe" `
    -ArgumentList @("-d", "Ubuntu", "-u", "root", "bash", "/mnt/d/Code/DEAICUP/Deshab/CODE/linux/tmp_run_visible.sh") `
    -RedirectStandardOutput $Out `
    -RedirectStandardError $Err `
    -WindowStyle Hidden `
    -PassThru
Write-Host "detached pid=$($p.Id) out=$Out"
