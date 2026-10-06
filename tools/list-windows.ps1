# Lists visible top-level windows wider than 200px: handle | class | title
$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class Win
{
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public static List<string> List()
    {
        var o = new List<string>();
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            RECT r; GetWindowRect(h, out r);
            if (r.R - r.L < 200) return true;
            var cls = new StringBuilder(300); GetClassNameW(h, cls, 300);
            var txt = new StringBuilder(300); GetWindowTextW(h, txt, 300);
            o.Add(((long)h).ToString() + " | " + cls + " | " + txt);
            return true;
        }, IntPtr.Zero);
        return o;
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp
[Win]::List() | ForEach-Object { Write-Output $_ }
