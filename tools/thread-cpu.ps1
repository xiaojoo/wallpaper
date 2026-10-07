# Per-thread CPU of the renderer: which threads are burning the 400%+.
# ASCII only on purpose: PowerShell 5.1 reads .ps1 as GBK and a Chinese comment can break parsing.
param([string]$Name = 'WallpaperRenderer')

$p = Get-Process -Name $Name -ErrorAction SilentlyContinue
if (-not $p) { Write-Output "no process named $Name"; exit 1 }

$pid1 = $p.Id
Write-Output ("pid {0}  ws_mb {1:N0}  threads {2}  start {3}" -f $pid1, ($p.WorkingSet64/1MB), $p.Threads.Count, $p.StartTime)

# Snapshot, wait, snapshot: the delta is the only honest per-thread rate.
$a = @{}
foreach ($t in $p.Threads) { $a[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds }
Start-Sleep -Seconds 5
$p2 = Get-Process -Id $pid1
$b = @{}
foreach ($t in $p2.Threads) { $b[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds }

$rows = @()
foreach ($k in $b.Keys) {
    if (-not $a.ContainsKey($k)) { continue }
    $ms = $b[$k] - $a[$k]
    $rows += [pscustomobject]@{ Tid = $k; PercentOfOneCore = [math]::Round($ms / 5000.0 * 100.0, 1); State = $p2.Threads | Where-Object { $_.Id -eq $k } | ForEach-Object { $_.ThreadState } }
}
$rows | Sort-Object -Descending PercentOfOneCore | Select-Object -First 14 | ForEach-Object {
    Write-Output ("  tid {0,-8} {1,7} % of one core   {2}" -f $_.Tid, $_.PercentOfOneCore, $_.State)
}
$sum = ($rows | Measure-Object -Property PercentOfOneCore -Sum).Sum
Write-Output ("  threads accounted: {0}   sum {1:N1} % of one core" -f $rows.Count, $sum)
Write-Output ("  ws_mb now {0:N0}" -f ($p2.WorkingSet64/1MB))
