# Solid-colour fixtures for the taskbar blend measurement: two colours as far apart as the format
# gets, so the slope of "taskbar pixel per wallpaper pixel" is readable against capture noise.
param([int]$W = 3840, [int]$H = 2160, [string]$Dir = "H:\wallpaper\build\fixtures")

Add-Type -AssemblyName System.Drawing
if (!(Test-Path $Dir)) { New-Item -ItemType Directory -Path $Dir | Out-Null }
$colors = @{ red = [System.Drawing.Color]::FromArgb(255, 0, 0)
             blue = [System.Drawing.Color]::FromArgb(0, 0, 255) }
foreach ($k in $colors.Keys) {
  $bmp = New-Object System.Drawing.Bitmap($W, $H)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.Clear($colors[$k])
  $g.Dispose()
  $out = Join-Path $Dir ("solid_" + $k + ".png")
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  Write-Output ("wrote " + $out)
}
