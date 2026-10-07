# Measures the mean colour of the taskbar band and of the wallpaper strip just above it.
# The band is what a transparency change has to move; the strip above is the reference for
# "what is behind the taskbar right now". Segments with a big sd contain icons or clock text,
# so read the flat ones. ASCII only: this shell reads .ps1 as GBK and drops non-ASCII silently.
param([int]$Segments = 8, [int]$RefRows = 24, [string]$Save = "", [int]$BandSkip = 4)

$src = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Band
{
    [DllImport("user32.dll")] public static extern IntPtr GetSystemMetrics(int n);

    public static void Run(int segs, int refRows, string save, int waBottom, int scrBottom, int scrW, int skip)
    {
        int w = (int)GetSystemMetrics(78);
        int h = (int)GetSystemMetrics(79);
        int x0 = (int)GetSystemMetrics(76);
        int y0 = (int)GetSystemMetrics(77);
        using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        using (Graphics g = Graphics.FromImage(bmp))
        {
            g.CopyFromScreen(x0, y0, 0, 0, new Size(w, h), CopyPixelOperation.SourceCopy);
            if (save.Length > 0) bmp.Save(save, ImageFormat.Png);
            BitmapData bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly,
                                         PixelFormat.Format32bppArgb);
            int[] px = new int[w * h];
            Marshal.Copy(bd.Scan0, px, 0, px.Length);
            bmp.UnlockBits(bd);
            // Screen metrics are the virtual screen; the taskbar rows come from the caller.
            // The window's own first row is a separator line, so the band body starts below it and
            // the wallpaper reference stops above it.
            int edge = waBottom - y0, bandTop = edge + skip, bandBot = scrBottom - y0;
            if (bandBot > h) bandBot = h;
            Console.WriteLine("virtual=" + w + "x" + h + " at " + x0 + "," + y0 +
                              " bandRows=" + bandTop + ".." + (bandBot - 1) +
                              " refRows=" + (edge - refRows) + ".." + (edge - 1));
            int segW = (w / segs) * segs;
            for (int s = 0; s < segs; s++)
            {
                int a = s * (segW / segs), b = (s == segs - 1) ? w : (s + 1) * (segW / segs);
                double[] m = Stat(px, w, a, b, bandTop, bandBot);
                Console.WriteLine("band seg" + s + " x=" + a + ".." + (b - 1) +
                                  " mean=" + F(m[0]) + "," + F(m[1]) + "," + F(m[2]) +
                                  " sd=" + F(m[3]) + "," + F(m[4]) + "," + F(m[5]));
            }
            double[] r = Stat(px, w, 0, w, edge - refRows, edge);
            Console.WriteLine("ref above band mean=" + F(r[0]) + "," + F(r[1]) + "," + F(r[2]) +
                              " sd=" + F(r[3]) + "," + F(r[4]) + "," + F(r[5]));
        }
    }

    static double[] Stat(int[] px, int stride, int xa, int xb, int ya, int yb)
    {
        double n = 0, sr = 0, sg = 0, sb = 0, qr = 0, qg = 0, qb = 0;
        for (int y = ya; y < yb; y++)
        {
            if (y < 0 || y >= px.Length / stride) continue;
            for (int x = xa; x < xb; x++)
            {
                int c = px[y * stride + x];
                double rr = (c >> 16) & 255, gg = (c >> 8) & 255, bb = c & 255;
                n++; sr += rr; sg += gg; sb += bb;
                qr += rr * rr; qg += gg * gg; qb += bb * bb;
            }
        }
        if (n == 0) return new double[] { 0, 0, 0, 0, 0, 0 };
        double vr = qr / n - Math.Pow(sr / n, 2), vg = qg / n - Math.Pow(sg / n, 2),
               vb = qb / n - Math.Pow(sb / n, 2);
        return new double[] { sr / n, sg / n, sb / n,
                              Math.Sqrt(Math.Max(vr, 0)), Math.Sqrt(Math.Max(vg, 0)),
                              Math.Sqrt(Math.Max(vb, 0)) };
    }

    static string F(double v) { return v.ToString("0.0"); }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$w = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$full = ""
if ($Save -ne "") {
  if ([System.IO.Path]::IsPathRooted($Save)) { $full = $Save }
  else { $full = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Save)) }
}
[Band]::Run($Segments, $RefRows, $full, $w.Bottom, $b.Bottom, $b.Width, $BandSkip)
