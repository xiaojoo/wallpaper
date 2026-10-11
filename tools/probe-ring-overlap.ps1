# tools/probe-ring-overlap.ps1 - freeze the ring at a chosen pair of sizes and grab the crop, so the
# overlapping icons can be looked at one at a time instead of in a moving picture.
#
# Prints the sizes it read back from the renderer (not the ones it asked for) and restores the ring's
# period afterwards, because the period is the state the user was actually looking at.
#
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$Exe = 'H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe',
    [int]$Ring = 50, [int]$Icon = 186,
    [int]$X = 1500, [int]$Y = 440, [int]$W = 1560, [int]$H = 900,
    [string]$Out = 'H:\wallpaper\tools\tmp\overlap.png'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Ctl([string[]]$a) { $null = & $Exe $a 2>&1 }

function State([string]$key, [string]$want) {
    for ($i = 0; $i -lt 40; $i++) {
        $f = Join-Path $env:TEMP ('ringov_' + (Get-Random) + '.json')
        Start-Process -FilePath $Exe -ArgumentList '--ctl', 'status' -NoNewWindow -Wait `
                      -RedirectStandardOutput $f 2>$null | Out-Null
        $t = Get-Content $f -Raw; Remove-Item $f -Force
        if ($t -match ('"' + $key + '":\s*' + $want)) { return $true }
        Start-Sleep -Milliseconds 25
    }
    return $false
}

Ctl @('--ctl', 'orbit', 'on'); $null = State 'orbit' 'true'
Ctl @('--ctl', 'orbitspeed', '0'); $null = State 'orbit_period_s' '0'
Ctl @('--ctl', 'orbitscale', "$Ring"); $null = State 'orbit_scale' "$Ring"
Ctl @('--ctl', 'orbiticonscale', "$Icon"); $null = State 'orbit_icon_scale' "$Icon"
Start-Sleep -Milliseconds 300
$bmp = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H),
                  ([System.Drawing.CopyPixelOperation]::SourceCopy))
$g.Dispose(); $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
Write-Output ("grabbed ring=$Ring icon=$Icon -> $Out")
