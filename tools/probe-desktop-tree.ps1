# Read-only: find the desktop icon list view by walking the window tree, not by asking a point.
#
# Two routes already failed on this machine and both failures are informative:
#   - FindWindowW("Progman") returns 0 on build 26300 (a fact the repo already records in
#     Engine/Desktop/WorkerW.cpp:69-82, which is why that code goes through GetShellWindow and
#     EnumWindows instead);
#   - WindowFromPoint over the icon area now answers Microsoft.UI.Content.DesktopChildSiteBridge,
#     because a real application window is sitting on the desktop right now.
# So enumerate every top-level window, name it, and descend Progman -> SHELLDLL_DefView ->
# SysListView32. Nothing is shown, hidden or moved.

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public class Tree {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr after, string cls, string win);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);

  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static List<string> Top = new List<string>();

  public static string Desc(IntPtr h) {
    var sb = new StringBuilder(256); GetClassNameW(h, sb, 256);
    RECT r; GetWindowRect(h, out r);
    return sb.ToString() + " vis=" + (IsWindowVisible(h) ? "1" : "0") + " @" + r.L + "," + r.T + "-" + r.R + "," + r.B + " hwnd=" + h;
  }

  public static void WalkTop() {
    Top.Clear();
    EnumWindows((h, l) => { Top.Add(Desc(h)); return true; }, IntPtr.Zero);
  }
  public static void WalkChildren(IntPtr p) {
    EnumChildWindows(p, (h, l) => { Top.Add("    child " + Desc(h)); return true; }, IntPtr.Zero);
  }
}
"@

[Tree]::WalkTop()
Write-Output ("top-level windows: " + [Tree]::Top.Count)
foreach ($t in [Tree]::Top) {
    if ($t -match 'Progman|WorkerW|Shell_TrayWnd|Program Manager') { Write-Output ("  " + $t) }
}

$prog = [IntPtr]::Zero
foreach ($h in @([Tree]::Top)) { }
# re-enumerate keeping handles so we can descend
$handles = New-Object System.Collections.ArrayList
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class T2 {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static System.Collections.Generic.List<IntPtr> All = new System.Collections.Generic.List<IntPtr>();
  public static void Go() { All.Clear(); EnumWindows((h,l)=>{ All.Add(h); return true; }, IntPtr.Zero); }
}
"@
[T2]::Go()
foreach ($h in [T2]::All) {
    $d = [Tree]::Desc($h)
    if ($d -like 'Progman*') {
        $prog = $h
        Write-Output ("Progman found: " + $d)
        [Tree]::WalkChildren($h)
        break
    }
}
if ($prog -eq [IntPtr]::Zero) { Write-Output 'no top-level window of class Progman on this machine' }

Write-Output '--- children of Progman (if any):'
foreach ($t in [Tree]::Top) { if ($t -like '    child *') { Write-Output $t } }

$dv = [Tree]::FindWindowExW($prog, [IntPtr]::Zero, 'SHELLDLL_DefView', $null)
Write-Output ("DefView under Progman: " + $(if ($dv -eq [IntPtr]::Zero) { '<none>' } else { [Tree]::Desc($dv) }))
$lv = [Tree]::FindWindowExW($dv, [IntPtr]::Zero, 'SysListView32', $null)
Write-Output ("SysListView32 under DefView: " + $(if ($lv -eq [IntPtr]::Zero) { '<none>' } else { [Tree]::Desc($lv) }))
if ($lv -ne [IntPtr]::Zero) { Write-Output ("  its parent is " + [Tree]::Desc([Tree]::GetParent($lv))) }
