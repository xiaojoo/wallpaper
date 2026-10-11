# tools/probe-engine-exitcode.ps1 - what code does the renderer leave with, and does a live preview
# bring it on?
#
# The engine's log ends mid-line with no "final:" farewell and Windows Error Reporting writes nothing,
# which rules out both a clean stop and a caught crash. The one fact that separates "it decided to
# leave" from "it was killed by its own bad pointer" is the process exit code, and the only way to read
# it is to be the parent that started it.
#
#   -Preview     hold a live preview open the way the settings window does (preview + previewbeat)
#   -Seconds     how long to watch
#
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$Bin = 'H:\wallpaper\bld\bin\RelWithDebInfo',
    [int]$Seconds = 180,
    [switch]$Preview,
    [string]$PreviewId = ''
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Bin 'WallpaperRenderer.exe'

function Ctl([string[]]$a) { $null = & $exe $a 2>&1 }

if ($Preview) {
    if ($PreviewId -eq '') {
        # Whatever the running engine has in its catalog, so this reproduces the window's own choice
        # rather than an id made up here.
        $f = Join-Path $env:TEMP 'exitcode_status.json'
        Start-Process -FilePath $exe -ArgumentList '--ctl','status' -NoNewWindow -Wait `
                      -RedirectStandardOutput $f 2>$null | Out-Null
        $t = Get-Content $f -Raw; Remove-Item $f -Force
        if ($t -match '"id":\s*"([^"]+)"') { $PreviewId = $Matches[1] }
        Write-Output ("preview id from the catalog: {0}" -f $PreviewId)
    }
}

$p = Start-Process -FilePath $exe -WorkingDirectory $Bin -PassThru
Write-Output ("started pid {0}" -f $p.Id)
$end = (Get-Date).AddSeconds($Seconds)
$beat = Get-Date
while ((Get-Date) -lt $end) {
    $p.Refresh()
    if ($p.HasExited) { break }
    if ($Preview -and ((Get-Date) - $beat).TotalSeconds -ge 2) {
        Ctl @('--ctl', 'preview', $PreviewId)
        Ctl @('--ctl', 'previewbeat')
        $beat = Get-Date
    }
    Start-Sleep -Milliseconds 500
}
$up = ((Get-Date) - $p.StartTime).TotalSeconds
if (-not $p.HasExited) {
    Write-Output ("pid {0} still alive after {1:N0} s - no exit to read" -f $p.Id, $up)
    exit 0
}
$code = $p.ExitCode
Write-Output ("pid {0} exited after {1:N0} s, exit code 0x{2:X8} ({2})" -f $p.Id, $up, $code)
switch ($code) {
    0            { Write-Output '  -> clean exit: something asked for it' }
    1            { Write-Output '  -> main returned 1 (the engine could not start)' }
    0xC0000005   { Write-Output '  -> ACCESS_VIOLATION' }
    0xC0000409   { Write-Output '  -> FASTFAIL (stack guard / heap corruption / abort)' }
    0xC0000374   { Write-Output '  -> HEAP_CORRUPTION' }
    0x40010004   { Write-Output '  -> the process was terminated by a debugger thread' }
    default      { if ($code -lt 0) { Write-Output '  -> an NTSTATUS exception code' } }
}
