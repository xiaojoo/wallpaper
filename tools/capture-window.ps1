# Captures one window's own pixels with PrintWindow, without touching focus or z-order.
# BLIND TO POPUPS: PrintWindow renders the window's backing store, and a Qt Quick Controls Popup
# (tooltips, menus, any Hint) lives in the overlay layer that PrintWindow does not composite - the
# shot comes back with the control hovered and its bubble simply absent. Proved twice on 2026-10-07
# and then contradicted by tools/capture-desktop.ps1 over the same pixels. When the thing under test
# is a popup, capture the screen region instead; use this only for the window's own content.
param([string]$Out = 'window.png', [string]$Class = 'Qt6112QWindowIcon', [string]$Title = '')
$src = @'
using System;
using System.Text;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class PW
{
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, int flags);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDC(IntPtr h);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public static string Run(string cls, string title, string outPath)
    {
        IntPtr found = IntPtr.Zero;
        long best = 0;
        // Qt registers several windows under the same class (tooltips included); take the biggest.
        EnumWindows((h, l) => {
            var sb = new StringBuilder(300);
            GetClassNameW(h, sb, 300);
            if (sb.ToString() != cls || !IsWindowVisible(h)) return true;
            // Qt Creator is the same window class as us and bigger, so a title filter is required.
            if (title.Length > 0) {
                var tb = new StringBuilder(300);
                GetWindowTextW(h, tb, 300);
                if (tb.ToString().IndexOf(title, StringComparison.OrdinalIgnoreCase) < 0) return true;
            }
            RECT rr; GetWindowRect(h, out rr);
            long area = (long)(rr.R - rr.L) * (rr.B - rr.T);
            if (area > best) { best = area; found = h; }
            return true;
        }, IntPtr.Zero);
        if (found == IntPtr.Zero) return "no window with class " + cls + (title.Length > 0 ? " and title " + title : "");

        RECT r; GetWindowRect(found, out r);
        int w = r.R - r.L, hh = r.B - r.T;
        if (w < 10 || hh < 10) return "window too small";

        using (Bitmap bmp = new Bitmap(w, hh, PixelFormat.Format32bppArgb))
        {
            using (Graphics g = Graphics.FromImage(bmp))
            {
                IntPtr dc = g.GetHdc();
                bool ok = PrintWindow(found, dc, 2 /* PW_RENDERFULLCONTENT */);
                g.ReleaseHdc(dc);
                if (!ok) return "PrintWindow failed";
            }
            bmp.Save(outPath, ImageFormat.Png);
        }
        return "printed " + w + "x" + hh + " -> " + outPath;
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing
$full = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { (Join-Path (Get-Location) $Out).ToString() }
Write-Output ([PW]::Run($Class, $Title, $full))
