<#
  Cho MeowBox PC agent tự chạy khi đăng nhập Windows.
    .\install_autostart.ps1              # tạo task "MeowBox Agent" trong Task Scheduler (mặc định)
    .\install_autostart.ps1 -Startup     # hoặc: tạo shortcut trong thư mục Startup
    .\install_autostart.ps1 -Uninstall   # gỡ cả hai
  Chạy bằng pythonw.exe nên không có cửa sổ; log ở %APPDATA%\MeowBox\agent.log
#>
param([switch]$Startup, [switch]$Uninstall)
$ErrorActionPreference = "Stop"

$TaskName = "MeowBox Agent"
$Agent = Join-Path $PSScriptRoot "agent.py"
$Lnk = Join-Path ([Environment]::GetFolderPath("Startup")) "MeowBox Agent.lnk"

if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item $Lnk -ErrorAction SilentlyContinue
    Write-Host "Đã gỡ autostart."
    return
}

$Pythonw = (Get-Command pythonw.exe -ErrorAction SilentlyContinue).Source
if (-not $Pythonw) {
    $Pythonw = Join-Path (Split-Path (Get-Command python.exe).Source) "pythonw.exe"
}
if (-not (Test-Path $Pythonw)) { throw "Không tìm thấy pythonw.exe" }

if ($Startup) {
    $sh = (New-Object -ComObject WScript.Shell).CreateShortcut($Lnk)
    $sh.TargetPath = $Pythonw
    $sh.Arguments = "`"$Agent`" run"
    $sh.WorkingDirectory = $PSScriptRoot
    $sh.Save()
    Write-Host "Đã tạo shortcut: $Lnk"
} else {
    $action = New-ScheduledTaskAction -Execute $Pythonw -Argument "`"$Agent`" run" -WorkingDirectory $PSScriptRoot
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 99 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings `
        -Description "MeowBox PC agent (stats, media, YouTube relay)" -Force | Out-Null
    Write-Host "Đã tạo task '$TaskName'. Chạy ngay: Start-ScheduledTask -TaskName '$TaskName'"
}
