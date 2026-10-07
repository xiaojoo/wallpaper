# tools/wer-report.ps1 - print the recent Application-log errors for our processes.
# Kept ASCII: PowerShell 5.1 reads .ps1 as GBK and non-ASCII breaks parsing.
param(
    [int]$Minutes = 30,
    [string]$Match = 'Wallpaper'
)
$since = (Get-Date).AddMinutes(-$Minutes)
$events = Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $since } -ErrorAction SilentlyContinue
foreach ($e in $events) {
    if ($e.Message -notmatch $Match) { continue }
    $text = $e.Message
    if ($text.Length -gt 500) { $text = $text.Substring(0, 500) }
    Write-Output ("[{0}] id={1}" -f $e.TimeCreated, $e.Id)
    Write-Output $text
    Write-Output "----"
}
