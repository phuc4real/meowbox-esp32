# Run idf.py for the MeowBox firmware in a container (ESP-IDF pinned in Dockerfile). Windows PowerShell.
#   firmware\docker\idf.ps1                 build (default)
#   firmware\docker\idf.ps1 fullclean       any other idf.py arguments
# Engine: $env:MEOWBOX_DOCKER if set, else kiri if installed, else docker.
# Output goes to firmware\build-docker\ (separate from a native build\). Flash from the host: firmware/README.md.
$ErrorActionPreference = 'Stop'
$fw = Split-Path -Parent $PSScriptRoot
$engine = if ($env:MEOWBOX_DOCKER) { $env:MEOWBOX_DOCKER } elseif (Get-Command kiri -ErrorAction SilentlyContinue) { 'kiri' } else { 'docker' }

# (Re)build the image only when the Dockerfile changed: exporting the 4 GB image takes minutes even when cached
$sum = (Get-FileHash (Join-Path $PSScriptRoot 'Dockerfile') -Algorithm SHA256).Hash.ToLower().Substring(0, 12)
$image = "meowbox-idf:$sum"
& $engine image inspect $image *> $null
if ($LASTEXITCODE -ne 0) {
    & $engine build -t $image $PSScriptRoot
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

[string[]]$idfArgs = if ($args.Count -gt 0) { $args } else { 'build' }
$opts = @()
if (-not [Console]::IsInputRedirected -and -not [Console]::IsOutputRedirected) { $opts += '-it' }
# Mount the whole repo: the firmware includes ../hw
& $engine run --rm @opts -v "$(Split-Path -Parent $fw):/meowbox" -w /meowbox/firmware $image `
    idf.py -B build-docker -DSDKCONFIG=build-docker/sdkconfig @idfArgs
exit $LASTEXITCODE
