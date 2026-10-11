# Why did WallpaperRenderer stop? Application-log events and WER reports from the last N minutes.
param([int]$Minutes = 25)
$ErrorActionPreference = 'Continue'
$start = (Get-Date).AddMinutes(-$Minutes)
$ev = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $start } -ErrorAction SilentlyContinue
$hits = @($ev | Where-Object { $_.Message -match 'WallpaperRenderer' })
Write-Output ("application-log events naming WallpaperRenderer: {0}" -f $hits.Count)
foreach ($e in ($hits | Select-Object -First 6)) {
    $m = $e.Message -replace '\s+', ' '
    if ($m.Length -gt 240) { $m = $m.Substring(0, 240) }
    Write-Output ("  {0}  id={1}  {2}" -f $e.TimeCreated, $e.Id, $e.ProviderName)
    Write-Output ("      {0}" -f $m)
}
$wer = Get-ChildItem "$env:LOCALAPPDATA\CrashDumps" -Filter 'WallpaperRenderer*' -ErrorAction SilentlyContinue
Write-Output ("crash dumps: {0}" -f @($wer).Count)
foreach ($f in $wer) { Write-Output ("  {0}  {1}" -f $f.LastWriteTime, $f.Name) }
