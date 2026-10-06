# Probe 2: locate the icon surface and the true Z-order of Progman. Read-only.
$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class D2
{
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrW(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    static string Cls(IntPtr h) { var s = new StringBuilder(300); GetClassNameW(h, s, 300); return s.ToString(); }
    static string Txt(IntPtr h) { var s = new StringBuilder(300); GetWindowTextW(h, s, 300); return s.ToString(); }

    public static string Line(IntPtr h)
    {
        RECT r; GetWindowRect(h, out r);
        uint pid; GetWindowThreadProcessId(h, out pid);
        long st = (long)GetWindowLongPtrW(h, -16);
        IntPtr par = GetParent(h);
        return string.Format("0x{0:X} parent=0x{1:X} cls='{2}' txt='{3}' vis={4} rect={5},{6},{7}x{8} style=0x{9:X8} pid={10}",
            (long)h, (long)par, Cls(h), Txt(h), IsWindowVisible(h), r.L, r.T, r.R - r.L, r.B - r.T, st & 0xFFFFFFFF, pid);
    }

    public static IntPtr Progman()
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { if (Cls(h) == "Progman") { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }

    public static List<string> Top(int max)
    {
        var list = new List<string>();
        int[] i = { 0 };
        EnumWindows((h, l) => {
            var c = Cls(h);
            if (c == "#32769") return true;
            list.Add(i[0] + " " + Line(h));
            i[0]++;
            if (c == "Progman" || i[0] >= max) return false;
            return true;
        }, IntPtr.Zero);
        return list;
    }

    public static List<string> Direct(IntPtr p)
    {
        var outp = new List<string>();
        IntPtr c = FindWindowExW(p, IntPtr.Zero, null, null);
        int guard = 0;
        while (c != IntPtr.Zero && guard++ < 40) { outp.Add(Line(c)); c = FindWindowExW(p, c, null, null); }
        return outp;
    }

    public static List<string> FindEvery(params string[] classes)
    {
        var outp = new List<string>();
        EnumWindows((h, l) => {
            foreach (var k in classes) if (Cls(h) == k) outp.Add("TOP " + Line(h));
            EnumChildWindows(h, (c, m) => {
                foreach (var k in classes) if (Cls(c) == k) outp.Add("   CHILD " + Line(c));
                return true;
            }, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return outp;
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

$pm = [D2]::Progman()
Write-Output ("PROGMAN = 0x{0:X}" -f [int64]$pm)
Write-Output ("FindWindowW('Progman') = 0x{0:X}" -f [int64]([D2]::FindWindowW('Progman', $null)))
Write-Output "--- Progman direct children ---"
[D2]::Direct($pm) | ForEach-Object { Write-Output $_ }
Write-Output "--- top-level Z order (top down to Progman) ---"
[D2]::Top(40) | ForEach-Object { Write-Output $_ }
Write-Output "--- every SysListView32 / WorkerW / DefView / Xaml ---"
[D2]::FindEvery('SysListView32', 'SHELLDLL_DefView', 'WorkerW', 'DesktopWindowXamlSource') | ForEach-Object { Write-Output $_ }
