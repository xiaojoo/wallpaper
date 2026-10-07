$p1 = Get-Process WallpaperRenderer -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p1) { 'no renderer'; exit 2 }
$c1 = $p1.CPU; $w1 = $p1.WorkingSet64
Start-Sleep 6
$p2 = Get-Process WallpaperRenderer -ErrorAction SilentlyContinue | Select-Object -First 1
$c2 = $p2.CPU; $w2 = $p2.WorkingSet64
$cores = [Environment]::ProcessorCount
$sec = ($c2 - $c1)
'thread-seconds used in 6 s wall: {0:N2}  = {1:N1}% of one core  = {2:N1}% of all {3} cores' -f $sec, ($sec/6*100), ($sec/6/$cores*100), $cores
'working set: {0:N1} -> {1:N1} MB   private: {2:N1} MB' -f ($w1/1MB), ($w2/1MB), ($p2.PrivateMemorySize64/1MB)
'pid {0}  start {1:HH:mm:ss}' -f $p2.Id, $p2.StartTime
