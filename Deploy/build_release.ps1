# Noctuary -- build the binaries other people get, and wrap them in a setup.
#
#   powershell -File Deploy\build_release.ps1 [-Version 1.0.0] [-SkipBuild] [-NoSetup]
#                                             [-Toolchain msvc|intel]
#
# Three things make this build different from an everyday one:
#
#   * the MSVC runtime is linked in (AMBIENT_STATIC_RUNTIME), so nothing has to be installed
#     first -- no redistributable, no DLL beside the executable;
#   * AVX2 is on (39x realtime without it, 52x with it, both measured). Every x86-64 processor
#     since 2013 has it, which is every machine anybody makes music on; the setup checks for it
#     before installing rather than letting an old one fail with an illegal instruction;
#   * it builds in its own folder, so the everyday build tree is left alone.
#
# The result is Deploy\out\Noctuary-<version>-Setup.exe plus Deploy\out\Noctuary-<version>-portable.zip
# for people who would rather not run an installer at all.
param(
    [string]$Version = "",
    # Which release the sample archives hang on. It is NOT always the release being built: the
    # content is 8 GB and only changes when the library does, so it stays with the tag that
    # introduced it and later installers point back at that one. Hardcoding it in the .iss meant
    # every installer since 1.0.0 asked v1.0.0 for files that had moved -- a 404 in the middle of
    # somebody's install, which is where this was finally noticed.
    [string]$ContentTag = "library-v5",   # 2.0: the library package (57 archives; Deploy/content-*.iss name them)
    # Code signing. Without it Windows shows "Unknown publisher" on the first run of the setup --
    # SmartScreen has nothing to go on but the file's reputation, and a fresh file has none.
    #   -SignWith "<thumbprint>"   a certificate in the current user's store (signtool /sha1)
    #   -SignWith "<file.pfx>"     a PFX on disk; -SignPassword goes with it
    # An EV certificate or Azure Trusted Signing removes the warning at once; an ordinary OV one
    # only earns it back over downloads. Unsigned is not dangerous, it is unattested: anyone can
    # check the SHA-256 below against the release page instead.
    [string]$SignWith = "",
    [string]$SignPassword = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [switch]$SkipBuild,       # reuse whatever is in build-release already
    [switch]$NoSetup,         # stage and zip, but do not call the Inno compiler
    [switch]$SkipManual,      # reuse the manual already in docs/manual
    # Which compiler builds the thing people get. "msvc" is the one every release so far was made
    # with. "intel" is oneAPI's icx, which on the same source and the same flags renders about a
    # fifth to a quarter faster -- 29 % on a bare offline render, 20 % through the plugin under
    # audio and automation, and only about 4 % in the standalone sitting idle, where nearly all
    # the work is the window rather than the sound.
    #
    # It is not a drop-in: oneAPI's own setvars.bat cannot find its per-component scripts on at
    # least one machine here, so the environment is set up by hand below; CMake calls icx
    # "IntelLLVM" rather than MSVC, so anything tied to if(MSVC) quietly does not apply; JUCE asks
    # for link-time optimisation, which makes icx emit LLVM bitcode that link.exe cannot read, so
    # the build needs lld; and there is no Visual Studio generator for it without the IDE
    # integration, so it builds with NMake and therefore without parallelism.
    #
    # What it does NOT change: the instrument is generative, so different floating-point code
    # generation puts it on a different trajectory -- every render differs from the MSVC one. That
    # was measured rather than assumed. Over sixty presets the descriptors the map is built from
    # move by 5 % of a typical distance between two presets (Spearman 0.985 over every pair), and
    # the loudness by a hundredth of a decibel on average, 0.31 dB at worst. The map and the
    # loudness matching stay valid; the library does not need measuring again.
    # Intel is what ships. Every release from 1.11.3 on is built with it, so a plain run of this
    # script produces the thing that goes to the release page; -Toolchain msvc is there for
    # comparing the two, not for packaging.
    [ValidateSet("msvc", "intel")]
    [string]$Toolchain = "intel"
)
$python = "Tools\TextureGen\.venv\Scripts\python.exe"
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

if (-not $Version) {
    $m = Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'project\(Noctuary VERSION ([0-9.]+)'
    if (-not $m) { throw "no version in CMakeLists.txt and none given" }
    $Version = $m.Matches[0].Groups[1].Value
}
Write-Host "Noctuary $Version" -ForegroundColor Cyan

