# Compares two PNG screenshots: how much moved, and what the top-left corner looks like.
param([Parameter(Mandatory=$true)][string]$A, [Parameter(Mandatory=$true)][string]$B, [string]$Label = 'pair')
$src = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Cmp
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Stats
    {
        public long changed;
        public long total;
        public double meanAbsDiff;
        public double maxDiff;
        public long cornerR, cornerG, cornerB;
        public long leftMeanR, leftMeanG, leftMeanB;
    }

    static byte[] Load(string path, out int w, out int h, out int pitch)
    {
        using (Bitmap bmp = new Bitmap(path))
        {
            w = bmp.Width; h = bmp.Height;
            BitmapData bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            pitch = bd.Stride;
            byte[] buf = new byte[Math.Abs(pitch) * h];
            Marshal.Copy(bd.Scan0, buf, 0, buf.Length);
            bmp.UnlockBits(bd);
            return buf;
        }
    }

    public static Stats Run(string pa, string pb)
    {
        int w1, h1, p1, w2, h2, p2;
        byte[] a = Load(pa, out w1, out h1, out p1);
        byte[] b = Load(pb, out w2, out h2, out p2);
        Stats s = new Stats();
        if (w1 != w2 || h1 != h2) { s.total = -1; return s; }
        double sum = 0;
        double lr = 0, lg = 0, lb = 0; long ln = 0;
        int cornerSide = 64;
        for (int y = 0; y < h1; y++)
        {
            for (int x = 0; x < w1; x++)
            {
                int i = y * p1 + x * 4;
                int j = y * p2 + x * 4;
                int dr = Math.Abs(a[i] - b[j]);
                int dg = Math.Abs(a[i + 1] - b[j + 1]);
                int db = Math.Abs(a[i + 2] - b[j + 2]);
                int d = dr + dg + db;
                if (d > 6) s.changed++;
                sum += d;
                if (d / 3.0 > s.maxDiff) s.maxDiff = d / 3.0;
                s.total++;
                if (x < cornerSide && y < cornerSide) { s.cornerR += a[i + 2]; s.cornerG += a[i + 1]; s.cornerB += a[i]; }
                if (x < 400) { lr += a[i + 2]; lg += a[i + 1]; lb += a[i]; ln++; }
            }
        }
        s.meanAbsDiff = sum / (double)s.total / 3.0;
        s.cornerR /= (cornerSide * cornerSide); s.cornerG /= (cornerSide * cornerSide); s.cornerB /= (cornerSide * cornerSide);
        s.leftMeanR = (long)(lr / ln); s.leftMeanG = (long)(lg / ln); s.leftMeanB = (long)(lb / ln);
        return s;
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing
$ra = [Cmp]::Run($A, $B)
if ($ra.total -lt 0) { Write-Output "SIZE MISMATCH"; exit 1 }
$pctP = 100.0 * $ra.changed / $ra.total
Write-Output ("{0}: px={1} changed={2} ({3:N3}%) meanAbsDiff={4:N3} maxDiff={5:N0} cornerBGRA=({6},{7},{8}) left400mean=({9},{10},{11})" -f `
  $Label, $ra.total, $ra.changed, $pctP, $ra.meanAbsDiff, $ra.maxDiff, $ra.cornerR, $ra.cornerG, $ra.cornerB, `
  $ra.leftMeanR, $ra.leftMeanG, $ra.leftMeanB)
