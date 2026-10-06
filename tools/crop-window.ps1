# Crops a window's region out of a full-screen capture, by window class.
param([string]$Src = 'shot.png', [string]$Out = 'crop.png', [string]$Class = 'Qt6112QWindowIcon')
$src = @'
using System;
using System.Text;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class Crop
{
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public static string Run(string cls, string srcPath, string outPath)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            var sb = new StringBuilder(300);
            GetClassNameW(h, sb, 300);
            if (sb.ToString() == cls && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        if (found == IntPtr.Zero) return "window class not found: " + cls;
        RECT r; GetWindowRect(found, out r);
        int x = Math.Max(0, r.L), y = Math.Max(0, r.T); int w = 0, hh = 0;
        using (Bitmap full = new Bitmap(srcPath))
        {
            w = r.R - r.L; hh = r.B - r.T;
            if (x + w > full.Width) w = full.Width - x;
            if (y + hh > full.Height) hh = full.Height - y;
            Console.WriteLine("rect " + r.L + "," + r.T + " " + (r.R - r.L) + "x" + (r.B - r.T) + " img " + full.Width + "x" + full.Height);
            if (w < 10 || hh < 10) return "window too small: " + w + "x" + hh;
            using (Bitmap cut = full.Clone(new Rectangle(x, y, w, hh), PixelFormat.Format32bppArgb))
            {
                cut.Save(outPath, ImageFormat.Png);
            }
        }
        return string.Format("cropped {0}x{1} at {2},{3} -> {4}", w, hh, x, y, outPath);
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Drawing
$s = if ([System.IO.Path]::IsPathRooted($Src)) { $Src } else { (Join-Path (Get-Location) $Src) }
$o = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { (Join-Path (Get-Location) $Out) }
Write-Output ([Crop]::Run($Class, $s, $o))
