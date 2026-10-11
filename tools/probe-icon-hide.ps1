# Stage 0 probe (a): can we hide the real desktop icons, and who owns the click while they are hidden?
#
# The repo's own note BACKLOG.md:429-430 says "fade the background but keep the icons crisp" is not
# achievable from outside explorer - but that is about FADING. HIDING is a different operation and has
# never been measured on this machine, so nothing here is assumed.
#
# Three things are wanted:
#   1. does ShowWindow(SW_HIDE) on the icon list view actually remove the icons from the screen -
#      measured in pixels off a real desktop capture, not by trusting the API's return value;
#   2. does SW_SHOW bring back exactly the same count;
#   3. while hidden, who owns a desktop point? This one decides the design: our wallpaper window is a
#      Progman child inserted below SHELLDLL_DefView, so if hiding the list view hands the click to
#      us, the whole separate hit-window layer becomes unnecessary.
#
# Nothing is synthesised: no clicks, no keystrokes, no right-click menu. Those need the user's own
# hand and are listed as the manual step at the end.
#
# The restore is in a finally block and is also verified after it runs.

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public class IconHide {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string a, string b);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string win);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PPOINT pt);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public static System.Collections.Generic.List<IntPtr> Tops = new System.Collections.Generic.List<IntPtr>();
  static EnumProc g_keep = Keep;
  static bool Keep(IntPtr h, IntPtr l) { Tops.Add(h); return true; }
  public static void WalkTop() { Tops.Clear(); EnumWindows(g_keep, IntPtr.Zero); }
  // FindWindowExW only walks DIRECT children, and SHELLDLL_DefView is not a direct child of Progman
  // on this build - EnumChildWindows is recursive, which is why the standalone tree walk saw it and
  // the FindWindowExW descent returned 0.
  static EnumProc g_kids = Kids;
  static bool Kids(IntPtr h, IntPtr l) { Kids_.Add(h); return true; }
  static System.Collections.Generic.List<IntPtr> Kids_ = new System.Collections.Generic.List<IntPtr>();
  public static IntPtr ListViewUnder(IntPtr root) {
    Kids_.Clear(); EnumChildWindows(root, g_kids, IntPtr.Zero);
    foreach (var h in Kids_) if (ClassOf(h) == "SysListView32") return h;
    return IntPtr.Zero;
  }
  public static string ClassOf(IntPtr h) { var sb=new StringBuilder(256); GetClassNameW(h,sb,256); return sb.ToString(); }
  // Progman -> SHELLDLL_DefView -> SysListView32. Found this way because FindWindowW("Progman")
  // returns 0 on build 26300 (Engine/Desktop/WorkerW.cpp:69-82) and a point query is unreliable while
  // the user has windows open over the desktop.
  public static string LastDiag = "";
  public static IntPtr ListView() {
    WalkTop();
    int prog = 0; IntPtr progH = IntPtr.Zero;
    foreach (var h in Tops) { if (ClassOf(h) == "Progman") { prog++; progH = h; } }
    LastDiag = "tops=" + Tops.Count + "  Progman matches=" + prog + " hwnd=" + progH;
    foreach (var h in Tops) {
      if (ClassOf(h) != "Progman") continue;
      var lv = ListViewUnder(h);
      if (lv != IntPtr.Zero) return lv;
    }
    return IntPtr.Zero;
  }

  [StructLayout(LayoutKind.Sequential)] public struct PPOINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public const int SW_HIDE = 0, SW_SHOW = 5, GW_CHILD = 5, GW_HWNDNEXT = 2;
  public static string Ancestor(IntPtr h, uint f) { return NameOf(GetAncestor(h, f)); }

  public static string NameOf(IntPtr h) {
    if (h == IntPtr.Zero) return "<none>";
    var sb = new StringBuilder(256); GetClassNameW(h, sb, 256);
    RECT r; GetWindowRect(h, out r);
    return sb.ToString() + " @" + r.L + "," + r.T + "-" + r.R + "," + r.B;
  }
  public static string At(int x, int y) {
    var p = new PPOINT { X = x, Y = y };
    return NameOf(WindowFromPoint(p));
  }
  public static string ChildOf(IntPtr parent, string cls) {
    return NameOf(FindWindowExW(parent, IntPtr.Zero, cls, null));
  }
}
"@

$list = [IconHide]::ListView()
$list = [IconHide]::ListView()
Write-Output ('diag: ' + [IconHide]::LastDiag)
if ($list -eq [IntPtr]::Zero) { Write-Output 'ABORT: chain not found, nothing was touched'; exit 1 }
Write-Output ("listview before: " + [IconHide]::NameOf($list) + "  visible=" + [IconHide]::IsWindowVisible($list))

# A region that holds icons and is clear of the taskbar. Top-left column at 4K.
$rx = 0; $ry = 0; $rw = 300; $rh = 700
function Shot($name) {
    $out = "tools/tmp/icons_" + $name + ".png"
    & powershell -NoProfile -ExecutionPolicy Bypass -File tools/capture-desktop.ps1 -Out $out -X $rx -Y $ry -W $rw -H $rh | Out-Null
    return $out
}

$before = Shot 'before'
try {
    [void][IconHide]::ShowWindow($list, 0)   # SW_HIDE
    Start-Sleep -Milliseconds 700
    $hidden = Shot 'hidden'
    Write-Output ("listview hidden: visible=" + [IconHide]::IsWindowVisible($list))
    Write-Output ("WHO OWNS A DESKTOP POINT WHILE HIDDEN (150,300) -> " + [IconHide]::At(150, 300))
    Write-Output ("                                        (150,120) -> " + [IconHide]::At(150, 120))
} finally {
    [void][IconHide]::ShowWindow($list, 5)   # SW_SHOW, always
    Start-Sleep -Milliseconds 700
}
$after = Shot 'restored'
Write-Output ("listview restored: visible=" + [IconHide]::IsWindowVisible($list))
Write-Output ("point owner after restore -> " + [IconHide]::At(150, 300))

Add-Type -AssemblyName System.Drawing
function Stats($f) {
    $bmp = New-Object System.Drawing.Bitmap $f
    $lit = 0; $sum = 0L
    for ($y = 0; $y -lt $bmp.Height; $y += 2) {
        for ($x = 0; $x -lt $bmp.Width; $x += 2) {
            $c = $bmp.GetPixel($x, $y)
            $l = ([int]$c.R + [int]$c.G + [int]$c.B) / 3
            $sum += $l
            if ($l -gt 90) { $lit++ }
        }
    }
    $n = [int]($bmp.Height / 2) * [int]($bmp.Width / 2)
    $bmp.Dispose()
    return @($lit, $n, [math]::Round($sum / $n, 1))
}
$sb = Stats $before; $sh = Stats $hidden; $sa = Stats $after
Write-Output ("icon region, lit px (>90) of " + $sb[1] + " sampled: before=" + $sb[0] + "  hidden=" + $sh[0] + "  restored=" + $sa[0])
Write-Output ("mean luminance: before=" + $sb[2] + "  hidden=" + $sh[2] + "  restored=" + $sa[2])
Write-Output 'MANUAL STEP STILL NEEDED: right-click menu, rubber-band select and F5 while hidden cannot'
Write-Output 'be measured without synthesising input into the shell, which this probe deliberately does not do.'
