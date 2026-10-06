# Raises the settings window (without stealing activation), clicks a point inside it, and captures
# the result. Used because Qt's self-drawn controls are not reachable through UIA on this build.
param([int]$X = 420, [int]$Y = 210, [string]$Out = 'click.png', [string]$Class = 'Qt6112QWindowIcon')
$src = @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class Clicker
{
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; IntPtr dwExtra; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int cb);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);

    const int HWND_TOP = 0;
    const uint SWP_NOSIZE = 1, SWP_NOACTIVATE = 0x10;

    static IntPtr Find(string cls)
    {
        IntPtr best = IntPtr.Zero; long area = 0;
        EnumWindows((h, l) => {
            var sb = new StringBuilder(300); GetClassNameW(h, sb, 300);
            if (sb.ToString() != cls || !IsWindowVisible(h)) return true;
            RECT r; GetWindowRect(h, out r);
            long a = (long)(r.R - r.L) * (r.B - r.T);
            if (a > area) { area = a; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    static void Send(uint flags, int x, int y)
    {
        INPUT[] i = new INPUT[1];
        i[0].type = 0;
        i[0].mi.dx = x; i[0].mi.dy = y; i[0].mi.dwFlags = flags;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }

    public static string Run(string cls, int cx, int cy)
    {
        IntPtr h = Find(cls);
        if (h == IntPtr.Zero) return "window not found";
        RECT r; GetWindowRect(h, out r);
        SetWindowPos(h, (IntPtr)HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        System.Threading.Thread.Sleep(250);
        int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77);
        int vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        int px = r.L + cx, py = r.T + cy;
        int ax = (int)(((long)(px - vx) * 65535) / (vw - 1));
        int ay = (int)(((long)(py - vy) * 65535) / (vh - 1));
        Send(0x8001 /*MOVE|ABSOLUTE*/ | 0x4000 /*VIRTUALDESK*/, ax, ay);
        System.Threading.Thread.Sleep(120);
        Send(0x0002 /*LEFTDOWN*/, 0, 0);
        System.Threading.Thread.Sleep(60);
        Send(0x0004 /*LEFTUP*/, 0, 0);
        System.Threading.Thread.Sleep(400);
        return string.Format("clicked {0},{1} of window at {2},{3} {4}x{5}", cx, cy, r.L, r.T, r.R - r.L, r.B - r.T);
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp
Write-Output ([Clicker]::Run($Class, $X, $Y))
$pw = Join-Path $PSScriptRoot 'capture-window.ps1'
& $pw -Out $Out -Class $Class