$intel = $Toolchain -eq "intel"
# A tree of its own per compiler: the two produce different objects from the same sources, and
# sharing a build directory between them means a rebuild that looks incremental and is not.
$buildDir = Join-Path $root ($(if ($intel) { "build-release-intel" } else { "build-release" }))
$stage = Join-Path $root "Deploy\stage"
$out = Join-Path $root "Deploy\out"

# Where the test binaries land. The Visual Studio generator is multi-configuration and puts them
# under the configuration's name; NMake, which is what the Intel build has to use, does not.
$testDir = Join-Path $buildDir ($(if ($intel) { "Tests" } else { "Tests\Release" }))

# Runs a batch file for its environment and keeps what it set. Visual Studio and oneAPI both ship
# their setup as batch files, and a batch file cannot change the environment of the PowerShell
# that called it -- so it is called in a cmd of its own, and what it left behind is read back.
function Import-CmdEnvironment([string]$batch) {
    if (-not (Test-Path $batch)) { throw "not found: $batch" }
    $tmp = [System.IO.Path]::GetTempFileName()
    cmd /c " `"$batch`" > nul 2>&1 && set > `"$tmp`" "
    foreach ($line in Get-Content $tmp) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
    Remove-Item $tmp -Force
}

# Everything oneAPI's setvars.bat would have done, done here instead, because on this machine it
# cannot find the per-component scripts it is supposed to call and reports each one as missing
# while returning success. Four directories: the compiler, its linker (lld-link lives one level
# down from icx and is needed because JUCE asks for link-time optimisation), its libraries and its
# headers.
function Enable-IntelToolchain {
    $vs = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue |
          Select-Object -First 1 -ExpandProperty FullName
    if (-not $vs) { throw "vcvars64.bat not found -- the Intel build still links with the Microsoft linker" }
    Import-CmdEnvironment $vs
    $icx = Get-ChildItem "C:\Program Files (x86)\Intel\oneAPI\compiler\*\bin\icx.exe" -ErrorAction SilentlyContinue |
           Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $icx) { throw "icx.exe not found -- is the oneAPI C++ compiler installed?" }
    $iroot = Split-Path -Parent (Split-Path -Parent $icx.FullName)
    $env:PATH = "$iroot\bin;$iroot\bin\compiler;$env:PATH"
    $env:LIB = "$iroot\lib;$env:LIB"
    $env:INCLUDE = "$iroot\include;$env:INCLUDE"
    Write-Host "  Intel oneAPI: $iroot" -ForegroundColor DarkGray
    return $icx.FullName
}

