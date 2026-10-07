# tools/measure-fall-speed.ps1 - a physical ruler for "how fast is the wallpaper moving".
#
# It grabs the real composited screen twice (CopyFromScreen - what the eye sees, not what the
# renderer claims), then finds the vertical shift of the brightness profile between the two grabs.
# The result is px/s, and screen-heights/s, which is the unit the packages themselves declare:
#   snowfall  params.fall_speed = 0.075  screen heights / s   -> 162.0 px/s at 2160
#   neonfall  params.fall_speed = 0.140 (middle depth, x1.1)  -> ~222 px/s
# If the two numbers disagree, the wallpaper is not running at its own declared speed - and this
# does not care whether the engine's delta, the package's clamp or the frame cap caused it.
#
# The desktop has to actually be visible: a window covering the screen is what gets captured, and
# then the answer is "nothing moved", not a speed. The tool reports the residual so that case is
# distinguishable from a still frame.
#
# usage:  measure-fall-speed.ps1 -Delta 2.0 [-MaxShift 400] [-Out build]
param(
    [double]$Delta = 2.0,
    [int]$MaxShift = 400,
    [int]$StripX = -1,
    [int]$StripW = -1,
    [string]$Out = 'build'
)

$src = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Fall
{
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);

    static double[] Profile(string path, int sx, int sw, out double mean)
    {
        mean = 0.0;
        using (Bitmap bmp = new Bitmap(path))
        {
            int h = bmp.Height;
            if (sx < 0) { sx = Math.Max(0, bmp.Width / 2 - 600); sw = 1200; }
            if (sx + sw > bmp.Width) sw = bmp.Width - sx;
            int stride = sw * 4;
            byte[] buf = new byte[stride];
            double[] rows = new double[h];
            Rectangle rect = new Rectangle(sx, 0, sw, h);
            BitmapData data = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            for (int y = 0; y < h; y++)
            {
                Marshal.Copy(IntPtr.Add(data.Scan0, y * data.Stride), buf, 0, stride);
                long sum = 0;
                // every 4th column is plenty for a vertical profile and keeps this cheap
                for (int px = 0; px < stride; px += 16)
                    sum += (long)buf[px] + (long)buf[px + 1] + (long)buf[px + 2];
                rows[y] = sum / (double)((stride / 16) * 3);
                mean += rows[y];
            }
            bmp.UnlockBits(data);
            mean /= h;
            return rows;
        }
    }

    public static void Run(string pathA, string pathB, int sx, int sw, int maxShift, double dt)
    {
        double meanA, meanB;
        double[] a = Profile(pathA, sx, sw, out meanA);
        double[] b = Profile(pathB, sx, sw, out meanB);
        int h = a.Length;

        // mean-removed: a whole-screen brightness drift must not read as motion
        for (int i = 0; i < h; i++) { a[i] -= meanA; b[i] -= meanB; }

        int lo = maxShift + 8, hi = h - maxShift - 8;
        if (hi - lo < 100) { Console.WriteLine("region too short to search " + maxShift + " px"); return; }

        double best = 1e30; int bestD = -999; double zero = 0;
        for (int d = -maxShift; d <= maxShift; d++)
        {
            double acc = 0.0; int n = 0;
            for (int y = lo; y < hi; y += 2)
            {
                int j = y + d;
                if (j < 0 || j >= h) continue;
                acc += Math.Abs(a[y] - b[j]); n++;
            }
            double mad = acc / n;
            if (d == 0) zero = mad;
            if (mad < best) { best = mad; bestD = d; }
        }
        int monH = GetSystemMetrics(79);
        double pxPerS = bestD / dt;
        Console.WriteLine(string.Format(
            "shift={0}px in {1}s  ->  {2:F1} px/s  =  {3:F4} screen-heights/s (monitor {4}px)  " +
            "residual={5:F2} atZero={6:F2} meanA={7:F1} meanB={8:F1}",
            bestD, dt.ToString("F3"), pxPerS, pxPerS / monH, monH, best, zero, meanA, meanB));
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing

$capSrc = @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Grab
{
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);

    public static double Run(string path)
    {
        int w = GetSystemMetrics(78), h = GetSystemMetrics(79);
        long stamp = 0;
        using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        using (Graphics g = Graphics.FromImage(bmp))
        {
            stamp = Stopwatch.GetTimestamp();
            g.CopyFromScreen(0, 0, 0, 0, new Size(w, h), CopyPixelOperation.SourceCopy);
            bmp.Save(path, ImageFormat.Png);
        }
        return (double)stamp;
    }
    public static double Freq() { return (double)Stopwatch.Frequency; }
}
'@
Add-Type -TypeDefinition $capSrc -Language CSharp -ReferencedAssemblies System.Drawing

New-Item -ItemType Directory -Force -Path $Out | Out-Null
$f1 = Join-Path $Out 'fall-a.png'
$f2 = Join-Path $Out 'fall-b.png'
$t1 = [Grab]::Run($f1)
Start-Sleep -Milliseconds ([int]($Delta * 1000))
$t2 = [Grab]::Run($f2)
$dt = ($t2 - $t1) / [Grab]::Freq()
[Fall]::Run($f1, $f2, $StripX, $StripW, $MaxShift, $dt)
