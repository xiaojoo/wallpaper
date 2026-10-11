# tools/probe-crash-selftest.ps1 - prove the crash reporter fires, and show what it writes.
#
# A handler nobody has seen write its file is a guess, so the engine carries a `--selfcrash` switch that
# writes through a null pointer one second after startup - the same kind of fault the real one is. It is
# the only honest way to test this: injecting the fault through a debugger (cdb `call`, then pointing rip
# at 0) never reached the process's own filter, and a debugger attached is itself the case the reporter
# has to survive, because SEM_NOGPFAULTERRORBOX is what stops Windows writing any fault report at all.
#
# The instance runs with --window --no-ipc, so it takes nothing over: no wallpaper on the desktop, no
# control pipe. It does need the session mutex, so the running engine is stopped first and brought back
# by the settings window's watchdog afterwards.
#
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$Bin = 'H:\wallpaper\bld\bin\RelWithDebInfo',
    [int]$Wait = 25
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Bin 'WallpaperRenderer.exe'
$logDir = Join-Path $Bin 'logs'

$before = @(Get-ChildItem $logDir -Filter 'crash-*.txt' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
Get-Process WallpaperRenderer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

$e = Start-Process -FilePath $exe -ArgumentList '--window', '--no-ipc', '--selfcrash' `
     -WorkingDirectory $Bin -PassThru
Write-Output ("throwaway engine pid {0}" -f $e.Id)
$end = (Get-Date).AddSeconds($Wait)
while ((Get-Date) -lt $end -and -not $e.HasExited) { Start-Sleep -Milliseconds 250 }
if (-not $e.HasExited) { Write-Output 'engine did not fault'; $e | Stop-Process -Force; exit 1 }
Write-Output ("exit code 0x{0:X8}" -f [int]$e.ExitCode)

Start-Sleep -Seconds 1
$new = @(Get-ChildItem $logDir -Filter 'crash-*.txt' -ErrorAction SilentlyContinue |
         Where-Object { $before -notcontains $_.Name })
if ($new.Count -eq 0) { Write-Output 'NO CRASH FILE - the reporter did not run'; exit 1 }
foreach ($f in $new) {
    Write-Output ("WROTE {0} ({1} bytes)" -f $f.Name, $f.Length)
    Get-Content $f.FullName | ForEach-Object { Write-Output ('  ' + $_) }
}
