# Watches the renderer's power decision while the desktop is interacted with.
# Opens a real desktop context menu, then activates Progman the way a desktop click does,
# and restores the window that had focus before. ASCII only: PS 5.1 reads .ps1 as GBK.
param(
    [int]$Samples = 6,
    [int]$IntervalMs = 300,
    [string]$Ctl = 'H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe'
)

$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class W
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string n);
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SwitchToThisWindow(IntPtr h, bool alt);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int idx);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint ms, out IntPtr res);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rt, B; }

    public static string Cls(IntPtr h)
    {
        if (h == IntPtr.Zero) return "-";
        StringBuilder sb = new StringBuilder(128);
        GetClassNameW(h, sb, 128);
        return sb.ToString();
    }
    public static string Rect(IntPtr h)
    {
        R r; if (h == IntPtr.Zero || !GetWindowRect(h, out r)) return "-";
        return r.L + "," + r.T + " " + (r.Rt - r.L) + "x" + (r.B - r.T);
    }
    public static int Style(IntPtr h) { return h == IntPtr.Zero ? 0 : GetWindowLongW(h, -16); }
    public static int ExStyle(IntPtr h) { return h == IntPtr.Zero ? 0 : GetWindowLongW(h, -20); }

    public const int WS_MAXIMIZE = 0x1000000;
    public const int WS_CHILD = 0x40000000;
    public const int WS_EX_TOOLWINDOW = 0x80;
    public const int WS_EX_NOACTIVATE = 0x8000000;
    public const int WS_EX_TRANSPARENT = 0x20;

    public static string Describe(string tag, IntPtr h)
    {
        int st = Style(h), ex = ExStyle(h);
        return tag + " " + Cls(h) + " " + Rect(h)
            + " style=0x" + st.ToString("X8") + " ex=0x" + ex.ToString("X8")
            + (h != IntPtr.Zero && (st & WS_MAXIMIZE) != 0 ? " MAXIMIZED" : "")
            + ((ex & WS_EX_TOOLWINDOW) != 0 ? " TOOLWINDOW" : "")
            + ((ex & WS_EX_NOACTIVATE) != 0 ? " NOACTIVATE" : "");
    }

    public static IntPtr Progman() { return GetShellWindow(); }
    public static IntPtr DefView() { return FindWindowExW(Progman(), IntPtr.Zero, "SHELLDLL_DefView", null); }
    public static IntPtr ListView() { return FindWindowExW(DefView(), IntPtr.Zero, "SysListView32", "FolderView"); }

    public static void Menu(IntPtr h, int x, int y)
    {
        IntPtr lp = (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF));
        IntPtr res;
        SendMessageTimeoutW(h, 0x007B /*WM_CONTEXTMENU*/, h, lp, 0x0002 /*ABORTIFHUNG*/, 250, out res);
    }
    public static void Cancel(IntPtr h) { PostMessageW(h, 0x001F /*WM_CANCELMODE*/, IntPtr.Zero, IntPtr.Zero); }
}
'@

Add-Type -TypeDefinition $code

function Snap([string]$tag) {
    $j = & $Ctl --ctl status 2>$null | Out-String
    $state = if ($j -match '"state":\s*"([^"]+)"') { $matches[1] } else { '?' }
    $cap   = if ($j -match '"cap_fps":\s*([0-9\-]+)') { $matches[1] } else { '?' }
    $fps   = if ($j -match '"measured_fps":\s*"([0-9.]+)"') { $matches[1] } else { '?' }
    $why   = if ($j -match '"state_reason":\s*"([^"]*)"') { $matches[1] } else { '?' }
    $fg    = [W]::Describe('fg=', [W]::GetForegroundWindow())
    Write-Output ("{0,-14} state={1,-10} cap={2,-4} fps={3,-6} {4} | {5}" -f $tag, $state, $cap, $fps, $why, $fg)
}

Write-Output ([W]::Describe('PROGMAN   ', [W]::Progman()))
Write-Output ([W]::Describe('DEFVIEW   ', [W]::DefView()))
Write-Output ([W]::Describe('LISTVIEW  ', [W]::ListView()))
Write-Output ''

Snap 'before'
for ($i = 0; $i -lt $Samples; $i++) { Start-Sleep -Milliseconds $IntervalMs; Snap "idle$i" }

$lv = [W]::ListView()
[W]::Menu($lv, 900, 500)
Start-Sleep -Milliseconds 250
$menuFg = [W]::GetForegroundWindow()
Snap 'menu-open'
for ($i = 0; $i -lt 4; $i++) { Start-Sleep -Milliseconds $IntervalMs; Snap "menu$i" }
[W]::Cancel($lv)
Start-Sleep -Milliseconds 400
Snap 'menu-closed'

# Now the desktop-click case: make Progman the foreground window, then give focus back.
$prev = [W]::GetForegroundWindow()
[W]::SwitchToThisWindow([W]::Progman(), $true)
Start-Sleep -Milliseconds 300
Snap 'desktop-fg'
for ($i = 0; $i -lt 4; $i++) { Start-Sleep -Milliseconds $IntervalMs; Snap "desk$i" }
if ($prev -ne [IntPtr]::Zero) { [W]::SwitchToThisWindow($prev, $true) }
Start-Sleep -Milliseconds 300
Snap 'restored'
