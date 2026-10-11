# How many WallpaperRenderer processes are alive, and since when? Two at once is a defect, not a
# detail: each holds its own snapshot of config.json, the pipe is served by whichever created it first,
# and the file is written by whichever saves last - so a setting can be answered one way by `status`
# and persisted the other way.
#
#   -Kill    stop every instance instead of just listing them
#
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param([switch]$Kill)
$p = @(Get-Process WallpaperRenderer -ErrorAction SilentlyContinue)
Write-Output ("instances: {0}" -f $p.Count)
foreach ($x in $p) {
    Write-Output ("  pid {0}  started {1}" -f $x.Id, $x.StartTime.ToString("HH:mm:ss"))
}
if ($Kill -and $p.Count -gt 0) {
    $p | Stop-Process -Force
    Start-Sleep -Seconds 4
    Write-Output ("after kill: {0}" -f @(Get-Process WallpaperRenderer -ErrorAction SilentlyContinue).Count)
}
