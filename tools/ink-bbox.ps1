# Bounding box of pixels that differ from a reference colour by more than a threshold, inside a
# region. Used to find where glyph ink actually reaches, so a stroke can be placed beside it.
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$In = 'H:\wallpaper\build\shots\ui_home.png',
    [int]$X0 = 0, [int]$Y0 = 0, [int]$X1 = 100, [int]$Y1 = 100,
    [int]$RefR = 28, [int]$RefG = 37, [int]$RefB = 50,
    [int]$Threshold = 40
)

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($In)
$minX = 100000; $maxX = -1; $minY = 100000; $maxY = -1; $hits = 0
for ($y = $Y0; $y -le $Y1; $y++) {
    for ($x = $X0; $x -le $X1; $x++) {
        $c = $bmp.GetPixel($x, $y)
        $d = ([Math]::Abs($c.R - $RefR) + [Math]::Abs($c.G - $RefG) + [Math]::Abs($c.B - $RefB))
        if ($d -gt $Threshold) {
            $hits = $hits + 1
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }
}
Write-Output ("region " + $X0 + "," + $Y0 + " " + ($X1 - $X0 + 1) + "x" + ($Y1 - $Y0 + 1) +
              " ref " + $RefR + "," + $RefG + "," + $RefB + " thr " + $Threshold)
if ($maxX -lt 0) { Write-Output 'ink: none'; $bmp.Dispose(); exit }
Write-Output ("ink bbox " + $minX + "," + $minY + " - " + $maxX + "," + $maxY +
              " w=" + ($maxX - $minX + 1) + " h=" + ($maxY - $minY + 1) +
              " px=" + $hits + " (counting the border rows is on you)")
$bmp.Dispose()
