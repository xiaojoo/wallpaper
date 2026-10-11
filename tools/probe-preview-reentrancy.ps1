# tools/probe-preview-reentrancy.ps1 - hammer the live preview the way the settings window's carousel
# does, and say whether the engine survives.
#
# The 22:32:48 access violation had StartPreview -> WarmWhileDrawing -> DrainMessages -> WndProc ->
# ProcessQueued -> UpdateSnapshot in its stack: the warm-up pumps messages, and a command that arrives
# during that pump runs on top of the half-built preview. The window walks its hero carousel onto a
# different clip every few seconds, so the overlap is normal there; this makes it dense on purpose.
#
# Each round fires four preview commands at once (detached, so they queue within the same warm-up) and
# then asks the engine who it is. A changed pid or a new crash-<pid>.txt is a crash.
#
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param(
    [string]$Bin = 'H:\wallpaper\bld\bin\RelWithDebInfo',
    [string]$Ids = 'local_02,local_03,local_05',
    [int]$Rounds = 40,
    [int]$GapMs = 250
)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $Bin 'WallpaperRenderer.exe'
$logDir = Join-Path $Bin 'logs'
$ids = $Ids.Split(',')

function StatusField([string]$key) {
    $f = Join-Path $env:TEMP ('reent_' + (Get-Random) + '.json')
    Start-Process -FilePath $exe -ArgumentList '--ctl', 'status' -NoNewWindow -Wait `
                  -RedirectStandardOutput $f 2>$null | Out-Null
    $t = Get-Content $f -Raw; Remove-Item $f -Force
    if ($t -match ('"' + $key + '":\s*"?(-?[0-9]+)')) { return $Matches[1] }
    return ''
}

function Fire([string[]]$a) { Start-Process -FilePath $exe -ArgumentList $a -WindowStyle Hidden | Out-Null }

if (-not @(Get-Process WallpaperRenderer -ErrorAction SilentlyContinue)) {
    Start-Process -FilePath $exe -WorkingDirectory $Bin | Out-Null
    Start-Sleep -Seconds 10
}
$pid0 = StatusField 'pid'
$crashes0 = @(Get-ChildItem $logDir -Filter 'crash-*.txt' -ErrorAction SilentlyContinue).Count
Write-Output ("engine pid {0}, {1} crash files on record" -f $pid0, $crashes0)

# Each round: drop the pass (that reaps the video instances, so the next open really warms), start a
# clip - which takes 50-150 ms of worker time while the presenting thread pumps messages - and then,
# partway through that window, fire two more commands so they arrive on the pump rather than joining
# the batch the outer command already took. Firing them all at once proves nothing: they get swapped
# out of the queue together and run one after another without nesting.
$lost = 0
for ($r = 1; $r -le $Rounds; $r++) {
    Fire @('--ctl', 'previewoff')
    Start-Sleep -Milliseconds 80
    Fire @('--ctl', 'preview', $ids[1])
    Start-Sleep -Milliseconds 45
    Fire @('--ctl', 'preview', $ids[2 % $ids.Count])
    Fire @('--ctl', 'orbit', 'on')
    Start-Sleep -Milliseconds $GapMs
    $now = StatusField 'pid'
    if ($now -ne $pid0) {
        Write-Output ("round {0}: pid went {1} -> {2} - the engine died" -f $r, $pid0, $now)
        $lost = $r
        break
    }
}
$crashes1 = @(Get-ChildItem $logDir -Filter 'crash-*.txt' -ErrorAction SilentlyContinue)
Write-Output ("rounds {0}, last alive pid {1}, new crash files: {2}" -f $Rounds, $pid0,
              ($crashes1.Count - $crashes0))
foreach ($f in $crashes1 | Select-Object -Last 2) {
    Write-Output ("  {0}: {1}" -f $f.Name, ((Get-Content $f.FullName -TotalCount 3)[1]).Trim())
}
if ($lost) { exit 2 }
