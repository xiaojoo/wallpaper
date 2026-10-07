param([int]$Id = 0)
$sig = @"
using System;
using System.Runtime.InteropServices;
public struct R { public int x,y,w,h; }
public static class W32 {
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
}
"@
Add-Type -TypeDefinition $sig | Out-Null
$p = Get-Process -Id $Id
$h = $p.MainWindowHandle
$sb = New-Object System.Text.StringBuilder 256
[W32]::GetClassName($h, $sb, 256) | Out-Null
$r = New-Object R
[W32]::GetWindowRect($h, [ref]$r) | Out-Null
"pid=$Id handle=0x$($h.ToString('X')) class=$($sb.ToString())"
"IsIconic=$([W32]::IsIconic($h)) IsWindowVisible=$([W32]::IsWindowVisible($h))"
"rect=$($r.x),$($r.y) $($r.w)x$($r.h)"
$fg = [W32]::GetForegroundWindow()
$sb2 = New-Object System.Text.StringBuilder 256
[W32]::GetClassName($fg, $sb2, 256) | Out-Null
"foreground=0x$($fg.ToString('X')) class=$($sb2.ToString())"
