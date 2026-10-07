$ErrorActionPreference = 'Continue'
$pid_ = [int]$args[0]
$log  = $args[1]
$cdb  = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
if (-not (Test-Path $cdb)) { "cdb not found at $cdb"; exit 3 }
"attaching to $pid_ ; log $log"
# g blocks until the debuggee breaks; the commands after it then run on that break and quit+detach.
& $cdb -p $pid_ -logo $log -c 'g;.ecxr;r;k;kP 30;.cxr;k;lm m nvwgf2umx,ucrtbase,mfcore,mfreadwrite,qd'
"cdb returned: $LASTEXITCODE"
