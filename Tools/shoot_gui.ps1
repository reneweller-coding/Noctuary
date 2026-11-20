# Screenshot of the Noctuary standalone window, so a GUI change can be looked at instead of
# imagined. Starts the app, waits for the window, captures it, closes it again.
#
#   powershell -File Tools\shoot_gui.ps1 -Out docs\screenshot.png [-Width 1500] [-Height 920]
#                                        [-Wait 6] [-Page ""|perform|browse|browse-map] [-Tab "5,1"]
#                                        [-Maximize] [-Layout 0..2] [-KeepSettings] [-Exe <path>]
#
# The instrument always starts muted (AMBIENT_MUTE=1): a picture is never a reason for sound.
# -Maximize maximises the window the way its title bar's button does (ShowWindow, no input is
# simulated). -KeepSettings puts the user's settings and window files back afterwards, byte for
# byte: the standalone writes both on the way out, and -Fresh deletes the first.
param(
    [string]$Out = "docs\screenshot.png",
    [int]$Width = 0,
    [int]$Height = 0,
    [int]$Wait = 6,
    [string]$Page = "",
    [string]$Tab = "",      # "<row>,<page>[;...]": open a tabbed row on one of its later pages
    [string]$Mod = "",      # lfo | env | matrix: which tab of the modulation strip
    [switch]$Fresh,         # forget the standalone's saved window size, so it opens at its own fit
    [switch]$Maximize,      # maximise the window before the picture
    [switch]$Restore,       # with -Maximize: maximise, then restore (the way back to a window)
    [int]$Layout = -1,      # 0 Normal, 1 Compact, 2 Expanded (AMBIENT_LAYOUT); -1 the recalled one
    [switch]$KeepSettings,  # restore Noctuary.settings and Noctuary.frame afterwards
    [string]$Exe = ""       # another build of the standalone (the build tree's, when bin\ is locked)
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = if ($Exe) { $Exe } else { Join-Path $root "bin\msvc\Noctuary.exe" }
if (-not (Test-Path $exe)) { throw "standalone not built: $exe" }
$env:AMBIENT_MUTE = "1"
if ($Layout -ge 0) { $env:AMBIENT_LAYOUT = "$Layout" } else { Remove-Item env:AMBIENT_LAYOUT -ErrorAction SilentlyContinue }
$userDir = Join-Path $env:APPDATA "Noctuary"
$kept = @{}
if ($KeepSettings) {
    foreach ($name in @("Noctuary.settings", "Noctuary.frame")) {
        $p = Join-Path $userDir $name
        if (Test-Path $p) { $kept[$p] = [System.IO.File]::ReadAllBytes($p) }
    }
}

# The editor reads these on start (see PluginEditor.cpp).
if ($Page -eq "perform") { $env:AMBIENT_PERFORM = "1" } else { Remove-Item env:AMBIENT_PERFORM -ErrorAction SilentlyContinue }
if ($Page -eq "browse") { $env:AMBIENT_BROWSE = "1" }
elseif ($Page -eq "browse-map") { $env:AMBIENT_BROWSE = "map" }
else { Remove-Item env:AMBIENT_BROWSE -ErrorAction SilentlyContinue }
if ($Tab) { $env:AMBIENT_TAB = $Tab } else { Remove-Item env:AMBIENT_TAB -ErrorAction SilentlyContinue }
if ($Mod) { $env:AMBIENT_MOD = $Mod } else { Remove-Item env:AMBIENT_MOD -ErrorAction SilentlyContinue }

if ($Fresh) {
    $settings = Join-Path $env:APPDATA "Noctuary\Noctuary.settings"
    if (Test-Path $settings) { Remove-Item $settings -Force }
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int t, bool repaint);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  // PrintWindow with PW_RENDERFULLCONTENT captures the window itself, so anything lying on top
  // of it (a browser, an alert) does not end up in the picture -- CopyFromScreen does.
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  // Without this the process is DPI-virtualised: GetWindowRect comes back in logical pixels and
  // PrintWindow renders only the top-left part of the window at physical size. At 150 % that
  // showed two thirds of the editor, and every layout "bug" it seemed to reveal was this.
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

[void][Win]::SetProcessDPIAware()
$proc = Start-Process -FilePath $exe -PassThru
try {
    $deadline = (Get-Date).AddSeconds($Wait + 25)
    $h = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 400
        $proc.Refresh()
        if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $h = $proc.MainWindowHandle; break }
    }
    if ($h -eq [IntPtr]::Zero) { throw "no window appeared" }
    Start-Sleep -Seconds 2          # the window keeper puts the window back where it was left first
    if ($Width -gt 0 -and $Height -gt 0) { [Win]::MoveWindow($h, 40, 40, $Width, $Height, $true) | Out-Null }
    if ($Maximize) { [Win]::ShowWindow($h, 3) | Out-Null }       # 3 = SW_MAXIMIZE
    if ($Maximize -and $Restore) { Start-Sleep -Seconds 2; [Win]::ShowWindow($h, 9) | Out-Null }   # 9 = SW_RESTORE
    [Win]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Seconds $Wait      # let the editor paint and the meters fill

    $r = New-Object Win+RECT
    [Win]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $t = $r.B - $r.T
    if ($w -le 0 -or $t -le 0) { throw "window has no area" }
    $bmp = New-Object System.Drawing.Bitmap $w, $t
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $dc = $g.GetHdc()
    $ok = [Win]::PrintWindow($h, $dc, 2)          # 2 = PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    if (-not $ok) {
        $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size($w, $t)))
    }
    $full = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $full) | Out-Null
    $bmp.Save($full, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Output "$full  ($w x $t)"
}
finally {
    if (-not $proc.HasExited) { $proc.CloseMainWindow() | Out-Null; $proc.WaitForExit(6000) | Out-Null }
    if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(3000) | Out-Null }
    foreach ($p in $kept.Keys) { [System.IO.File]::WriteAllBytes($p, $kept[$p]) }
}
