# Captures the real composited screen (what the user sees) to a PNG and prints a coarse summary.
param([string]$Out = 'shot.png', [int]$X = -1, [int]$Y = -1, [int]$W = 0, [int]$H = 0)
$src = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Shot
{
    [DllImport("user32.dll")] public static extern IntPtr GetSystemMetrics(int n);

    public static void Run(string path, int cx, int cy, int cw, int ch)
    {
        int w = (int)GetSystemMetrics(78); // SM_CXVIRTUALSCREEN
        int h = (int)GetSystemMetrics(79); // SM_CYVIRTUALSCREEN
        int x = (int)GetSystemMetrics(76); // SM_XVIRTUALSCREEN
        int y = (int)GetSystemMetrics(77);
        if (cw > 0 && ch > 0) { x += cx; y += cy; w = cw; h = ch; }
        using (Bitmap bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        using (Graphics g = Graphics.FromImage(bmp))
        {
            g.CopyFromScreen(x, y, 0, 0, new Size(w, h), CopyPixelOperation.SourceCopy);
            bmp.Save(path, ImageFormat.Png);
            Console.WriteLine(string.Format("captured {0}x{1} at {2},{3} -> {4}", w, h, x, y, path));
        }
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing
if ([System.IO.Path]::IsPathRooted($Out)) { $full = $Out } else { $full = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Out)) }
[Shot]::Run($full, $X, $Y, $W, $H)
