# Probe 3: correct Unicode marshalling. Locate Progman, its children, icon surface, Z-order. Read-only.
$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class D3
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string n);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrW(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    static string Cls(IntPtr h) { var s = new StringBuilder(512); int n = GetClassNameW(h, s, 512); return n == 0 ? "" : s.ToString(0, n); }
    static string Txt(IntPtr h) { var s = new StringBuilder(512); int n = GetWindowTextW(h, s, 512); return n == 0 ? "" : s.ToString(0, n); }

    public static string Line(IntPtr h)
    {
        RECT r; GetWindowRect(h, out r);
        uint pid; GetWindowThreadProcessId(h, out pid);
        long st = (long)GetWindowLongPtrW(h, -16);
        long ex = (long)GetWindowLongPtrW(h, -20);
        return string.Format("0x{0:X} parent=0x{1:X} cls='{2}' txt='{3}' vis={4} rect={5},{6},{7}x{8} style=0x{9:X8} ex=0x{10:X8} pid={11}",
            (long)h, (long)GetParent(h), Cls(h), Txt(h), IsWindowVisible(h),
            r.L, r.T, r.R - r.L, r.B - r.T, st & 0xFFFFFFFF, ex & 0xFFFFFFFF, pid);
    }
    public static string ClsOf(IntPtr h) { return Cls(h); }

    public static List<string> Direct(IntPtr p)
    {
        var o = new List<string>();
        IntPtr c = FindWindowExW(p, IntPtr.Zero, null, null);
        int g = 0;
        while (c != IntPtr.Zero && g++ < 40) { o.Add(Line(c)); c = FindWindowExW(p, c, null, null); }
        return o;
    }

    public static List<string> TopZ(int max)
    {
        var o = new List<string>();
        int[] i = { 0 };
        EnumWindows((h, l) => {
            o.Add(i[0] + " " + Line(h));
            i[0]++;
            return i[0] < max;
        }, IntPtr.Zero);
        return o;
    }

    public static List<string> Search(params string[] classes)
    {
        var o = new List<string>();
        EnumWindows((h, l) => {
            foreach (var k in classes) if (Cls(h) == k) o.Add("TOP   " + Line(h));
            EnumChildWindows(h, (c, m) => {
                foreach (var k in classes) if (Cls(c) == k) o.Add("  KID " + Line(c));
                return true;
            }, IntPtr.Zero);
            return true;
        }, IntPtr.Zero);
        return o;
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

$pm = [D3]::FindWindowW('Progman', $null)
Write-Output ("FindWindowW Progman = 0x{0:X}" -f [int64]$pm)
if ($pm -eq [IntPtr]::Zero) { Write-Output "!! Progman NOT found by FindWindowW" }
else {
  Write-Output "--- Progman itself ---"
  Write-Output ([D3]::Line($pm))
  Write-Output "--- Progman direct children ---"
  [D3]::Direct($pm) | ForEach-Object { Write-Output $_ }
}
Write-Output "--- top-level Z order (first 25) ---"
[D3]::TopZ(25) | ForEach-Object { Write-Output $_ }
Write-Output "--- search icon surfaces ---"
[D3]::Search('SysListView32', 'SHELLDLL_DefView', 'WorkerW', 'Progman', 'DesktopWindowXamlSource', 'Windows.UI.Core.CoreWindow') | ForEach-Object { Write-Output $_ }
