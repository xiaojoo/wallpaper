# tools/probe-ring-drag.ps1 - drag the ring's grab layer and check the ring moved by the same amount.
#
# The claim being measured is that one computation feeds everything: the number the drag layer stores,
# the centre the shader draws around, and the rectangles the click boxes sit in have to move together,
# because the moment they do not, a click lands somewhere other than the icon it points at. So this
# drags the layer by a known pixel delta and reads all three back.
#
#   -Dx -Dy     how far to drag, in device pixels
#   -Home       afterwards, put the centre back where the work area says it belongs
#
# Input is SendInput with absolute coordinates - the form that actually starts a drag on this machine
# (mouse_event + SetCursorPos moves the cursor but the drag threshold never fires). The button goes
# down only after the point has been confirmed to belong to our own drag layer.
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [int]$Dx = 320,
    [int]$Dy = -180,
    [switch]$Reset
)

$ErrorActionPreference = 'Stop'
$exe = 'H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe'
$layerTitle = 'SmartWallpaper shortcut ring - drag to move'

$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class D {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct PT { public int X,Y; }
  [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint flags; public uint time; IntPtr extra; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h,out RECT r);
  [DllImport("user32.dll")] static extern int GetSystemMetrics(int n);
  [DllImport("user32.dll")] static extern uint SendInput(uint n, INPUT[] i, int cb);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr FindWindowW(string cls, string win);
  public delegate bool EnumProc(IntPtr h, IntPtr l);

  const uint MOVE=0x1, DOWN=0x2, UP=0x4, ABS=0x8000, VDES=0x4000;

  public static IntPtr Layer(string title) {
    var sb = new StringBuilder(256);
    IntPtr found = IntPtr.Zero;
    EnumWindows((h,l) => {
      GetWindowTextW(h, sb, 256);
      if (sb.ToString() == title && IsWindowVisible(h)) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static string Cls(IntPtr h){ var b=new StringBuilder(128); GetClassNameW(h,b,128); return b.ToString(); }
  public static string Rect(IntPtr h){ RECT r; GetWindowRect(h,out r); return string.Format("{0} {1} {2} {3} {4}x{5}", r.L,r.T,r.R,r.B,r.R-r.L,r.B-r.T); }
  public static string LayerInfo(string title) {
    var h = Layer(title);
    if (h == IntPtr.Zero) return "absent";
    return string.Format("0x{0:X} {1} {2}", (long)h, Cls(h), Rect(h));
  }
  static void MoveTo(int x, int y) {
    int vl=GetSystemMetrics(76), vt=GetSystemMetrics(77), vw=GetSystemMetrics(78), vh=GetSystemMetrics(79);
    var i = new INPUT[1];
    i[0].type = 0;
    i[0].mi.dx = (int)(((long)(x - vl) * 65535) / (vw - 1));
    i[0].mi.dy = (int)(((long)(y - vt) * 65535) / (vh - 1));
    i[0].mi.flags = MOVE | ABS | VDES;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  static void Btn(uint f) {
    var i = new INPUT[1]; i[0].type = 0; i[0].mi.flags = f;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
  }
  public static string Drag(int x1,int y1,int x2,int y2) {
    MoveTo(x1,y1); Thread.Sleep(200);
    Btn(DOWN); Thread.Sleep(320);
    for (int k=1;k<=20;k++) { MoveTo(x1+(x2-x1)*k/20, y1+(y2-y1)*k/20); Thread.Sleep(25); }
    Thread.Sleep(250);
    int cx=0, cy=0;
    PT p; GetCursorPos(out p); cx=p.X; cy=p.Y;
    Btn(UP); Thread.Sleep(400);
    return string.Format("released at {0},{1}", cx, cy);
  }
  [DllImport("user32.dll")] static extern bool GetCursorPos(out PT p);
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Runtime.InteropServices

function Status([string]$key) {
    $raw = & $exe --ctl status 2>&1 | Out-String
    $m = [regex]::Match($raw, ('"' + $key + '":\s*(\[[^\]]*\]|"[^"]*"|[a-z0-9_.-]+)'))
    if ($m.Success) { return $m.Groups[1].Value }
    return '<absent>'
}
function RectsLine {
    $log = Get-ChildItem 'H:\wallpaper\bld\bin\RelWithDebInfo\logs' -Filter 'renderer-*.log' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $l = Select-String -Path $log.FullName -Pattern 'orbit rects hover=' | Select-Object -Last 1
    if (-not $l) { return $null }
    $o = @{}
    foreach ($m in [regex]::Matches($l.Line, ' (\d+):(\d+),(\d+)\+(\d+)')) {
        $o[[int]$m.Groups[1].Value] = @([int]$m.Groups[2].Value, [int]$m.Groups[3].Value)
    }
    return $o
}

Write-Output ("before: orbit=" + (Status 'orbit') + "  orbit_drag=" + (Status 'orbit_drag') +
              "  orbit_pos=" + (Status 'orbit_pos'))
Write-Output ("layer:  " + ([D]::LayerInfo($layerTitle)))

if ($Reset) {
    & $exe --ctl orbithome | Out-Null
    Start-Sleep -Milliseconds 900
    Write-Output ("after orbithome: orbit_pos=" + (Status 'orbit_pos') + "  layer=" + ([D]::LayerInfo($layerTitle)))
    exit 0
}

& $exe --ctl orbitdrag on | Out-Null
Start-Sleep -Milliseconds 900
$info = [D]::LayerInfo($layerTitle)
Write-Output ("armed:  orbit_drag=" + (Status 'orbit_drag') + "  layer=" + $info)
if ($info -eq 'absent') { Write-Output 'VERDICT: the drag layer did not appear'; exit 1 }

$h = [D]::Layer($layerTitle)
$r = [D]::Rect($h)
$nums = [regex]::Matches($r, '-?\d+') | ForEach-Object { [int]$_.Value }
$L = $nums[0]; $T = $nums[1]; $R = $nums[2]; $B = $nums[3]
$cx = ($L + $R) / 2; $cy = ($T + $B) / 2
$before = RectsLine
$slot = (($before.Keys | Sort-Object)[0])
Write-Output ("grabbing the layer centre at " + [int]$cx + "," + [int]$cy + " (slot " + $slot + " at " +
              ($before[$slot] -join ',') + ")")
$what = [D]::Drag([int]$cx, [int]$cy, [int]($cx + $Dx), [int]($cy + $Dy))
Write-Output ("drag: " + $what)
Start-Sleep -Milliseconds 1500

$after = RectsLine
$stored = Status 'orbit_pos'
Write-Output ("after:  orbit_drag=" + (Status 'orbit_drag') + "  orbit_pos=" + $stored + "  layer=" + ([D]::LayerInfo($layerTitle)))
if ($after.ContainsKey($slot)) {
    $dxr = $after[$slot][0] - $before[$slot][0]; $dyr = $after[$slot][1] - $before[$slot][1]
    Write-Output ("slot " + $slot + " moved by " + $dxr + "," + $dyr + "  (asked: " + $Dx + "," + $Dy + ")")
    $ok = ([Math]::Abs($dxr - $Dx) -le 6) -and ([Math]::Abs($dyr - $Dy) -le 6)
    Write-Output ("VERDICT drag: the ring followed the layer within 6 px = " + $(if ($ok) { 'YES' } else { 'NO' }))
} else {
    Write-Output 'VERDICT drag: that slot is no longer visible, cannot compare'
}