if (-not $SkipBuild) {
    # JUCE writes the Windows version resource once and does not notice afterwards that the
    # project's version has changed. Version 1.1.0 was therefore built, packaged, installed and
    # tested with 1.0.0 stamped inside the executable -- the setup was named right, the file
    # properties were wrong, and nothing in the build said so. Deleting it forces the regenerate.
    $rc = Join-Path $buildDir "Plugin\Noctuary_artefacts\JuceLibraryCode\Noctuary_resources.rc"
    if (Test-Path $rc) { Remove-Item $rc -Force }

    $common = @(
        "-DAMBIENT_STATIC_RUNTIME=ON", "-DAMBIENT_AVX2=ON", "-DAMBIENT_BUILD_TOOLS=ON",
        "-DFETCHCONTENT_SOURCE_DIR_JUCE=$root/build/_deps/juce-src"   # the JUCE already fetched
    )
    if ($intel) {
        $icx = Enable-IntelToolchain
        # NMake because there is no Visual Studio toolset for icx without the IDE integration, and
        # lld because JUCE turns on link-time optimisation, which makes icx write LLVM bitcode
        # where the Microsoft linker expects objects -- it stops with LNK1107 on the first one.
        $lld = "-fuse-ld=lld"
        cmake -S . -B $buildDir -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release `
            "-DCMAKE_C_COMPILER=$icx" "-DCMAKE_CXX_COMPILER=$icx" `
            "-DCMAKE_EXE_LINKER_FLAGS=$lld" "-DCMAKE_SHARED_LINKER_FLAGS=$lld" "-DCMAKE_MODULE_LINKER_FLAGS=$lld" `
            @common
        if ($LASTEXITCODE -ne 0) { throw "configure failed" }
        # NMake builds one file at a time, so this is slow and, unlike the -j 2 below, already
        # leaves the machine usable.
        cmake --build $buildDir
        if ($LASTEXITCODE -ne 0) { throw "build failed" }
    } else {
        # A build for other people is not worth having in a hurry: -j 2 leaves the machine usable.
        cmake -S . -B $buildDir -G "Visual Studio 18 2026" -A x64 @common
        if ($LASTEXITCODE -ne 0) { throw "configure failed" }
        cmake --build $buildDir --config Release --parallel 2
        if ($LASTEXITCODE -ne 0) { throw "build failed" }
    }

    # The tests are built in the same configuration that ships, and have to pass in it: a static
    # runtime and a missing AVX2 are exactly the kind of change that is fine until it is not.
    # The host test measures levels -- a transition has to stay audible -- and AMBIENT_MUTE, set
    # in the shell for every standalone that is started by hand, clears the buffer and makes every
    # level -180 dBFS. A release build failed its own tests that way once. The tests run unmuted;
    # they open no audio device.
    Remove-Item env:AMBIENT_MUTE -ErrorAction SilentlyContinue
    $env:AMBIENT_PACKS = Join-Path $root "Library\Packs"
    & (Join-Path $testDir "ambient_selftest.exe")
    if ($LASTEXITCODE -ne 0) { throw "self test failed in the release configuration" }
    & (Join-Path $testDir "ambient_hosttest.exe")
    if ($LASTEXITCODE -ne 0) { throw "host test failed in the release configuration" }
    # The two threads against each other, in the configuration that ships. Every serious fault
    # this instrument has had was a handover between them, so it is worth the four seconds.
    & (Join-Path $testDir "ambient_racetest.exe") 4
    if ($LASTEXITCODE -ne 0) { throw "race test failed in the release configuration" }
}

$art = Join-Path $buildDir "Plugin\Noctuary_artefacts\Release"
$exe = Join-Path $art "Standalone\Noctuary.exe"
$vst = Join-Path $art "VST3\Noctuary.vst3"
foreach ($p in @($exe, $vst)) { if (-not (Test-Path $p)) { throw "missing build output: $p" } }

# ---------------------------------------------------------------- what must not be there
# A binary that still wants the Visual C++ runtime would fail on a machine without it, and the
# failure looks like "the app just does not start". Checked here rather than discovered later.
$dumpbin = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue |
           Select-Object -First 1 -ExpandProperty FullName
if ($dumpbin) {
    foreach ($bin in @($exe, (Join-Path $vst "Contents\x86_64-win\Noctuary.vst3"))) {
        $deps = & $dumpbin /dependents $bin | Select-String -Pattern '^\s+\S+\.dll' | ForEach-Object { $_.Line.Trim() }
        # Intel's runtime is the same trap wearing another name: libmmd and svml_dispmd sit beside
        # icx and are found while building, and are nowhere on the machine of somebody who has
        # only ever installed a DAW. They do not appear when the runtime is linked in statically,
        # which is what AMBIENT_STATIC_RUNTIME does and why icx then picks libmmt over libmmd --
        # but that is a thing to check rather than to trust.
        $bad = $deps | Where-Object { $_ -match '^(VCRUNTIME|MSVCP|CONCRT|api-ms-win-crt|libmmd|svml|libiomp|libirng)' }
        if ($bad) { throw "$([System.IO.Path]::GetFileName($bin)) still needs a runtime nobody has: $($bad -join ', ')" }
        Write-Host ("  {0}: {1} system DLLs, none of them a redistributable" -f [System.IO.Path]::GetFileName($bin), $deps.Count)
    }
} else {
    Write-Warning "dumpbin not found -- the runtime check was skipped"
}

# ---------------------------------------------------------------- the manual
# Made from a running instrument: its pictures are snapshots of the real panel, so they cannot go
# out of date the way a drawing would. AMBIENT_PRESET picks a patch with all three sources and the
# effects in use, or the Sources chapter would be illustrated with a greyed-out section.
$manualDir = Join-Path $root "docs\manual"
# The manual is exported into a work folder of its own and only then copied into docs\manual.
# docs\manual is where people open the PDF, and a PDF open in a reader cannot be deleted or
# overwritten -- which twice cost a whole release build. The work folder is nobody's reading
# copy, so it can always be cleared; the copy into docs\manual is best effort, and the
# installer stages the PDF from the work folder either way.
$manualWork = Join-Path $root "Deploy\manual-work"
if (-not $SkipManual) {
    if (Test-Path $manualWork) { Remove-Item $manualWork -Recurse -Force }
    New-Item -ItemType Directory -Force $manualWork | Out-Null
    $env:AMBIENT_PRESET = "Tidal Expanse"   # four sources (FM, Bow, Harmonic, Wavetable), the Cosmos, a delay and a strike in use
    $env:AMBIENT_MANUAL = $manualWork
    # A clip for the manual's gallery of source types: the Texture and Stretch pictures show it
    # loaded and playing. Any seamless field recording will do; the first one alphabetically is
    # the same one every time.
    $clip = Get-ChildItem (Join-Path $root "Library\FieldRecordings") -Filter "*_loop.flac" -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -First 1
    if ($clip) { $env:AMBIENT_MANUAL_CLIP = $clip.FullName }
    # And the library: the browser and the map are photographed too, and without the packs they
    # show the 256 built-in presets of an instrument that ships with 14592.
    $env:AMBIENT_PACKS = Join-Path $root "Library\Packs"
    $mp = Start-Process $exe -PassThru
    if (-not $mp.WaitForExit(90000)) { $mp.Kill() ; throw "the manual export did not finish" }
    Remove-Item env:AMBIENT_MANUAL, env:AMBIENT_PRESET, env:AMBIENT_MANUAL_CLIP, env:AMBIENT_PACKS -ErrorAction SilentlyContinue
    & $python (Join-Path $root "Tools\make_manual.py") --dir $manualWork
    if ($LASTEXITCODE -ne 0) { throw "the manual did not print" }
    # Into docs\manual, as far as a reader lets us.
    New-Item -ItemType Directory -Force $manualDir | Out-Null
    $copyFailed = $false
    Get-ChildItem $manualWork -File | ForEach-Object {
        try { Copy-Item $_.FullName (Join-Path $manualDir $_.Name) -Force -ErrorAction Stop }
        catch { $copyFailed = $true }
    }
    if ($copyFailed) { Write-Warning "docs\manual could not be fully updated (a file is open there); the installer takes the manual from $manualWork" }
}
$manualPdf = if ($SkipManual) { Join-Path $manualDir "Noctuary-Manual.pdf" } else { Join-Path $manualWork "Noctuary-Manual.pdf" }

# ---------------------------------------------------------------- stage
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, $out | Out-Null
Copy-Item $exe $stage
Copy-Item $vst (Join-Path $stage "Noctuary.vst3") -Recurse
Copy-Item (Join-Path $root "docs\logo.ico") $stage
if (Test-Path $manualPdf) { Copy-Item $manualPdf $stage } else { Write-Warning "no manual PDF to ship" }
Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
New-Item -ItemType Directory -Force -Path (Join-Path $stage "Packs") | Out-Null
Copy-Item (Join-Path $root "Library\Packs\*.ambientpack") (Join-Path $stage "Packs")
$packCount = (Get-ChildItem (Join-Path $stage "Packs") -Filter *.ambientpack).Count
if ($packCount -lt 1) { throw "no preset packs staged" }
# The journey templates (Library\Journeys\*.journey): presets in a row for an evening, beside the packs.
$journeys = Join-Path $root "Library\Journeys"
if (Test-Path $journeys) {
    New-Item -ItemType Directory -Force -Path (Join-Path $stage "Journeys") | Out-Null
    Copy-Item (Join-Path $journeys "*.journey") (Join-Path $stage "Journeys")
}
$journeyCount = (Get-ChildItem (Join-Path $stage "Journeys") -Filter *.journey -ErrorAction SilentlyContinue).Count

@"
Noctuary $Version
=====================

A drone and ambient synthesiser: three source slots, two filters, a resonating body, delays, a
convolution room, a far reverb, the Cosmos feedback network, and a conductor that plays it.

WHAT IS HERE

  Noctuary.exe        the standalone instrument. Nothing else needs to be installed.
  Noctuary.vst3       the plug-in. Copy the whole folder to
                          C:\Program Files\Common Files\VST3\ and rescan in your DAW.
  Noctuary-Manual.pdf the manual, the same one the Help page shows.
  Packs\                  $packCount preset packs. Copy them to
                          C:\ProgramData\Noctuary\Packs (for everyone on the machine) or
                          Documents\Noctuary\Packs (just for you). The instrument reads both.
  Journeys\               $journeyCount journeys: presets in a row, each held for a while and
                          crossfaded into the next, for an evening that plays itself. Copy them
                          beside the packs (..\Noctuary\Journeys); your own go to
                          Documents\Noctuary\Journeys.

The setup does all of that for you; this archive is for anyone who would rather it did not.

FIRST RUN

Start it and wait: the conductor begins a piece within a few seconds. Point at any control to
read what it does; Help (or F1) opens the manual. The standalone comes back the way you left it
-- the Recall button in the header turns that off.

The preset library that the packs name (samples, wavetables and impulse responses) is a separate
download; presets that cannot find their sample fall back to the built-in sources and still play.

$(Get-Content (Join-Path $root "LICENSE") -TotalCount 1)
"@ | Set-Content (Join-Path $stage "README.txt") -Encoding utf8

# ---------------------------------------------------------------- portable archive
# A version number used to mean one binary. With a choice of compiler it can mean two, and the
# files here are named after the version alone -- so a run can quietly replace an archive that
# was published under that name with a different build of it, same name, different checksum.
# Said out loud rather than discovered later by somebody comparing hashes with a release page.
$zip = Join-Path $out "Noctuary-$Version-portable.zip"
if (Test-Path $zip) {
    Write-Warning ("replacing {0} (built {1}) with a {2} build" -f
                   [System.IO.Path]::GetFileName($zip), (Get-Item $zip).LastWriteTime, $Toolchain)
    Remove-Item $zip -Force
}
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal
Write-Host ("  portable zip: {0:N1} MB" -f ((Get-Item $zip).Length / 1MB))

# ---------------------------------------------------------------- signing
function Invoke-Sign([string]$path) {
    if (-not $SignWith) { return }
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
                Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $signtool) { throw "signtool.exe not found (Windows SDK). Install it, or build without -SignWith." }
    $args = @("sign", "/fd", "SHA256", "/tr", $TimestampUrl, "/td", "SHA256")
    if (Test-Path $SignWith) {
        $args += @("/f", $SignWith)
        if ($SignPassword) { $args += @("/p", $SignPassword) }
    } else {
        $args += @("/sha1", $SignWith)
    }
    & $signtool @args $path
    if ($LASTEXITCODE -ne 0) { throw "signing failed for $path" }
    Write-Host "  signed $(Split-Path $path -Leaf)" -ForegroundColor Green
}
# The executable is signed before it goes into the installer, and the installer after it is built:
# Windows checks both, and a signed setup that unpacks an unsigned exe warns on the exe instead.
Invoke-Sign (Join-Path $stage "Noctuary.exe")

# ---------------------------------------------------------------- setup
if (-not $NoSetup) {
    $iscc = Get-ChildItem "C:\Program Files\Inno Setup *\ISCC.exe", "C:\Program Files (x86)\Inno Setup *\ISCC.exe" -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
    if (-not $iscc) { throw "Inno Setup not found. winget install JRSoftware.InnoSetup, or run with -NoSetup." }
    $contentUrl = "https://github.com/reneweller-coding/Noctuary/releases/download/$ContentTag"
    Write-Host "  sample library from $contentUrl"
    & $iscc "/DVersion=$Version" "/DContentBaseUrl=$contentUrl" (Join-Path $root "Deploy\Noctuary.iss")
    if ($LASTEXITCODE -ne 0) { throw "the installer failed to build" }
    $setup = Join-Path $out "Noctuary-$Version-Setup.exe"
    Invoke-Sign $setup
    Write-Host ("  setup: {0:N1} MB" -f ((Get-Item $setup).Length / 1MB)) -ForegroundColor Green
}

# ---------------------------------------------------------------- checksums
# What an unsigned build can offer instead of a signature: the hashes, printed here and meant for
# the release notes, so anyone can check that the file they downloaded is the file that was built.
$sums = Join-Path $out "SHA256SUMS.txt"
Get-ChildItem $out -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content $sums -Encoding ascii
Write-Host "  checksums: $sums" -ForegroundColor Green
Get-Content $sums | ForEach-Object { Write-Host "    $_" }
Get-ChildItem $out | Format-Table Name, @{n="MB";e={"{0:N1}" -f ($_.Length/1MB)}}, LastWriteTime
