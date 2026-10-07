# Mean colour of every screen row in a band, over one x span. Finds where one surface ends and the
# next begins - used to check that the reference rows really are wallpaper and not the taskbar's own
# shadow. ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param([int]$Y0 = 2040, [int]$Y1 = 2160, [int]$X0 = 900, [int]$X1 = 1500, [int]$Step = 4)

$src = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Rows
{
    [DllImport("user32.dll")] public static extern IntPtr GetSystemMetrics(int n);

    public static void Run(int y0, int y1, int x0, int x1, int step)
    {
        int w = (int)GetSystemMetrics(78), h = (int)GetSystemMetrics(79);
        int ox = (int)GetSystemMetrics(76), oy = (int)GetSystemMetrics(77);
        using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        using (Graphics g = Graphics.FromImage(bmp))
        {
            g.CopyFromScreen(ox, oy, 0, 0, new Size(w, h), CopyPixelOperation.SourceCopy);
            BitmapData bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly,
                                         PixelFormat.Format32bppArgb);
            int[] px = new int[w * h];
            Marshal.Copy(bd.Scan0, px, 0, px.Length);
            bmp.UnlockBits(bd);
            for (int y = y0; y < y1; y += step)
            {
                int r = (y - oy) * w + (x0 - ox);
                double sr = 0, sg = 0, sb = 0; int n = 0;
                for (int x = x0; x < x1; x++, r++)
                {
                    int c = px[r];
                    sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255; n++;
                }
                Console.WriteLine("y=" + (y - oy) + " mean=" +
                    (sr / n).ToString("0.0") + "," + (sg / n).ToString("0.0") + "," +
                    (sb / n).ToString("0.0"));
            }
        }
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing
[Rows]::Run($Y0, $Y1, $X0, $X1, $Step)
