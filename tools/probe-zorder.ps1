# Where does our wallpaper window actually sit? Lists Progman's child chain in Z-order.
$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class Z
{
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrW(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    static string Cls(IntPtr h) { var s = new StringBuilder(512); int n = GetClassNameW(h, s, 512); return n == 0 ? "" : s.ToString(0, n); }
    static string Txt(IntPtr h) { var s = new StringBuilder(512); int n = GetWindowTextW(h, s, 512); return n == 0 ? "" : s.ToString(0, n); }

    public static string Line(IntPtr h)
    {
        RECT r; GetWindowRect(h, out r);
        uint pid; GetWindowThreadProcessId(h, out pid);
        long st = (long)GetWindowLongPtrW(h, -16);
        bool vis = IsWindowVisible(h);
        return string.Format("0x{0:X} cls='{1}' txt='{2}' vis={3} iconic={4} rect={5},{6},{7}x{8} style=0x{9:X8} pid={10}",
            (long)h, Cls(h), Txt(h), vis, IsIconic(h), r.L, r.T, r.R - r.L, r.B - r.T, st & 0xFFFFFFFF, pid);
    }

    public static string ClsOf(IntPtr h) { return Cls(h); }

    public static List<string> Children(IntPtr p)
    {
        var o = new List<string>();
        int[] i = { 0 };
        EnumChildWindows(p, (h, l) => { o.Add(i[0] + " " + Line(h)); i[0]++; return true; }, IntPtr.Zero);
        return o;
    }

    public static List<string> Top()
    {
        var o = new List<string>();
        int[] i = { 0 };
        EnumWindows((h, l) => { o.Add(i[0] + " " + Line(h)); i[0]++; return i[0] < 12; }, IntPtr.Zero);
        return o;
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

$pm = [Z]::GetShellWindow()
Write-Output ("Progman = " + [Z]::Line($pm))
Write-Output "--- Progman child chain (Z-order, top first) ---"
[Z]::Children($pm) | ForEach-Object { Write-Output $_ }
Write-Output "--- top 12 top-level windows ---"
[Z]::Top() | ForEach-Object { Write-Output $_ }
