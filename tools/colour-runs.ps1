# Rows in one column (or columns in one row) whose pixel is within a tolerance of a target colour.
# Used to find where a known stroke actually landed. ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$In = 'H:\wallpaper\build\shots\ui_home.png',
    [string]$Mode = 'col',
    [int]$Fixed = 630,
    [int]$From = 0,
    [int]$To = 200,
    [int]$TR = 59, [int]$TG = 130, [int]$TB = 246,
    [int]$Threshold = 24
)

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($In)
$runs = New-Object System.Collections.Generic.List[string]
$start = -1
for ($i = $From; $i -le $To; $i++) {
    if ($Mode -eq 'col') { $c = $bmp.GetPixel($Fixed, $i) } else { $c = $bmp.GetPixel($i, $Fixed) }
    $d = ([Math]::Abs($c.R - $TR) + [Math]::Abs($c.G - $TG) + [Math]::Abs($c.B - $TB))
    $on = $d -le $Threshold
    if ($on -and $start -lt 0) { $start = $i }
    if ((-not $on) -and $start -ge 0) { $runs.Add(($start.ToString() + '..' + ($i - 1).ToString())); $start = -1 }
}
if ($start -ge 0) { $runs.Add(($start.ToString() + '..' + $To.ToString())) }
Write-Output ($Mode + " " + $Fixed + " target " + $TR + "," + $TG + "," + $TB + " thr " + $Threshold +
              " runs: " + ($runs -join '  '))
$bmp.Dispose()
