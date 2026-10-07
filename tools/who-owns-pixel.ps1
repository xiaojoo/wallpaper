# tools/who-owns-pixel.ps1 - asks the window manager which HWND owns a screen point, and lists
# Progman's / WorkerW's children in z-order. Answers "is the desktop patch I captured actually our
# wallpaper surface, or is it another window / the system picture painted above it".
param([int]$PX = 1650, [int]$PY = 1820)

$src = @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class Own
{
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);

    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }

    public static void Run(int px, int py)
    {
        POINT pt = new POINT(); pt.x = px; pt.y = py;
        IntPtr w = WindowFromPoint(pt);
        Report("WindowFromPoint", w);
        Report("root owner", GetAncestor(w, 2u));   // GA_ROOTOWNER
        Report("parent", GetParent(w));
    }

    static void Report(string label, IntPtr h)
    {
        if (h == IntPtr.Zero) { Console.WriteLine(label + ": 0"); return; }
        StringBuilder sb = new StringBuilder(256);
        GetClassName(h, sb, 256);
        RECT r; GetWindowRect(h, out r);
        Console.WriteLine(string.Format("{0}: 0x{1:X} class='{2}' rect=({3},{4})-({5},{6}) visible={7}",
            label, (long)h, sb, r.l, r.t, r.r, r.b, IsWindowVisible(h)));
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp
[Own]::Run($PX, $PY)
