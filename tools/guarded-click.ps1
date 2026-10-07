# Clicks points inside the settings window, but only after proving with WindowFromPoint that the
# screen coordinate really lands on that window. The user's monitor may be covered by a fullscreen
# game client, and a synthetic click that misses our window would land in his application.
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$Pts = '1000,476',
    [string]$Title = 'Wallpaper',
    [string]$Class = 'Qt6112QWindowIcon',
    [string]$Out = '',
    [switch]$Topmost,
    [switch]$Post
)
$src = @'
using System;
using System.Text;
using System.Drawing;
using System.Runtime.InteropServices;

public static class GC2
{
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; IntPtr dwExtra; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public MOUSEINPUT mi; }

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int cb);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(Point p);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref Point p);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);

    public static string Run(string cls, string title, string pts, string outPath, bool topmost, bool post)
    {
        IntPtr h = IntPtr.Zero; long area = 0;
        EnumWindows((w, l) => {
            var sb = new StringBuilder(300); GetClassNameW(w, sb, 300);
            if (sb.ToString() != cls || !IsWindowVisible(w)) return true;
            if (title.Length > 0) {
                var tb = new StringBuilder(300); GetWindowTextW(w, tb, 300);
                if (tb.ToString().IndexOf(title, StringComparison.OrdinalIgnoreCase) < 0) return true;
            }
            RECT rr; GetWindowRect(w, out rr);
            long a = (long)(rr.R - rr.L) * (rr.B - rr.T);
            if (a > area) { area = a; h = w; }
            return true;
        }, IntPtr.Zero);
        if (h == IntPtr.Zero) return "window not found";

        RECT r; GetWindowRect(h, out r);
        // HWND_TOPMOST when asked: a game client that runs topmost cannot be out-raised with
        // HWND_TOP, and the guard below would (correctly) refuse every point.
        if (topmost) { SetWindowPos(h, (IntPtr)(-2), 0, 0, 0, 0, 1 | 0x10); }
        else { SetWindowPos(h, (IntPtr)0, 0, 0, 0, 0, 1 | 0x10); }   // TOP, NOSIZE, NOACTIVATE
        System.Threading.Thread.Sleep(300);

        int vx = GetSystemMetrics(76), vy = GetSystemMetrics(77);
        int vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        Point client0 = new Point(0, 0); ClientToScreen(h, ref client0);
        var log = new System.Collections.Generic.List<string>();
        foreach (var p in pts.Split(new char[] { ';' }, StringSplitOptions.RemoveEmptyEntries)) {
            var xy = p.Split(',');
            int px = r.L + int.Parse(xy[0]), py = r.T + int.Parse(xy[1]);
            int cx = px - client0.X, cy = py - client0.Y;
            RECT cr; GetClientRect(h, out cr);
            if (cx < 0 || cy < 0 || cx >= cr.R || cy >= cr.B) {
                log.Add("ABORT at " + p + ": client point " + cx + "," + cy + " is outside the window's " + cr.R + "x" + cr.B);
                if (topmost) SetWindowPos(h, (IntPtr)(-3), 0, 0, 0, 0, 1 | 0x10 | 0x1);   // NOTOPMOST
                return string.Join("\n", log.ToArray());
            }
            if (post) {
                // Straight to the window's own queue: no cursor, no z-order, so it cannot land in
                // whatever else happens to be on top of it (a topmost game client was).
                IntPtr lp = (IntPtr)((cy << 16) | (cx & 0xFFFF));
                PostMessage(h, 0x0200, IntPtr.Zero, lp);
                System.Threading.Thread.Sleep(90);
                PostMessage(h, 0x0201, (IntPtr)0x0001, lp);
                System.Threading.Thread.Sleep(60);
                PostMessage(h, 0x0202, IntPtr.Zero, lp);
                System.Threading.Thread.Sleep(450);
                log.Add("posted click " + p + " -> client " + cx + "," + cy);
                continue;
            }
            IntPtr hit = WindowFromPoint(new Point(px, py));
            if (hit != h) {
                var cb = new StringBuilder(300); if (hit != IntPtr.Zero) GetClassNameW(hit, cb, 300);
                log.Add("ABORT at " + p + ": point belongs to 0x" + ((long)hit).ToString("X") + " (" + cb + "), not our window");
                if (topmost) SetWindowPos(h, (IntPtr)(-3), 0, 0, 0, 0, 1 | 0x10 | 0x1);   // NOTOPMOST
                return string.Join("\n", log.ToArray());
            }
            int ax = (int)(((long)(px - vx) * 65535) / (vw - 1));
            int ay = (int)(((long)(py - vy) * 65535) / (vh - 1));
            Send(0x8001 | 0x4000, ax, ay);
            System.Threading.Thread.Sleep(120);
            Send(0x0002, 0, 0);
            System.Threading.Thread.Sleep(60);
            Send(0x0004, 0, 0);
            System.Threading.Thread.Sleep(450);
            log.Add("clicked " + p + " (verified on our window)");
        }
        if (topmost) SetWindowPos(h, (IntPtr)(-3), 0, 0, 0, 0, 1 | 0x10 | 0x1);   // NOTOPMOST
        if (outPath.Length > 0) {
            var bmp = new Bitmap(r.R - r.L, r.B - r.T);
            using (var g = Graphics.FromImage(bmp)) {
                IntPtr dc = g.GetHdc();
                PrintWindow(h, dc, 2);   // PW_RENDERFULLCONTENT: Qt draws through DirectComposition
                g.ReleaseHdc(dc);
            }
            bmp.Save(outPath, System.Drawing.Imaging.ImageFormat.Png);
            bmp.Dispose();
            log.Add("captured " + outPath);
        }
        return string.Join("\n", log.ToArray());
    }

    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, int flags);

    static void Send(uint flags, int x, int y)
    {
        INPUT[] i = new INPUT[1];
        i[0].type = 0;
        i[0].mi.dx = x; i[0].mi.dy = y; i[0].mi.dwFlags = flags;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing
Write-Output ([GC2]::Run($Class, $Title, $Pts, $Out, [bool]$Topmost, [bool]$Post))
