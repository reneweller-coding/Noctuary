# Noctuary -- the manual on its own, from a build that is already there, with the standalone
# MUTED while it photographs itself.
#
#   powershell -File Deploy\export_manual.ps1 [-Toolchain intel|msvc]
#
# build_release.ps1 does the same as part of a release, but it clears AMBIENT_MUTE for the tests
# and then starts the standalone for the pictures unmuted -- ninety seconds of the instrument
# playing out of the speakers, which at six in the morning is not what anybody asked for. So the
# release is built in two passes (-SkipManual -NoSetup, then -SkipBuild -SkipManual) with this in
# between: the same export, the same work folder, the same copy into docs\manual, and silence.
param(
    [ValidateSet("msvc", "intel")]
    [string]$Toolchain = "intel"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root
$python = "Tools\TextureGen\.venv\Scripts\python.exe"
$buildDir = Join-Path $root ($(if ($Toolchain -eq "intel") { "build-release-intel" } else { "build-release" }))
$exe = Join-Path $buildDir "Plugin\Noctuary_artefacts\Release\Standalone\Noctuary.exe"
if (-not (Test-Path $exe)) { throw "no release standalone at $exe -- run build_release.ps1 -SkipManual -NoSetup first" }

$manualDir = Join-Path $root "docs\manual"
$manualWork = Join-Path $root "Deploy\manual-work"
if (Test-Path $manualWork) { Remove-Item $manualWork -Recurse -Force }
New-Item -ItemType Directory -Force $manualWork | Out-Null
$env:AMBIENT_MUTE = "1"                    # the pictures are the same; the sound is not wanted
$env:AMBIENT_PRESET = "Tidal Expanse"      # four sources, the Cosmos, a delay and a strike in use
$env:AMBIENT_MANUAL = $manualWork
$clip = Get-ChildItem (Join-Path $root "Library\FieldRecordings") -Filter "*_loop.flac" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -First 1
if ($clip) { $env:AMBIENT_MANUAL_CLIP = $clip.FullName }
$env:AMBIENT_PACKS = Join-Path $root "Library\Packs"
$mp = Start-Process $exe -PassThru
if (-not $mp.WaitForExit(120000)) { $mp.Kill(); throw "the manual export did not finish" }
Remove-Item env:AMBIENT_MANUAL, env:AMBIENT_PRESET, env:AMBIENT_MANUAL_CLIP, env:AMBIENT_PACKS, env:AMBIENT_MUTE -ErrorAction SilentlyContinue
& $python (Join-Path $root "Tools\make_manual.py") --dir $manualWork
if ($LASTEXITCODE -ne 0) { throw "the manual did not print" }
New-Item -ItemType Directory -Force $manualDir | Out-Null
$copyFailed = $false
Get-ChildItem $manualWork -File | ForEach-Object {
    try { Copy-Item $_.FullName (Join-Path $manualDir $_.Name) -Force -ErrorAction Stop }
    catch { $copyFailed = $true }
}
if ($copyFailed) { Write-Warning "docs\manual could not be fully updated (a file is open there); the installer takes the manual from $manualWork" }
$pdf = Join-Path $manualWork "Noctuary-Manual.pdf"
if (Test-Path $pdf) { Write-Host ("  manual: {0:N1} MB" -f ((Get-Item $pdf).Length / 1MB)) -ForegroundColor Green } else { throw "no manual PDF was written" }
