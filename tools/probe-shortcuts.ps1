# Stage 0 probe (c): can the orbit actually be built out of the Desktop folder's shortcuts?
#
# Three numbers are wanted, none of which is known today:
#   1. how many items the Desktop really holds, split between .lnk files and the shell namespace
#      items (This PC, Recycle Bin) which are NOT files and need a different path to launch;
#   2. of the .lnk files, how many resolve to a target that exists on disk;
#   3. what icon size is actually reachable without dropping to shell COM. ExtractAssociatedIcon is
#      documented to return at most 32x32, which on a 4K desktop at orbit scale would be blurry, so
#      the number that matters is whether SHGetImageList is needed.
#
# Read-only. Nothing here creates, hides, or launches anything.

$ErrorActionPreference = 'Stop'

# Both the user's and the public desktop: the shell shows the union of the two.
$user = [Environment]::GetFolderPath('Desktop')
$pub  = [Environment]::GetFolderPath('CommonDesktopDirectory')
Write-Output ("desktop dirs: user=" + $user + "  public=" + $pub)

$items = @()
foreach ($d in @($user, $pub)) {
    if (Test-Path $d) {
        $items += Get-ChildItem -LiteralPath $d -Force -File -ErrorAction SilentlyContinue
        $items += Get-ChildItem -LiteralPath $d -Force -Directory -ErrorAction SilentlyContinue
    }
}

$lnk   = @($items | Where-Object { $_.Extension -eq '.lnk' })
$dirs  = @($items | Where-Object { $_.PSIsContainer })
$other = @($items | Where-Object { -not $_.PSIsContainer -and $_.Extension -ne '.lnk' })
Write-Output ("files/dirs on the desktop: lnk=" + $lnk.Count + "  dirs=" + $dirs.Count + "  other=" + $other.Count)

Add-Type -AssemblyName System.Drawing
$shell = New-Object -ComObject WScript.Shell

$resolved = 0; $missing = 0; $empty = 0; $args_count = 0
$widths = @{}
foreach ($f in $lnk) {
    try {
        $sc = $shell.CreateShortcut($f.FullName)
        $t = $sc.TargetPath
        if ([string]::IsNullOrEmpty($t)) { $empty++ ; continue }
        if (-not [string]::IsNullOrEmpty($sc.Arguments)) { $args_count++ }
        if (Test-Path -LiteralPath $t) { $resolved++ } else { $missing++ }
    } catch {
        $empty++
    }
}
Write-Output ("target resolution of " + $lnk.Count + " .lnk: exists=" + $resolved +
              "  missing=" + $missing + "  blank-target=" + $empty + "  with-args=" + $args_count)

# Icon reachability without shell COM.
try {
    $probe = $lnk | Select-Object -First 8
    foreach ($f in $probe) {
        $ic = [System.Drawing.Icon]::ExtractAssociatedIcon($f.FullName)
        $key = [string]$ic.Width + "x" + [string]$ic.Height
        if ($widths.ContainsKey($key)) { $widths[$key] = $widths[$key] + 1 } else { $widths[$key] = 1 }
        $ic.Dispose()
    }
} catch {
    Write-Output ("icon probe failed: " + $_.Exception.Message)
}
Write-Output ("ExtractAssociatedIcon sizes seen: " + (($widths.GetEnumerator() | ForEach-Object { $_.Key + "=" + $_.Value }) -join " "))

# What the shell itself reports, as the cross-check for the namespace items.
$ns = New-Object -ComObject Shell.Application
$desk = $ns.NameSpace(0)   # ssDesktop
$names = @()
foreach ($it in $desk.Items()) { $names += $it.Name }
Write-Output ("shell namespace desktop item count: " + $names.Count + "  (the union the user actually sees)")
Write-Output ("first 12 names: " + (($names | Select-Object -First 12) -join " | "))

$res = (Get-WmiObject Win32_VideoController | ForEach-Object { [string]$_.CurrentHorizontalResolution + "x" + [string]$_.CurrentVerticalResolution }) -join ", "
Write-Output ("screen: " + $res)
