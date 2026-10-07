# Does SetCursorPos actually move the pointer? A menu or overlay that calls ClipCursor pins it,
# which is what made the last two clicks land on the same pixel.
$code = @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class Cur
{
    [StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RC { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out PT p);
    [DllImport("user32.dll")] public static extern bool GetClipCursor(out RC r);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PT p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();

    public static string Go(int x, int y)
    {
        PT a; GetCursorPos(out a);
        string from = a.X + "," + a.Y;
        SetCursorPos(x, y);
        Thread.Sleep(200);
        PT b; GetCursorPos(out b);
        RC c; GetClipCursor(out c);
        PT q = b; IntPtr w = WindowFromPoint(q);
        var sb = new System.Text.StringBuilder(256); GetClassNameW(w, sb, 256);
        return "from=" + from + " asked=" + x + "," + y + " at=" + b.X + "," + b.Y +
               " clip=" + c.L + "," + c.T + "," + c.R + "," + c.B +
               " under=" + sb.ToString() + " fg=" + ((long)GetForegroundWindow()).ToString("X");
    }
}
'@
Add-Type -TypeDefinition $code
Write-Output ([Cur]::Go(1200, 1200))
