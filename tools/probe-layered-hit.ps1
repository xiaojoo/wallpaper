# Stage 0 probe (b): does a per-pixel-alpha layered window pass the mouse through where alpha is 0?
#
# This is the load-bearing unknown for clickable orbiting shortcuts. The wallpaper surface is a child
# of Progman inserted below SHELLDLL_DefView, so it owns no desktop click at all (README.md:997
# measured 53/53 icon points belonging to SysListView32). The plan is to keep drawing on that surface
# and put a topmost layered window on top purely as the hit layer - but that only works if the window
# stops the mouse ONLY where it is opaque. If it also swallows the alpha=0 pixels, the whole desktop
# would stop answering clicks and the feature is dead.
#
# The repo already has half the answer in prose: Engine/Desktop/HudDragWindow.hpp:2-14 says a colour
# KEY makes keyed pixels transparent to the mouse, which is why that window uses per-pixel alpha with
# an interior of alpha 1 so the whole card is grabbable. That is the same coin seen from the other
# side, and it predicts alpha=0 passes through. Prediction is not measurement, so this probes it.
#
# What it does: puts a 420x220 topmost layered window over the desktop, left half opaque red, right
# half alpha 0, and asks WindowFromPoint who owns a pixel in each half. Baseline first, with no
# window up, so "the layer below" is a known answer rather than an assumption.
#
# It creates and destroys its own window only. Nothing else on the desktop is touched.

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public class HitProbe {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr CreateWindowExW(uint ex, string cls, string title, uint style, int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
  [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll", SetLastError=true)] public static extern bool UpdateLayeredWindow(IntPtr h, IntPtr hdc, ref PPOINT ppt, ref PSize sz, IntPtr hdcSrc, ref PPOINT srcp, uint key, ref BLENDFUNCTION bf, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PPOINT pt);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr dc);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateDIBSection(IntPtr dc, ref BITMAPINFO bi, uint usage, out IntPtr bits, IntPtr sec, uint off);
  [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
  [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
  [DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandle(string n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern ushort RegisterClassW(ref WNDCLASS wc);
  [DllImport("user32.dll")] public static extern IntPtr DefWindowProcW(IntPtr h, uint msg, IntPtr w, IntPtr l);

  [StructLayout(LayoutKind.Sequential)] public struct PPOINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct PSize { public int cx, cy; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct BLENDFUNCTION { public byte BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat; }
  [StructLayout(LayoutKind.Sequential)] public struct RGBQUAD { public byte b,g,r,a; }
  [StructLayout(LayoutKind.Sequential)] public struct BITMAPINFO { public int size; public int width; public int height; public short planes; public short bitCount; public int compression; public int sizeImage; public int xppm; public int yppm; public int clrUsed; public int clrImportant; }
  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct WNDCLASS { public uint style; public IntPtr lpfnWndProc; public int cbClsExtra; public int cbWndExtra; public IntPtr hInstance; public IntPtr hIcon; public IntPtr hCursor; public IntPtr hbrBackground; public string lpszMenuName; public string lpszClassName; }

  public const uint WS_POPUP = 0x80000000;
  public const uint WS_EX_LAYERED = 0x00080000, WS_EX_TOOLWINDOW = 0x00000080, WS_EX_TOPMOST = 0x00000008;
  public const int SW_SHOWNOACTIVATE = 4;
  public const uint ULW_ALPHA = 2;
  public const byte AC_SRC_ALPHA = 1, BS_BLEND = 2;

  public delegate IntPtr WndProcDel(IntPtr h, uint msg, IntPtr w, IntPtr l);
  static WndProcDel g_proc = WndProc;
  static IntPtr WndProc(IntPtr h, uint msg, IntPtr w, IntPtr l) { return DefWindowProcW(h, msg, w, l); }

  public static string NameOf(IntPtr h) {
    if (h == IntPtr.Zero) return "<none>";
    var sb = new StringBuilder(256);
    GetClassNameW(h, sb, 256);
    RECT r; GetWindowRect(h, out r);
    return sb.ToString() + " @" + r.L + "," + r.T + "-" + r.R + "," + r.B;
  }

  public static string ProbeAt(int x, int y) {
    var p = new PPOINT { X = x, Y = y };
    return NameOf(WindowFromPoint(p));
  }

  static IntPtr g_dib; static IntPtr g_mem; static IntPtr g_bits;

  // alpha 255 on the left half, 0 on the right. Premultiplied BGRA, so the transparent half must be
  // all zero, not just zero alpha.
  public static void Make(int w, int h, IntPtr screenDc) {
    var bi = new BITMAPINFO();
    bi.size = Marshal.SizeOf(typeof(BITMAPINFO));
    bi.width = w; bi.height = -h; bi.planes = 1; bi.bitCount = 32; bi.compression = 0;
    g_mem = CreateCompatibleDC(screenDc);
    g_dib = CreateDIBSection(screenDc, ref bi, 0, out g_bits, IntPtr.Zero, 0);
    SelectObject(g_mem, g_dib);
    int stride = w * 4;
    byte[] buf = new byte[stride * h];
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        int o = y * stride + x * 4;
        bool opaque = x < w / 2;
        buf[o + 0] = (byte)(opaque ? 0   : 0);   // B
        buf[o + 1] = (byte)(opaque ? 0   : 0);   // G
        buf[o + 2] = (byte)(opaque ? 255 : 0);   // R
        buf[o + 3] = (byte)(opaque ? 255 : 0);   // A
      }
    }
    Marshal.Copy(buf, 0, g_bits, buf.Length);
  }

  public static IntPtr Show(int x, int y, int w, int h) {
    var dc = GetDC(IntPtr.Zero);
    Make(w, h, dc);
    ReleaseDC(IntPtr.Zero, dc);
    var wc = new WNDCLASS();
    wc.lpfnWndProc = Marshal.GetFunctionPointerForDelegate(g_proc);
    wc.hInstance = GetModuleHandle(null);
    wc.lpszClassName = "SmartWallpaper.HitProbe";
    ushort atom = RegisterClassW(ref wc);
    if (atom == 0) throw new Exception("RegisterClassW failed, win32=" + Marshal.GetLastWin32Error());
    IntPtr hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        "SmartWallpaper.HitProbe", "hitprobe", WS_POPUP, x, y, w, h, IntPtr.Zero, IntPtr.Zero, wc.hInstance, IntPtr.Zero);
    if (hwnd == IntPtr.Zero) throw new Exception("CreateWindowEx failed, win32=" + Marshal.GetLastWin32Error());
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    var pt = new PPOINT { X = x, Y = y };
    var sz = new PSize { cx = w, cy = h };
    var sp = new PPOINT { X = 0, Y = 0 };
    var bf = new BLENDFUNCTION { BlendOp = BS_BLEND, BlendFlags = 0, SourceConstantAlpha = 255, AlphaFormat = AC_SRC_ALPHA };
    bool ok = UpdateLayeredWindow(hwnd, dc0(), ref pt, ref sz, g_mem, ref sp, 0, ref bf, ULW_ALPHA);
    if (!ok) throw new Exception("UpdateLayeredWindow failed, win32=" + Marshal.GetLastWin32Error() + " hwnd=" + hwnd);
    return hwnd;
  }

  static IntPtr dc0() { var d = GetDC(IntPtr.Zero); return d; }

  public static void Clean(IntPtr h) {
    if (h != IntPtr.Zero) DestroyWindow(h);
    if (g_dib != IntPtr.Zero) DeleteObject(g_dib);
    if (g_mem != IntPtr.Zero) DeleteDC(g_mem);
  }
}
"@

# The probe point has to be somewhere the desktop is actually visible. A browser window opened over
# the first candidate between runs and the baseline read Chrome_RenderWidgetHostHWND - measuring that
# would have answered nothing. So walk candidates and take the first whose baseline owner is the
# desktop layer itself.
$w = 420; $h = 220
$desktopOwners = @('SysListView32','Progman','WorkerW','SHELLDLL_DefView')
$cands = @()
foreach ($yy in 620,420,820,300,1020) { foreach ($xx in 900,1500,2100,600,2600) { $cands += ,@($xx,$yy) } }
$x = $null; $y = $null; $base = ''
foreach ($c in $cands) {
    $cx0 = [int]$c[0]; $cy0 = [int]$c[1]
    $a = [HitProbe]::ProbeAt([int]($cx0 + $w / 4), [int]($cy0 + $h / 2))
    $b = [HitProbe]::ProbeAt([int]($cx0 + 3 * $w / 4), [int]($cy0 + $h / 2))
    if (($a + '|' + $b) -match '^(\w+) @') { }
    $clsA = ($a -split ' ')[0]; $clsB = ($b -split ' ')[0]
    if ($desktopOwners -contains $clsA -and $desktopOwners -contains $clsB) {
        $x = $cx0; $y = $cy0; $base = $clsA; break
    }
    Write-Output ("  candidate " + $cx0 + "," + $cy0 + " rejected: baseline owner " + $clsA + " / " + $clsB)
}
if ($null -eq $x) { Write-Output 'NO candidate point belongs to the desktop - aborting rather than measuring a third-party window'; exit 1 }
$ox = [int]($x + $w / 4); $bx = [int]($x + 3 * $w / 4); $cy = [int]($y + $h / 2)
Write-Output ("using probe origin " + $x + "," + $y + "  baseline owner = " + $base)

Write-Output ("baseline, no probe window up:")
Write-Output ("  opaque-half point (" + $ox + "," + $cy + ") -> " + [HitProbe]::ProbeAt($ox, $cy))
Write-Output ("  alpha0-half point (" + $bx + "," + $cy + ") -> " + [HitProbe]::ProbeAt($bx, $cy))

$hwnd = [HitProbe]::Show($x, $y, $w, $h)
Start-Sleep -Milliseconds 400
try {
    Write-Output ("with the layered window up (left half opaque red, right half alpha 0):")
    $a = [HitProbe]::ProbeAt($ox, $cy)
    $b = [HitProbe]::ProbeAt($bx, $cy)
    Write-Output ("  opaque point -> " + $a)
    Write-Output ("  alpha0 point -> " + $b)
    if ($a -like '*HitProbe*') { Write-Output '  VERDICT: opaque pixels are grabbed by us (expected)' } else { Write-Output '  VERDICT: UNEXPECTED - we did not even take the opaque half' }
    if ($b -like '*HitProbe*') { Write-Output '  VERDICT: FAIL - alpha=0 still steals the click, so a persistent hit window would eat the desktop' } else { Write-Output '  VERDICT: PASS - alpha=0 passes the mouse through to the layer below' }
} finally {
    [HitProbe]::Clean($hwnd)
    Start-Sleep -Milliseconds 200
    Write-Output ("after destroy, opaque point -> " + [HitProbe]::ProbeAt($ox, $cy))
}
