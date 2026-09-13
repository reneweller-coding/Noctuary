# Screenshot of the Noctuary standalone window, so a GUI change can be looked at instead of
# imagined. Starts the app, waits for the window, captures it, closes it again.
#
#   powershell -File Tools\shoot_gui.ps1 -Out docs\screenshot.png [-Width 1500] [-Height 920]
#                                        [-Wait 6] [-Page ""|perform|browse|browse-map] [-Tab "5,1"]
param(
    [string]$Out = "docs\screenshot.png",
    [int]$Width = 0,
    [int]$Height = 0,
    [int]$Wait = 6,
    [string]$Page = "",
    [string]$Tab = "",      # "<row>,<page>[;...]": open a tabbed row on one of its later pages
    [string]$Mod = "",      # lfo | env | matrix: which tab of the modulation strip
    [switch]$Fresh          # forget the standalone's saved window size, so it opens at its own fit
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $root "build\Plugin\Noctuary_artefacts\Release\Standalone\Noctuary.exe"
if (-not (Test-Path $exe)) { throw "standalone not built: $exe" }

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
    if ($Width -gt 0 -and $Height -gt 0) { [Win]::MoveWindow($h, 40, 40, $Width, $Height, $true) | Out-Null }
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
    if (-not $proc.HasExited) { $proc.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 800 }
    if (-not $proc.HasExited) { $proc.Kill() }
}
