# Watch the renderer's process for a while: does it die on its own, and what was the last thing it
# logged? The engine restarted four times in five minutes on 2026-10-11 with no error line and no WER
# report, and the client's watchdog gives up after three in ten minutes - so "the desktop is frozen"
# looks like a rendering bug when it is really a process that keeps leaving.
#
# usage: powershell -File tools/probe-renderer-alive.ps1 -Seconds 180
param([int]$Seconds = 180, [int]$Every = 10, [switch]$Start)
$ErrorActionPreference = 'Continue'
$bin = 'H:\wallpaper\bld\bin\RelWithDebInfo'
if ($Start) {
    if (-not (Get-Process WallpaperRenderer -ErrorAction SilentlyContinue)) {
        Start-Process -FilePath ($bin + '\WallpaperRenderer.exe') -WorkingDirectory $bin
        Write-Output 'started one'
        Start-Sleep -Seconds 4
    }
}
$end = (Get-Date).AddSeconds($Seconds)
$last = 0
while ((Get-Date) -lt $end) {
    $p = Get-Process WallpaperRenderer -ErrorAction SilentlyContinue
    if ($p) {
        Write-Output ("{0}  pid {1}  up {2:N0} s" -f (Get-Date -Format HH:mm:ss), $p.Id,
                      ((Get-Date) - $p.StartTime).TotalSeconds)
        if ($p.Id -ne $last) {
            if ($last -ne 0) { Write-Output ("   ^ pid changed from {0}" -f $last) }
            $last = $p.Id
        }
    } else {
        Write-Output ("{0}  NOT RUNNING" -f (Get-Date -Format HH:mm:ss))
    }
    Start-Sleep -Seconds $Every
}
$log = Get-ChildItem (Join-Path $bin 'logs\renderer-*.log') | Sort-Object LastWriteTime | Select-Object -Last 1
Write-Output ("newest log {0}" -f $log.Name)
Get-Content $log.FullName -Tail 6 | ForEach-Object { Write-Output ("   " + $_.Substring(0, [Math]::Min(120, $_.Length))) }
