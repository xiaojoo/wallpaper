# Clicks an empty spot on the desktop (the way he does) and samples the renderer's decision
# while the desktop is the foreground window, then gives focus back to whatever had it.
param([string]$Ctl = 'H:\wallpaper\bld\bin\RelWithDebInfo\WallpaperRenderer.exe',
       [int]$X = 3000, [int]$Y = 800, [int]$Samples = 5)

$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class C
{
    [StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SwitchToThisWindow(IntPtr h, bool alt);
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern int GetWindowLongW(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out PT p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint ms, out IntPtr res);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);

    public static IntPtr ListView()
    {
        IntPtr def = FindWindowExW(GetShellWindow(), IntPtr.Zero, "SHELLDLL_DefView", null);
        return FindWindowExW(def, IntPtr.Zero, "SysListView32", "FolderView");
    }
    public static void Menu(IntPtr h, int x, int y)
    {
        IntPtr lp = (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF));
        IntPtr res;
        SendMessageTimeoutW(h, 0x007B /*WM_CONTEXTMENU*/, h, lp, 0x0002 /*ABORTIFHUNG*/, 250, out res);
    }
    public static void Cancel(IntPtr h) { PostMessageW(h, 0x001F /*WM_CANCELMODE*/, IntPtr.Zero, IntPtr.Zero); }
    public static void F5(IntPtr h)
    {
        PostMessageW(h, 0x0100 /*WM_KEYDOWN*/, (IntPtr)0x74 /*VK_F5*/, IntPtr.Zero);
        PostMessageW(h, 0x0101 /*WM_KEYUP*/, (IntPtr)0x74, IntPtr.Zero);
    }

    public static string Cls(IntPtr h)
    {
        if (h == IntPtr.Zero) return "-";
        StringBuilder sb = new StringBuilder(128);
        GetClassNameW(h, sb, 128);
        return sb.ToString();
    }
    public static int Style(IntPtr h) { return h == IntPtr.Zero ? 0 : GetWindowLongW(h, -16); }
    public static void Click(int x, int y)
    {
        PT before; GetCursorPos(out before);
        // The input events are queued, so the cursor must stay put until they have been delivered:
        // restoring it right after mouse_event made the click land back on the old position.
        SetCursorPos(x, y);
        Start();
        mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);   // LEFTDOWN
        Start();
        mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);   // LEFTUP
        Start();
        SetCursorPos(before.X, before.Y);
    }
    static void Start() { System.Threading.Thread.Sleep(120); }
}
'@

Add-Type -TypeDefinition $code

$prev = [C]::GetForegroundWindow()
Write-Output ("focus was: " + [C]::Cls($prev))
[C]::Click($X, $Y)
Start-Sleep -Milliseconds 350
$fg = [C]::GetForegroundWindow()
$line = "foreground now: " + [C]::Cls($fg) + " style=0x" + ([C]::Style($fg)).ToString("X8")
$line = $line + "  progman=0x" + ([C]::GetShellWindow()).ToString("X")
Write-Output $line

function Snap([string]$tag) {
    $j = & $Ctl --ctl status 2>$null | Out-String
    $state = if ($j -match '"state":\s*"([^"]+)"') { $matches[1] } else { '?' }
    $cap   = if ($j -match '"cap_fps":\s*([0-9\-]+)') { $matches[1] } else { '?' }
    $fps   = if ($j -match '"measured_fps":\s*"([0-9.]+)"') { $matches[1] } else { '?' }
    $why   = if ($j -match '"state_reason":\s*"([^"]*)"') { $matches[1] } else { '?' }
    Write-Output ("{0,-14} state={1,-10} cap={2,-3} fps={3,-6} {4}" -f $tag, $state, $cap, $fps, $why)
}

for ($i = 0; $i -lt $Samples; $i++) { Snap "click$i"; Start-Sleep -Milliseconds 400 }

# The two operations he named, with the desktop already in front.
$lv = [C]::ListView()
[C]::Menu($lv, 900, 500)
Start-Sleep -Milliseconds 400
Snap 'menu-open'
for ($i = 0; $i -lt 2; $i++) { Snap "menu$i"; Start-Sleep -Milliseconds 400 }
[C]::Cancel($lv)
Start-Sleep -Milliseconds 300

[C]::F5($lv)
Snap 'after-f5'
for ($i = 0; $i -lt 2; $i++) { Snap "f5$i"; Start-Sleep -Milliseconds 400 }

if ($prev -ne [IntPtr]::Zero) { [C]::SwitchToThisWindow($prev, $true) }
Write-Output ("focus back to: " + [C]::Cls([C]::GetForegroundWindow()))
