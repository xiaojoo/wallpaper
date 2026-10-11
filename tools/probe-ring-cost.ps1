# tools/probe-ring-cost.ps1 - what each icon on the ring costs the frame, measured off the engine's own
# frame counter rather than off its reported average.
#
# Why the counter and not avg_frame_ms: that field is a rolling mean and it keeps reporting the rate the
# frame was running at before the change, so a downgrade looks like nothing happened. The frames counter
# only ever moves forward, so the difference between two reads a known number of seconds apart IS the
# rate.
#
# The settings window must not be previewing anything while this runs - a live preview is a second
# surface and it would be counted in the same frame budget.
#
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$Exe = 'H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe',
    [int]$Seconds = 12,
    [int[]]$Counts = @(0, 12, 24, 43)
)
$ErrorActionPreference = 'Stop'

function Status() {
    $f = Join-Path $env:TEMP ('ringcost_' + (Get-Random) + '.json')
    Start-Process -FilePath $Exe -ArgumentList '--ctl', 'status' -NoNewWindow -Wait `
                  -RedirectStandardOutput $f 2>$null | Out-Null
    $t = Get-Content $f -Raw; Remove-Item $f -Force
    return $t
}

function Num([string]$json, [string]$key) {
    if ($json -match ('"' + $key + '":\s*(-?[0-9.]+)')) { return [double]$Matches[1] }
    return -1
}

function Ctl([string[]]$a) { $null = & $Exe $a 2>&1 }

Ctl @('--ctl', 'orbit', 'on')
foreach ($c in $Counts) {
    if ($c -eq 0) { Ctl @('--ctl', 'orbit', 'off') } else {
        Ctl @('--ctl', 'orbit', 'on'); Ctl @('--ctl', 'orbitcount', "$c")
    }
    Start-Sleep -Seconds 3                       # let the new loop length settle
    $j1 = Status; $f1 = Num $j1 'frames'; $t1 = Get-Date
    Start-Sleep -Seconds $Seconds
    $j2 = Status; $f2 = Num $j2 'frames'; $t2 = Get-Date
    $dt = ($t2 - $t1).TotalSeconds
    $rate = ($f2 - $f1) / $dt
    $label = if ($c -eq 0) { 'off ' } else { "$c   " }
    Write-Output ("icons {0} counted {1,6:N2} fps over {2:N1} s   avg_frame_ms {3,5:N2}" -f `
                  $label, $rate, $dt, (Num $j2 'avg_frame_ms'))
}
Ctl @('--ctl', 'orbitcount', '12')
Write-Output 'back to 12 icons'
