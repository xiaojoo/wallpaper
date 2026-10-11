# tools/probe-orbit-fade.ps1 - desktop grabs around the ring's own toggle, for the depth fade.
#
# The ring must be frozen first (--ctl orbitspeed 0), because the question is "what does one icon's own
# pixel cost at this depth", and a moving icon answers a different question.
#
# Each pair is one ring-on frame and the ring-off frame that follows it, written to
# tools/tmp/fade_on<i>.png / fade_off<i>.png; tools/probe-orbit-fade.py does the arithmetic. The off
# frame is the background the icon was composited over, so their difference is the icon's own
# contribution and nothing else on the desktop's.
#
# ASCII only: PowerShell 5.1 parses this file under a non-UTF8 codepage.
param(
    [string]$Exe = 'H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe',
    [int]$X = 1500, [int]$Y = 440, [int]$W = 1560, [int]$H = 900,
    [string]$Dir = 'H:\wallpaper\tools\tmp',
    [int]$Pairs = 3
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$src = @'
using System;
using System.Runtime.InteropServices;

public static class VM
{
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);
    public static int VX { get { return GetSystemMetrics(76); } }
    public static int VY { get { return GetSystemMetrics(77); } }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

function Grab([string]$name) {
    $t = Get-Date
    $bmp = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size $W, $H),
                      ([System.Drawing.CopyPixelOperation]::SourceCopy))
    $g.Dispose()
    $out = Join-Path $Dir $name
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output ("{0,-16} ms {1}" -f $name, [int]($t.TimeOfDay.TotalMilliseconds))
}

function Ctl([string[]]$args2) {
    & $Exe $args2 2>&1 | Out-Null
}

# The pipe queues commands, so the reply says "queued" and not "done": a capture taken right after the
# toggle still shows the OLD state. Its first run did exactly that - the middle frame turned out to be
# a second ring-on frame, which is a fine noise control but not a measurement. So wait for status to
# read back the state asked for before grabbing.
#
# Two traps on the way here: a bare `& $Exe --ctl status` inside a function captures nothing at all
# (the redirect has to go through Start-Process to a file), and the status JSON is pretty-printed, so
# the key reads `"orbit": false` with a space - a pattern without one never matches and the wait
# reports "never applied" for a toggle that worked.
function OrbitState() {
    $f = Join-Path $env:TEMP ('orbitfade_status_' + (Get-Random) + '.json')
    Start-Process -FilePath $Exe -ArgumentList '--ctl', 'status' -NoNewWindow -Wait `
                  -RedirectStandardOutput $f 2>$null | Out-Null
    if (-not (Test-Path $f)) { return '?' }
    $t = Get-Content $f -Raw
    Remove-Item $f -Force
    if ($t -match '"orbit":\s*(true|false)') { return $Matches[1] }
    return '?'
}

function CtlWait([string]$verb, [string]$want) {
    $t = Get-Date
    Ctl @('--ctl', $verb, $want)
    $aim = if ($want -eq 'off') { 'false' } else { 'true' }
    for ($i = 0; $i -lt 40; $i++) {
        if ((OrbitState) -eq $aim) {
            Write-Output ("{0,-16} applied in {1} ms" -f "$verb $want",
                          [int](((Get-Date) - $t).TotalMilliseconds))
            return
        }
        Start-Sleep -Milliseconds 25
    }
    Write-Output ("{0,-16} NEVER APPLIED (status still {1} after 2 s)" -f "$verb $want", (OrbitState))
}

if (-not (Test-Path $Dir)) { New-Item -ItemType Directory -Path $Dir | Out-Null }
Write-Output ("virtual origin {0},{1} - crop absolute {2},{3} {4}x{5}" -f [VM]::VX, [VM]::VY, $X, $Y, $W, $H)
# Several pairs rather than one: the queue drains about once a second, so an on/off pair is always a
# second apart, and the wallpaper underneath is a clock whose digits change on that scale. Averaging
# the pairs is what turns "the back icon reads -2.0 in one grab" into a number with an error bar.
for ($i = 1; $i -le $Pairs; $i++) {
    CtlWait 'orbit' 'on'
    Grab ("fade_on{0}.png" -f $i)
    CtlWait 'orbit' 'off'
    Grab ("fade_off{0}.png" -f $i)
}
CtlWait 'orbit' 'on'
Write-Output 'done'
