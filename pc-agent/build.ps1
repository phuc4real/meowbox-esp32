<#
  Build MeowBoxAgent-Setup-<version>.exe (docs/firmware_v0.2.0.md, 2.10-2.11):
    .\build.ps1            # icon + version resource -> PyInstaller (onedir, two exe) -> Inno Setup
    .\build.ps1 -Venv      # same, in a fresh .venv (clean dependency set; needs internet for pip)
  Optional bundled tools: put them in packaging\vendor\ before building (not in git):
    ffmpeg.exe   an LGPL build (e.g. BtbN FFmpeg-Builds "win64-lgpl"), its licence files in vendor\licenses\
    yt-dlp.exe   from github.com/yt-dlp/yt-dlp/releases (the agent copies it to %LOCALAPPDATA%\MeowBox\bin so -U works)
  Without them the installer still works; the agent then needs ffmpeg / yt-dlp in PATH for YouTube.
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
    foreach ($t in "ffmpeg.exe", "yt-dlp.exe") {
        if (-not (Test-Path "vendor\$t")) { Write-Warning "vendor\$t missing: the installer will not bundle it" }
    }
    $Iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $Iscc) { throw "Inno Setup 6 not found (winget install JRSoftware.InnoSetup)" }
    & $Iscc /Q "/DAppVersion=$Version" MeowBoxAgent.iss
    if ($LASTEXITCODE) { throw "Inno Setup failed" }
    $Out = Join-Path $Pkg "build\MeowBoxAgent-Setup-$Version.exe"
    Write-Host ("-> {0} ({1:N1} MB)" -f $Out, ((Get-Item $Out).Length / 1MB))
} finally { Pop-Location }
