# Start Noctuary from the tree, with the library this tree carries, and NOT muted.
#
#   powershell -File Tools\play.ps1 [-Dev] [-Preset "Tidal Expanse"] [-Layout 0|1|2]
#
# A double-click on the built Noctuary.exe finds only the 256 compiled-in presets: the packs and
# their recordings live in Library/ here, while an installed copy reads ProgramData\Noctuary. This
# points AMBIENT_PACKS at Library\Packs, which is also what makes the Journeys appear -- they are
# looked for one level above every loaded pack, so Library\Journeys comes with it.
#
# AMBIENT_MUTE is cleared on purpose. Every other script here sets it, because a standalone that
# photographs itself should be silent; this one exists to be heard.
param(
    [switch]$Dev,                 # the everyday MSVC build instead of the Intel release build
    [string]$Preset = "",         # open on a named preset
    [ValidateRange(0, 2)]
    [int]$Layout = -1             # 0 normal, 1 compact, 2 expanded (the Journey row needs the room of 2)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$tree = if ($Dev) { "build" } else { "build-release-intel" }
$exe = Join-Path $root "$tree\Plugin\Noctuary_artefacts\Release\Standalone\Noctuary.exe"
if (-not (Test-Path $exe)) { throw "not built: $exe" }

Remove-Item env:AMBIENT_MUTE -ErrorAction SilentlyContinue
$env:AMBIENT_PACKS = Join-Path $root "Library\Packs"
if ($Preset) { $env:AMBIENT_PRESET = $Preset } else { Remove-Item env:AMBIENT_PRESET -ErrorAction SilentlyContinue }
if ($Layout -ge 0) { $env:AMBIENT_LAYOUT = "$Layout" } else { Remove-Item env:AMBIENT_LAYOUT -ErrorAction SilentlyContinue }

$p = Start-Process $exe -PassThru
Write-Output ("{0}  (PID {1}, packs: {2})" -f $exe, $p.Id, $env:AMBIENT_PACKS)
