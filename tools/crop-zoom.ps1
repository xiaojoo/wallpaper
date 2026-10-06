# Crops a region of a PNG and scales it up with no smoothing, to look at corners and 1px edges.
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$In = 'H:\wallpaper\build\shots\ui_radius.png',
    [string]$Out = 'H:\wallpaper\build\shots\crop.png',
    [int]$X = 0, [int]$Y = 0, [int]$W = 200, [int]$H = 60, [int]$Zoom = 3
)

Add-Type -AssemblyName System.Drawing
$src = [System.Drawing.Bitmap]::FromFile($In)
$rect = New-Object System.Drawing.Rectangle($X, $Y, $W, $H)
$nw = $W * $Zoom
$nh = $H * $Zoom
$dst = New-Object System.Drawing.Bitmap($nw, $nh)
$g = [System.Drawing.Graphics]::FromImage($dst)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$dest = New-Object System.Drawing.Rectangle(0, 0, $nw, $nh)
$g.DrawImage($src, $dest, $rect, [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose()
$dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$dst.Dispose()
$src.Dispose()
Write-Output ("wrote " + $Out + " " + $nw + "x" + $nh)
