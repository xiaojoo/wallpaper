# Extracts the icon each executable carries and saves it as a PNG, so the embedded resource can be
# looked at rather than assumed. ExtractAssociatedIcon hands back the small form the shell uses.
param([string]$Out = "H:\wallpaper\build\icons\from-exe.png")

Add-Type -AssemblyName System.Drawing
$paths = @('H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe',
           'H:\wallpaper\bld\bin\RelWithDebInfo\Wallpaper.exe')
$cells = @()
foreach ($p in $paths) {
    $ic = [System.Drawing.Icon]::ExtractAssociatedIcon($p)
    Write-Output ($p + " -> " + $ic.Width + "x" + $ic.Height)
    $bmp = $ic.ToBitmap()
    $cells += ,@([System.IO.Path]::GetFileName($p), $bmp)
}
$w = 0; $h = 0
foreach ($c in $cells) { $w += $c[1].Width * 4 + 24; if ($c[1].Height -gt $h) { $h = $c[1].Height } }
# The sheet is sized for the 4x drawing, not for the 1x icons - sizing it at 1x clipped the bottom
# half of every cell and made a good icon look truncated.
$sheet = New-Object System.Drawing.Bitmap(($w + 24), ($h * 4 + 24))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear([System.Drawing.Color]::FromArgb(21, 28, 39))
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$x = 12
foreach ($c in $cells) {
    # drawn at 4x so a 32 px icon can actually be judged on screen
    $g.DrawImage($c[1], $x, 12, ($c[1].Width * 4), ($c[1].Height * 4))
    $x += ($c[1].Width * 4) + 24
}
$g.Dispose()
$sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output ("wrote " + $Out)
