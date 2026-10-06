# Reports where shapes start and stop along a row or column of a screenshot, so "same inset" and
# "same gap" can be answered with numbers instead of looking at it.
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$In = 'H:\wallpaper\build\shots\ui_gaps.png',
    [string]$Rows = '',
    [string]$Cols = '',
    [int]$Threshold = 7
)

Add-Type -AssemblyName System.Drawing
$bmp = [System.Drawing.Bitmap]::FromFile($In)

function Delta([System.Drawing.Bitmap]$b, [int]$x, [int]$y, [System.Drawing.Color]$ref) {
    $c = $b.GetPixel($x, $y)
    return ([Math]::Abs($c.R - $ref.R) + [Math]::Abs($c.G - $ref.G) + [Math]::Abs($c.B - $ref.B))
}

Write-Output ("image " + $bmp.Width + "x" + $bmp.Height)

foreach ($y in ($Rows -split "," | Where-Object { $_ -ne "" } | ForEach-Object { [int]$_ })) {
    $ref = $bmp.GetPixel(4, $y)          # the page background, sampled at the far left
    $runs = New-Object System.Collections.Generic.List[string]
    $start = -1
    for ($x = 1; $x -lt $bmp.Width; $x++) {
        $on = (Delta $bmp $x $y $ref) -gt $Threshold
        if ($on -and $start -lt 0) { $start = $x }
        if ((-not $on) -and $start -ge 0) {
            if ($x - $start -ge 3) { $runs.Add(("$start..$($x-1) w=$($x-$start)")) | Out-Null }
            $start = -1
        }
    }
    if ($start -ge 0) { $runs.Add(("$start..end")) | Out-Null }
    Write-Output ("row y=$y : " + ($runs -join '  '))
}

foreach ($x in ($Cols -split "," | Where-Object { $_ -ne "" } | ForEach-Object { [int]$_ })) {
    $ref = $bmp.GetPixel($x, 40)
    $runs = New-Object System.Collections.Generic.List[string]
    $start = -1
    for ($y = 1; $y -lt $bmp.Height; $y++) {
        $on = (Delta $bmp $x $y $ref) -gt $Threshold
        if ($on -and $start -lt 0) { $start = $y }
        if ((-not $on) -and $start -ge 0) {
            if ($y - $start -ge 3) { $runs.Add(("$start..$($y-1) h=$($y-$start)")) | Out-Null }
            $start = -1
        }
    }
    if ($start -ge 0) { $runs.Add(("$start..end")) | Out-Null }
    Write-Output ("col x=$x : " + ($runs -join '  '))
}

$bmp.Dispose()
