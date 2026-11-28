<#
  Build MeowBoxAgent-Setup-<version>.exe:
    .\build.ps1            # cargo build --release (MeowBoxAgent.exe + meowbox.exe) -> Inno Setup
    .\build.ps1 -SkipTests # same without cargo test first
  Nothing third-party is bundled: the installer downloads yt-dlp (meowbox tools update), the agent downloads ffmpeg
  the first time YouTube live is used. Both live in %LOCALAPPDATA%\MeowBox\bin and update from Settings.
  Version: `version` in Cargo.toml (same number as the firmware).
#>
param([switch]$SkipTests)
$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot
$Pkg = Join-Path $Here "packaging"
$Dist = Join-Path $Pkg "build\dist\MeowBoxAgent"

$Version = (Select-String -Path "$Here\Cargo.toml" -Pattern '^version\s*=\s*"([^"]+)"' | Select-Object -First 1).Matches[0].Groups[1].Value
Write-Host "MeowBox Agent $Version"

Push-Location $Here
try {
    if (-not $SkipTests) {
        cargo test --quiet
        if ($LASTEXITCODE) { throw "cargo test failed" }
    }
    cargo build --release --quiet
    if ($LASTEXITCODE) { throw "cargo build failed" }
} finally { Pop-Location }

if (Test-Path $Dist) { Remove-Item -Recurse -Force $Dist }
New-Item -ItemType Directory -Force $Dist | Out-Null
foreach ($exe in "MeowBoxAgent.exe", "meowbox.exe") { Copy-Item "$Here\target\release\$exe" $Dist }
# the window's display fonts are embedded in MeowBoxAgent.exe; their SIL OFL goes with them
New-Item -ItemType Directory -Force "$Dist\licenses" | Out-Null
Copy-Item "$Here\assets\fonts\OFL-*.txt" "$Dist\licenses"

$Iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Iscc) { throw "Inno Setup 6 not found (winget install JRSoftware.InnoSetup)" }
Push-Location $Pkg
try {
    & $Iscc /Q "/DAppVersion=$Version" MeowBoxAgent.iss
    if ($LASTEXITCODE) { throw "Inno Setup failed" }
} finally { Pop-Location }
$Out = Join-Path $Pkg "build\MeowBoxAgent-Setup-$Version.exe"
Write-Host ("-> {0} ({1:N1} MB)" -f $Out, ((Get-Item $Out).Length / 1MB))
