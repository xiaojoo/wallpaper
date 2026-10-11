# tools/probe-orbit-click.ps1 - what owns the shortcut ring's pixels, and does a click start a program.
#
# Reads the rectangles the ENGINE logged (orbit rects in the newest renderer log) rather than
# recomputing them, so the probe and the click layer are measuring the same layout. The ring must be
# frozen first:  tools/probe-orbit-click.ps1 -Mode map
#
#   -Mode map       every slot centre, and a point just outside each box, and who owns each
#   -Mode launch    click one slot and check that the fixture shortcut's target actually ran
#   -Mode off       click layer down, re-run the map, so the two tables can be compared
#
# ASCII only: PowerShell 5.1 parses this file under a non-UTF8 codepage.
param(
    [ValidateSet('map','launch')][string]$Mode = 'map',
    [int]$Slot = -1,
    [string]$FixtureLog = 'H:\wallpaper\build\orbit-click.log'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase

$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class Hit
{
    [StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PT p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextLengthW(IntPtr h);

    const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004, GA_ROOT = 2;

    public static void Click(int x, int y)
    {
        SetCursorPos(x, y);
        Thread.Sleep(80);
        mouse_event(LEFTDOWN, 0, 0, 0, IntPtr.Zero);
        Thread.Sleep(60);
        mouse_event(LEFTUP, 0, 0, 0, IntPtr.Zero);
    }

    // Class plus the root window's title: the class alone cannot tell "the desktop" from "an
    // application whose list panel happens to be topmost".
    public static string OwnerAt(int x, int y)
    {
        PT p = new PT(); p.X = x; p.Y = y;
        IntPtr h = WindowFromPoint(p);
        if (h == IntPtr.Zero) return "none";
        var cls = new StringBuilder(300); int n = GetClassNameW(h, cls, 300);
        IntPtr root = GetAncestor(h, GA_ROOT);
        var t = new StringBuilder(300);
        int tn = GetWindowTextW(root, t, 300);
        string title = tn > 0 ? t.ToString(0, tn) : "(no title)";
        return string.Format("{0} root=\"{1}\"", (n > 0 ? cls.ToString(0, n) : "?"), title);
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Runtime.InteropServices

function Get-Rects {
    $dir = 'H:\wallpaper\bld\bin\RelWithDebInfo\logs'
    $log = Get-ChildItem -Path $dir -Filter 'renderer-*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $log) { throw "no renderer log under $dir" }
    $line = Select-String -Path $log.FullName -Pattern 'orbit rects hover=' | Select-Object -Last 1
    if (-not $line) { throw ("no 'orbit rects' line in " + $log.Name + " - is the ring on and clickable?") }
    Write-Output ("source: " + $log.Name + "  " + $line.Line.Substring(0, 23))
    $hover = [regex]::Match($line.Line, 'hover=(-?\d+)').Groups[1].Value
    Write-Output ("engine says hover = " + $hover)
    $out = @()
    foreach ($m in [regex]::Matches($line.Line, ' (\d+):(\d+),(\d+)\+(\d+)')) {
        $out += [pscustomobject]@{
            Slot = [int]$m.Groups[1].Value
            CX   = [int]$m.Groups[2].Value
            CY   = [int]$m.Groups[3].Value
            Half = [int]$m.Groups[4].Value
        }
    }
    , $out
}

$rects = (Get-Rects)[-1]
Write-Output ("slots the engine reports as visible: " + $rects.Count)

if ($Mode -eq 'map') {
    Write-Output ''
    Write-Output 'inside each box (box centre):        owner'
    foreach ($r in $rects) {
        Write-Output ("  slot {0,2} at {1,4},{2,4}  {3}" -f $r.Slot, $r.CX, $r.CY, ([Hit]::OwnerAt($r.CX, $r.CY)))
    }
    Write-Output ''
    Write-Output 'one step outside each box (centre + half + 12):  owner'
    foreach ($r in $rects) {
        $x = $r.CX + $r.Half + 12
        Write-Output ("  slot {0,2} at {1,4},{2,4}  {3}" -f $r.Slot, $x, $r.CY, ([Hit]::OwnerAt($x, $r.CY)))
    }
    $inOurs   = @($rects | Where-Object { ([Hit]::OwnerAt($_.CX, $_.CY)) -like 'SmartWallpaper.IconHit*' }).Count
    $outOurs  = @($rects | Where-Object { ([Hit]::OwnerAt(($_.CX + $_.Half + 12), $_.CY)) -like 'SmartWallpaper.IconHit*' }).Count
    Write-Output ''
    Write-Output ("VERDICT inside : " + $inOurs + " of " + $rects.Count + " box centres owned by the click layer")
    Write-Output ("VERDICT outside: " + $outOurs + " of " + $rects.Count + " points one step out still owned by the click layer (want 0)")
}

if ($Mode -eq 'launch') {
    if ($Slot -lt 0) { throw 'launch wants -Slot N, the slot to click' }
    $r = @($rects | Where-Object { $_.Slot -eq $Slot })
    if ($r.Count -ne 1) { throw ("slot $Slot is not among the visible ones: " + (($rects | ForEach-Object Slot) -join ',')) }
    $owner = [Hit]::OwnerAt($r[0].CX, $r[0].CY)
    Write-Output ("target slot $Slot at $($r[0].CX),$($r[0].CY) owned by: $owner")
    # The rule this machine already paid for: never synthesize a click before proving the window at
    # that point is the one under test - a cloaked or overlapping window otherwise takes the click.
    if ($owner -notlike 'SmartWallpaper.IconHit*') {
        Write-Output 'VERDICT launch: REFUSED - the click layer does not own that point'
        return
    }
    $before = ( @( Get-Content -LiteralPath $FixtureLog -ErrorAction SilentlyContinue ) ).Count
    [Hit]::Click($r[0].CX, $r[0].CY)
    Start-Sleep -Milliseconds 2500
    $after = @( Get-Content -LiteralPath $FixtureLog -ErrorAction SilentlyContinue )
    Write-Output ("fixture target log lines: before=$before after=" + $after.Count)
    $dir = 'H:\wallpaper\bld\bin\RelWithDebInfo\logs'
    $log = Get-ChildItem -Path $dir -Filter 'renderer-*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $cl = Select-String -Path $log.FullName -Pattern 'orbit click' | Select-Object -Last 2
    foreach ($c in $cl) { Write-Output ("renderer: " + $c.Line) }
    if ($after.Count -gt $before) { Write-Output 'VERDICT launch: THE CLICK STARTED THE PROGRAM' }
    else { Write-Output 'VERDICT launch: NOTHING STARTED - the hit test agreed but no launch happened' }
}
