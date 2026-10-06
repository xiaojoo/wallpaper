# Prints the RGB of a run of pixels along one row or column, so a multi-layer stroke can be read
# as numbers instead of guesses. ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$In = 'H:\wallpaper\build\shots\ui_home.png',
    [string]$Rows = '',
    [string]$Cols = '',
    [int]$From = 0,
    [int]$To = 0
)

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($In)
Write-Output ("image " + $bmp.Width + "x" + $bmp.Height)

foreach ($y in ($Rows -split "," | Where-Object { $_ -ne "" } | ForEach-Object { [int]$_ })) {
    $a = $From; if ($To -le $a) { $a = 0 }
    $b = $To; if ($b -le $a) { $b = $bmp.Width - 1 }
    $line = New-Object System.Collections.Generic.List[string]
    for ($x = $a; $x -le $b; $x++) {
        $c = $bmp.GetPixel($x, $y)
        $line.Add(("x" + $x + " " + $c.R + "," + $c.G + "," + $c.B))
    }
    Write-Output ("row y=" + $y + " : " + ($line -join ' | '))
}

foreach ($x in ($Cols -split "," | Where-Object { $_ -ne "" } | ForEach-Object { [int]$_ })) {
    $a = $From; if ($To -le $a) { $a = 0 }
    $b = $To; if ($b -le $a) { $b = $bmp.Height - 1 }
    $line = New-Object System.Collections.Generic.List[string]
    for ($y = $a; $y -le $b; $y++) {
        $c = $bmp.GetPixel($x, $y)
        $line.Add(("y" + $y + " " + $c.R + "," + $c.G + "," + $c.B))
    }
    Write-Output ("col x=" + $x + " : " + ($line -join ' | '))
}
$bmp.Dispose()
