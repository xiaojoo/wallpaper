param([int]$Minutes = 40)
$start = (Get-Date).AddMinutes(-$Minutes)
$hits = Get-WinEvent -FilterHashtable @{LogName='Application'; StartTime=$start} -ErrorAction SilentlyContinue |
    Where-Object { $_.Message -match 'WallpaperRenderer|Wallpaper' }
"matches: $(@($hits).Count) in the last $Minutes min"
foreach ($e in ($hits | Select-Object -First 8)) {
    $first = ($e.Message -split "`r?`n" | Select-Object -First 5) -join ' | '
    "{0}  id={1}  {2}  {3}" -f $e.TimeCreated.ToString('HH:mm:ss'), $e.Id, $e.LevelDisplayName, $first.Substring(0, [Math]::Min(180, $first.Length))
}
"--- renderer processes now: $(@(Get-Process WallpaperRenderer -ErrorAction SilentlyContinue).Count) ---"
"--- WER file reports ---"
Get-ChildItem "$env:ProgramData\Microsoft\Windows\WER\ReportArchive","$env:ProgramData\Microsoft\Windows\WER\ReportQueue" -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -gt $start } | Select-Object -First 6 Name,LastWriteTime | Format-Table -AutoSize
