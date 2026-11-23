<#
  Build MeowBoxAgent-Setup-<version>.exe (docs/firmware_v0.2.0.md, 2.10-2.11):
    .\build.ps1            # icon + version resource -> PyInstaller (onedir, two exe) -> Inno Setup
    .\build.ps1 -Venv      # same, in a fresh .venv (clean dependency set; needs internet for pip)
  Nothing third-party is bundled: the installer downloads yt-dlp (meowbox tools update), the agent downloads ffmpeg
  the first time YouTube live is used. Both live in %LOCALAPPDATA%\MeowBox\bin and update from Settings.
#>
param([switch]$Venv)
$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot
$Pkg = Join-Path $Here "packaging"

$Py = "python"
if ($Venv) {
    if (-not (Test-Path "$Here\.venv")) { python -m venv "$Here\.venv" }
    $Py = "$Here\.venv\Scripts\python.exe"
    & $Py -m pip install -q -r "$Here\requirements.txt" pyinstaller
}

$Version = (& $Py "$Pkg\make_assets.py" | Select-Object -Last 1).Trim()
Write-Host "MeowBox Agent $Version"
Push-Location $Pkg
try {
    & $Py -m PyInstaller --noconfirm --clean --distpath build\dist --workpath build\work meowbox.spec
    if ($LASTEXITCODE) { throw "PyInstaller failed" }
    $Iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Iscc) { throw "Inno Setup 6 not found (winget install JRSoftware.InnoSetup)" }
    & $Iscc /Q "/DAppVersion=$Version" MeowBoxAgent.iss
    if ($LASTEXITCODE) { throw "Inno Setup failed" }
    $Out = Join-Path $Pkg "build\MeowBoxAgent-Setup-$Version.exe"
    Write-Host ("-> {0} ({1:N1} MB)" -f $Out, ((Get-Item $Out).Length / 1MB))
} finally { Pop-Location }
