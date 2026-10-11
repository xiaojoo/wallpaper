# tools/probe-orbit-hover.ps1 - does the ring hold still for a pointer that is on another program?
#
# The ring pauses under the pointer, and until 2026-10-11 that pause was decided by comparing the
# cursor against the icon rectangles the engine made last frame - geometry only, no z-order. So a
# window sitting over the wallpaper stopped the ring while the pointer never touched it.
#
# This measures the two cases that must now differ, which is what makes it a test rather than a
# reading: the same icon box with nothing over it, and another box covered by a real window. Under the
# old geometry-only rule both answered "hovered"; the fix can only pass if they disagree.
#
# The cursor is saved and put back. -Cover is the screen rect of a window to use as the cover; pass
# 'auto' to take the settings window's own rectangle.
#
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$Bin = 'H:\wallpaper\bld\bin\RelWithDebInfo',
    [string]$Cover = 'auto'
)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class H {
  [StructLayout(LayoutKind.Sequential)] public struct P { public int x, y; }
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out P p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  public static R Cover(string cls, string title) {
    R hit = new R(); bool found = false;
    EnumWindows((h, l) => {
      if (found || !IsWindowVisible(h)) return true;
      var c = new StringBuilder(256); GetClassNameW(h, c, 256);
      var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
      if (c.ToString().StartsWith(cls) && t.ToString() == title) { GetWindowRect(h, out hit); found = true; return false; }
      return true;
    }, IntPtr.Zero);
    if (!found) throw new Exception("no visible window " + cls + " / " + title);
    return hit;
  }
}
'@ -Language CSharp

$exe = Join-Path $Bin 'WallpaperRenderer.exe'
$log = @(Get-ChildItem (Join-Path $Bin 'logs\renderer-*.log') | Sort-Object LastWriteTime -Descending |
         Select-Object -First 1)
$line = $null
foreach ($l in Get-Content $log.FullName) { if ($l -match 'orbit rects') { $line = $l } }
if (-not $line) { throw ('no orbit rects line in ' + $log.Name) }
$boxes = @([regex]::Matches($line, '(\d+):(\d+),(\d+)\+(\d+)') | ForEach-Object {
    [pscustomobject]@{ slot = [int]$_.Groups[1].Value; x = [int]$_.Groups[2].Value;
                       y = [int]$_.Groups[3].Value; half = [int]$_.Groups[4].Value } })

if ($Cover -eq 'auto') {
    $c = [H]::Cover('Qt6112QWindowIcon', 'Wallpaper')
    $coverText = ('{0},{1}-{2},{3} (settings window)' -f $c.l, $c.t, $c.r, $c.b)
} else {
    $n = $Cover.Split(',').Trim('[', ']')
    $c = New-Object H+R; $c.l = [int]$n[0]; $c.t = [int]$n[1]; $c.r = [int]$n[2]; $c.b = [int]$n[3]
    $coverText = $Cover
}
Write-Output ("boxes from {0}: {1}" -f $log.Name, $boxes.Count)
Write-Output ("cover rect {0}" -f $coverText)

# The verb's pipe reply is only "queued" - the real answer goes to the log - so the hover is read out
# of the engine's own once-a-second `orbit rects hover=N` line rather than asked for. That line is only
# written while the click layer is armed on the pointer's monitor, which is the case being measured.
function HoverAt([int]$x, [int]$y) {
    [void][H]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 1400
    $all = @(Get-Content $log.FullName)
    for ($i = $all.Count - 1; $i -ge 0; $i--) {
        if ($all[$i] -match 'orbit rects hover=(-?\d+)') { return [int]$Matches[1] }
    }
    return -99
}

$orig = New-Object H+P
[void][H]::GetCursorPos([ref]$orig)
try {
    $free = $boxes | Where-Object { $_.x -lt $c.l -or $_.x -gt $c.r -or $_.y -lt $c.t -or $_.y -gt $c.b } |
            Select-Object -First 1
    $cov  = $boxes | Where-Object { $_.x -gt $c.l -and $_.x -lt $c.r -and $_.y -gt $c.t -and $_.y -lt $c.b } |
            Select-Object -First 1
    if (-not $free -or -not $cov) { throw ('need one covered and one uncovered box; got ' +
                                           "$($null -ne $free) / $($null -ne $cov)") }
    $hf = HoverAt $free.x $free.y
    $hc = HoverAt $cov.x $cov.y
    Write-Output ("uncovered slot {0,-2} at {1},{2}  hover {3}   (want >= 0: the ring holds still)" -f `
                  $free.slot, $free.x, $free.y, $hf)
    Write-Output ("covered   slot {0,-2} at {1},{2}  hover {3}   (want -1: another window owns the pixel)" -f `
                  $cov.slot, $cov.x, $cov.y, $hc)
    if ($hf -ge 0 -and $hc -eq -1) { Write-Output 'VERDICT: pass' } else { Write-Output 'VERDICT: FAIL' }
} finally {
    [void][H]::SetCursorPos($orig.x, $orig.y)
    Write-Output ("cursor put back at {0},{1}" -f $orig.x, $orig.y)
}
