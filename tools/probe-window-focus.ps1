param([string]$Action = 'status', [string]$Title = 'Wallpaper', [int]$Id = 0)
$sig = @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class FW {
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
}
"@
Add-Type -TypeDefinition $sig | Out-Null
if ($Id -gt 0) {
    # A handle from the process beats matching its title: notepad's title is localised, and a
    # non-ASCII argument through -File here arrived as mojibake.
    $h = (Get-Process -Id $Id -ErrorAction SilentlyContinue).MainWindowHandle
} else {
    $sb = New-Object System.Text.StringBuilder 300
    $h = [IntPtr]::Zero
    [FW]::EnumWindows({
        param($w, $l)
        [FW]::GetWindowTextW($w, $sb, 300) | Out-Null
        if ($sb.ToString() -eq $Title -and [FW]::IsWindowVisible($w)) { $script:found = $w; return $false }
        return $true
    }, [IntPtr]::Zero) | Out-Null
    $h = $script:found
}
if ($h -eq $null -or $h -eq [IntPtr]::Zero) { "no window found"; exit 2 }
switch ($Action) {
    'min'     { [FW]::ShowWindow($h, 6) | Out-Null }      # SW_MINIMIZE
    'restore' { [FW]::ShowWindow($h, 9) | Out-Null }      # SW_RESTORE
    'focus'   { "set_fg=$([FW]::SetForegroundWindow($h))" }
    'console' { $c = [FW]::GetConsoleWindow(); "console=0x$($c.ToString('X')) set_fg=$([FW]::SetForegroundWindow($c))" }
}
$fg = [FW]::GetForegroundWindow()
"handle=0x$($h.ToString('X')) iconic=$([FW]::IsIconic($h)) visible=$([FW]::IsWindowVisible($h)) foreground=0x$($fg.ToString('X')) fg_same=$($fg -eq $h)"
