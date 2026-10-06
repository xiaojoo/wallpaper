# Generates the tileable grain texture used by the embers wallpaper (exercises the WIC load path).
$src = @'
using System;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;

public static class Grain
{
    public static void Run(string outPath)
    {
        const int S = 512;
        var rnd = new Random(20261004);
        byte[,] field = new byte[8, 8];
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) field[x, y] = (byte)rnd.Next(0, 256);

        Bitmap bmp = new Bitmap(S, S, PixelFormat.Format32bppArgb);
        BitmapData bd = bmp.LockBits(new Rectangle(0, 0, S, S), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        byte[] buf = new byte[S * S * 4];

        for (int py = 0; py < S; py++)
        {
            double fy = py / (double)S * 8.0;
            int y0 = (int)Math.Floor(fy) % 8, y1 = (y0 + 1) % 8;
            double ty = fy - Math.Floor(fy);
            double sy = ty * ty * (3 - 2 * ty);
            for (int px = 0; px < S; px++)
            {
                double fx = px / (double)S * 8.0;
                int x0 = (int)Math.Floor(fx) % 8, x1 = (x0 + 1) % 8;
                double tx = fx - Math.Floor(fx);
                double sx = tx * tx * (3 - 2 * tx);
                double a = field[x0, y0], b = field[x1, y0], c = field[x0, y1], d = field[x1, y1];
                int v = (int)((a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy);
                int g = (int)(v * 0.72 + rnd.Next(0, 256) * 0.28);
                if (g > 255) g = 255;
                int i = (py * S + px) * 4;
                buf[i] = (byte)g; buf[i + 1] = (byte)g; buf[i + 2] = (byte)g; buf[i + 3] = 255;
            }
        }
        Marshal.Copy(buf, 0, bd.Scan0, buf.Length);
        bmp.UnlockBits(bd);
        bmp.Save(outPath, ImageFormat.Png);
        bmp.Dispose();
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$target = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($here, '..', 'Wallpapers', 'embers', 'textures'))
$null = New-Item -ItemType Directory -Force -Path $target
$full = [System.IO.Path]::Combine($target, 'grain.png')
Write-Output ("target " + $full)
[Grain]::Run($full)
Write-Output ("grain.png bytes=" + (Get-Item -LiteralPath $full).Length)
