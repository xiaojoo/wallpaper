# Captures a rectangle of the REAL composited screen as fast as possible, stamping each frame.
# PrintWindow on a Qt Quick window returns a stale bitmap (6 s / 251 captures, zero pixel delta),
# so cadence has to be measured off the desktop like the user sees it. ASCII only.
param([int]$Seconds = 4, [switch]$RectOnly, [string]$OutDir = 'C:\temp\cadence',
       [int]$X = 0, [int]$Y = 0, [int]$W = 400, [int]$H = 300)

$src = @'
using System;
using System.Text;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Scr
{
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public static string Rect(string cls, string title)
    {
        IntPtr found = IntPtr.Zero;
        long best = 0;
        EnumWindows((h, l) => {
            var sb = new StringBuilder(300);
            GetClassNameW(h, sb, 300);
            if (sb.ToString() != cls || !IsWindowVisible(h)) return true;
            var tb = new StringBuilder(300);
            GetWindowTextW(h, tb, 300);
            if (tb.ToString().IndexOf(title, StringComparison.OrdinalIgnoreCase) < 0) return true;
            RECT rr; GetWindowRect(h, out rr);
            long area = (long)(rr.R - rr.L) * (rr.B - rr.T);
            if (area > best) { best = area; found = h; }
            return true;
        }, IntPtr.Zero);
        if (found == IntPtr.Zero) return "none";
        RECT r; GetWindowRect(found, out r);
        return r.L + "," + r.T + "," + r.R + "," + r.B;
    }

    public static Bitmap Grab(int x, int y, int w, int h)
    {
        Bitmap bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        using (Graphics g = Graphics.FromImage(bmp)) g.CopyFromScreen(x, y, 0, 0, new Size(w, h), CopyPixelOperation.SourceCopy);
        return bmp;
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing

if ($RectOnly) { Write-Output ([Scr]::Rect('Qt6112QWindowIcon', 'Wallpaper')); exit 0 }

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
else { Remove-Item -Path (Join-Path $OutDir '*.png') -Force -ErrorAction SilentlyContinue }

$origin = [DateTime]::UtcNow
$end = $origin.AddSeconds($Seconds)
$lines = @()
$k = 0
while ([DateTime]::UtcNow -lt $end) {
    $ms = ([DateTime]::UtcNow - $origin).TotalMilliseconds
    $bmp = [Scr]::Grab($X, $Y, $W, $H)
    $name = Join-Path $OutDir ('c{0:d5}.png' -f $k)
    $bmp.Save($name, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    $lines += ('{0:d5} {1:F1}' -f $k, $ms)
    $k++
}
$lines | Set-Content -Path (Join-Path $OutDir 'times.txt') -Encoding ASCII
Write-Output ("captures=" + $k + " rect=" + $X + "," + $Y + "," + $W + "," + $H)
