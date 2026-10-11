# Stage 1 follow-up: what size does the shell ACTUALLY give us for a desktop shortcut's icon?

# The ring draws its front icons at 0.085 * 2160 = 184 device pixels across, while the atlas cell is
# 128 px - so even a perfect 128 px source is magnified 1.44x and will look soft. But that only
# explains softness at 128. This asks whether the shell hands over 128 at all, or something smaller
# that we then stretch twice over.
#
# SHDefExtractIconW takes the wanted size in its last parameter and is documented to give the best
# match from the file. GetIconInfo then reports the colour bitmap's real height, from which the
# square edge follows (icons are square, and the mask row pitch is padded to a DWORD).

$ErrorActionPreference = 'Stop'

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;

public class IconSize {
  [DllImport("shell32.dll", CharSet=CharSet.Unicode)] public static extern int SHDefExtractIconW(string file, int idx, uint flags, out IntPtr big, out IntPtr small, uint cx);
  [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetIconInfo(IntPtr h, out ICONINFO ii);
  [DllImport("gdi32.dll")] public static extern bool GetObjectW(IntPtr obj, int cb, IntPtr pv);

  [StructLayout(LayoutKind.Sequential)]
  public struct BITMAP { public int type; public int width; public int height; public int planes; public int bitCount; public int sizeImage; public IntPtr bits; public IntPtr mask; public int colorsUsed; public int colorsImportant; }

  [StructLayout(LayoutKind.Sequential)]
  public struct ICONINFO { public bool fIcon; public int xHotspot; public int yHotspot; public IntPtr hbmMask; public IntPtr hbmColor; }

  public static string Edge(IntPtr hicon) {
    ICONINFO ii;
    if (!GetIconInfo(hicon, out ii)) return "GetIconInfo failed";
    string r;
    if (ii.hbmColor == IntPtr.Zero) {
      // A monochrome icon: only the AND mask exists, and its height is 2x the edge.
      BITMAP b;
      IntPtr mem = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(BITMAP)));
      bool ok = GetObjectW(ii.hbmMask, Marshal.SizeOf(typeof(BITMAP)), mem);
      b = ok ? (BITMAP)Marshal.PtrToStructure(mem, typeof(BITMAP)) : new BITMAP();
      Marshal.FreeHGlobal(mem);
      r = "mono, mask height " + b.height + " -> edge " + (b.height / 2);
    } else {
      IntPtr mem = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(BITMAP)));
      bool ok = GetObjectW(ii.hbmColor, Marshal.SizeOf(typeof(BITMAP)), mem);
      BITMAP b = ok ? (BITMAP)Marshal.PtrToStructure(mem, typeof(BITMAP)) : new BITMAP();
      Marshal.FreeHGlobal(mem);
      r = "color " + b.width + "x" + b.height + " at " + b.bitCount + "bpp";
      DeleteObject(ii.hbmColor);
    }
    if (ii.hbmMask != IntPtr.Zero) DeleteObject(ii.hbmMask);
    return r;
  }
  [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
}
"@

$dirs = @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('CommonDesktopDirectory'))
$lnk = @()
foreach ($d in $dirs) { if (Test-Path $d) { $lnk += Get-ChildItem -LiteralPath $d -Filter *.lnk -File } }
Write-Output ("desktop .lnk count: " + $lnk.Count)

foreach ($want in 128, 256) {
    Write-Output ("--- asking SHDefExtractIconW for " + $want + " px")
    $seen = @{}
    foreach ($f in ($lnk | Select-Object -First 14)) {
        $big = [IntPtr]::Zero; $small = [IntPtr]::Zero
        [void][IconSize]::SHDefExtractIconW($f.FullName, -1, 0, [ref]$big, [ref]$small, [uint32]$want)
        if ($big -ne [IntPtr]::Zero) {
            $d = [IconSize]::Edge($big)
            $key = $d
            if ($seen.ContainsKey($key)) { $seen[$key] = $seen[$key] + 1 } else { $seen[$key] = 1 }
            [void][IconSize]::DestroyIcon($big)
        } else {
            $k = 'returned no icon'; if ($seen.ContainsKey($k)) { $seen[$k]++ } else { $seen[$k] = 1 }
        }
        if ($small -ne [IntPtr]::Zero) { [void][IconSize]::DestroyIcon($small) }
    }
    foreach ($k in $seen.Keys) { Write-Output ("   " + $k + "  x" + $seen[$k]) }
}

# The one he screenshotted, named out.
$chrome = $lnk | Where-Object { $_.Name -like '*hrom*' } | Select-Object -First 1
if ($chrome) {
    foreach ($want in 128, 256) {
        $big = [IntPtr]::Zero; $small = [IntPtr]::Zero
        [void][IconSize]::SHDefExtractIconW($chrome.FullName, -1, 0, [ref]$big, [ref]$small, [uint32]$want)
        Write-Output ($chrome.Name + " asked for " + $want + " -> " + $(if ($big) { [IconSize]::Edge($big) } else { 'none' }))
        if ($big) { [void][IconSize]::DestroyIcon($big) }
        if ($small) { [void][IconSize]::DestroyIcon($small) }
    }
}
