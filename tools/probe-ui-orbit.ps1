# tools/probe-ui-orbit.ps1 - is the ring's switch actually in the settings window, and does it work?
#
# A control that exists only in the source is not a feature, and QML fails quietly: a binding that
# cannot resolve leaves the row invisible rather than red. So this reads the running window through
# UI Automation, clicks into the tools tab, lists what the ring's group actually presents, then clicks
# the switch and asks the RENDERER whether the state moved - the far end, not the widget.
#
#   -Mode tabs      what the window offers at the top level
#   -Mode group     open tools and list the rows in it
#   -Mode click     as group, then click the ring's switch and read the engine back
param(
    [ValidateSet('tabs','group','click')][string]$Mode = 'group',
    [long]$Hwnd = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase, System.Drawing

$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct PPOINT { public int X, Y; }
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static string Cls(IntPtr h) { var b=new StringBuilder(128); GetClassNameW(h,b,128); return b.ToString(); }
  public static string Title(IntPtr h) { var b=new StringBuilder(256); GetWindowTextW(h,b,256); return b.ToString(); }
  public static List<IntPtr> Tops() {
    var l = new List<IntPtr>();
    EnumProc cb = (h, a) => { l.Add(h); return true; };
    EnumWindows(cb, IntPtr.Zero);
    GC.KeepAlive(cb);
    return l;
  }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;
  public static void Click(int x, int y) {
    SetCursorPos(x, y); Thread.Sleep(120);
    mouse_event(LEFTDOWN,0,0,0,IntPtr.Zero); Thread.Sleep(60);
    mouse_event(LEFTUP,0,0,0,IntPtr.Zero); Thread.Sleep(350);
  }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Runtime.InteropServices

# "tools" and "orbit ring", built from code points so this file stays ASCII.
$tools = ,([string][char]0x5DE5 + [char]0x5177)
$ring  = ,([string][char]0x5FEB + [char]0x6377 + [char]0x952E + [char]0x73AF)
$hide  = ,([string][char]0x9690 + [char]0x85CF + [char]0x684C + [char]0x9762 + [char]0x56FE + [char]0x6807)

$hwnd = [IntPtr]::Zero
if ($Hwnd -ne 0) {
    # Driving a second instance of our own window rather than the one the user may be working in.
    $hwnd = [IntPtr]$Hwnd
} else {
foreach ($h in [W]::Tops()) {
    if (-not [W]::IsWindowVisible($h)) { continue }
    if (-not [W]::Cls($h).StartsWith('Qt')) { continue }
    if ([W]::Title($h) -eq 'Wallpaper') { $hwnd = $h; break }
}
}
if ($hwnd -eq [IntPtr]::Zero) { Write-Output 'ABORT: no visible Wallpaper window'; exit 1 }
Write-Output ("main window 0x{0:X} class={1} title={2}" -f [int64]$hwnd, [W]::Cls($hwnd), [W]::Title($hwnd))

$root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)

function Find([string]$needle, [int]$depth) {
    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $needle)
    return $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
}

# The settings page is a swap inside this same window, not a dialog of its own, and the button that
# opens it is a toggle - clicking it twice closes it again, which is exactly what two runs of an
# earlier version of this script did to each other. So it is only clicked when the tab strip is not
# already on screen.
$toolsName = [string][char]0x5DE5 + [char]0x5177          # "tools"
function HasTools {
    $c = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty, $toolsName)
    return $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $c).Count -gt 0
}
if (-not (HasTools)) {
    $c2 = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::NameProperty,
        ([string][char]0x8BBE + [char]0x7F6E))            # "settings"
    $settings = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $c2)
    if ($settings.Count -gt 0) {
        $b = $settings[0].Current.BoundingRectangle
        [W]::Click([int]($b.X + $b.Width/2), [int]($b.Y + $b.Height/2))
        Start-Sleep -Milliseconds 1200
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
        Write-Output ("clicked the settings toggle; tools tab now visible: " + (HasTools))
    } else {
        Write-Output 'no settings button in this window and no tools tab'
    }
} else {
    Write-Output 'the tools tab was already on screen'
}

function ListNames([int]$max) {
    $all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
        [System.Windows.Automation.Condition]::TrueCondition)
    $n = 0
    foreach ($e in $all) {
        $nm = $e.Current.Name
        if ([string]::IsNullOrWhiteSpace($nm)) { continue }
        $r = $e.Current.BoundingRectangle
        Write-Output ("  {0,-22} {1,-14} @ {2},{3} {4}x{5}" -f ($e.Current.ControlType.ProgrammaticName -replace 'ControlType\.',''), $nm,
                      [int]$r.X, [int]$r.Y, [int]$r.Width, [int]$r.Height)
        if ((++$n) -ge $max) { Write-Output "  (stopped at $max)"; return }
    }
    Write-Output "  named elements total: $n"
}

if ($Mode -eq 'tabs') { ListNames 80; exit 0 }

$hits = Find $tools[0] 1
Write-Output ("elements named like the tools tab: " + $hits.Count)
if ($hits.Count -eq 0) { ListNames 60; exit 1 }
$r = $hits[0].Current.BoundingRectangle
Write-Output ("clicking the tools tab at " + ([int]($r.X + $r.Width/2)) + "," + ([int]($r.Y + $r.Height/2)))
[W]::Click([int]($r.X + $r.Width/2), [int]($r.Y + $r.Height/2))
Start-Sleep -Milliseconds 700

foreach ($n in @($ring[0], $hide[0])) {
    $f = Find $n 1
    Write-Output ("row '" + $n + "' found: " + $f.Count)
    foreach ($e in $f) {
        $b = $e.Current.BoundingRectangle
        Write-Output ("   " + ($e.Current.ControlType.ProgrammaticName -replace 'ControlType\.','') + " @ " +
                      ([int]$b.X) + "," + ([int]$b.Y) + " " + ([int]$b.Width) + "x" + ([int]$b.Height))
    }
}

if ($Mode -eq 'click') {
    $f = Find $ring[0] 1
    if ($f.Count -eq 0) { Write-Output 'VERDICT click: the ring row is not in the window'; exit 1 }
    $b = $f[0].Current.BoundingRectangle
    # The switch sits at the right end of the same row. The row's own text element does not span it,
    # so aim off the row's right edge rather than guessing a window-relative x.
    $x = [int]($b.X + $b.Width + 220); $y = [int]($b.Y + $b.Height/2)
    Write-Output ("clicking the ring switch at " + $x + "," + $y)
    [W]::Click($x, $y)
    Start-Sleep -Milliseconds 1200
    $out = & 'H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe' --ctl status 2>&1
    $t = ($out | Out-String)
    foreach ($k in @('orbit','orbit_clickable','icons_hidden')) {
        if ($t -match ('"' + $k + '":\s*([a-z0-9]+)')) { Write-Output ("engine says " + $k + " = " + $matches[1]) }
    }
}
