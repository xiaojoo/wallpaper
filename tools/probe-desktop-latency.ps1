# Times how long each desktop window's owning thread takes to answer a trivial message.
# A right-click or F5 on the desktop makes Explorer touch every window in Progman's child list,
# so if our wallpaper window answers slowly, the desktop's own input path waits for us.
# ASCII only: PowerShell 5.1 reads .ps1 as GBK and non-ASCII silently breaks parsing.
param(
    [int]$Samples = 20,
    [int]$IntervalMs = 400,
    [int]$ClickAtSample = -1,
    [string]$Label = 'run',
    [string]$OutFile = 'H:\wallpaper\build\probe-latency.csv'
)

$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class Lat
{
    public delegate bool EnumProc(IntPtr h, IntPtr l);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool SendMessageTimeoutW(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint ms, out IntPtr res);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr data);
    [DllImport("kernel32.dll")]
    public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")]
    public static extern IntPtr GetShellWindow();

    const uint WM_NULL = 0x0000;
    const uint WM_CONTEXTMENU = 0x007B;
    const uint WM_CANCELMODE = 0x001F;
    const uint SMTO_ABORTIFHUNG = 0x0002;
    const uint SMTO_NORMAL = 0x0000;

    public static string Class(IntPtr h)
    {
        if (h == IntPtr.Zero) return "";
        StringBuilder sb = new StringBuilder(128);
        GetClassNameW(h, sb, sb.Capacity);
        return sb.ToString();
    }

    // Round-trip time for WM_NULL. Returns -1 when the thread did not answer inside the timeout.
    public static double Ping(IntPtr h, uint timeoutMs)
    {
        if (h == IntPtr.Zero) return -2;
        IntPtr res;
        long t0 = Now();
        bool ok = SendMessageTimeoutW(h, WM_NULL, IntPtr.Zero, IntPtr.Zero,
                                      SMTO_ABORTIFHUNG | SMTO_NORMAL, timeoutMs, out res);
        long t1 = Now();
        return ok ? (t1 - t0) / 10000.0 : -1;
    }

    static long Now() { return DateTime.UtcNow.Ticks; }

    // The desktop context menu, opened the way Explorer's own view control opens it.
    public static void ShowMenu(IntPtr h, int x, int y)
    {
        IntPtr lp = (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF));
        IntPtr res;
        SendMessageTimeoutW(h, WM_CONTEXTMENU, h, lp, SMTO_NORMAL, 250, out res);
    }
    public static void CancelMenu(IntPtr h) { PostMessageW(h, WM_CANCELMODE, IntPtr.Zero, IntPtr.Zero); }

    public static IntPtr FindListView(IntPtr progman)
    {
        IntPtr defView = FindWindowExW(progman, IntPtr.Zero, "SHELLDLL_DefView", null);
        return FindWindowExW(defView, IntPtr.Zero, "SysListView32", "FolderView");
    }

    public static IntPtr FindOurs(IntPtr progman)
    {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(progman, delegate (IntPtr h, IntPtr d)
        {
            string c = Class(h);
            if (c.Length > 12 && c.Substring(0, 12) == "SmartWallpap") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

Add-Type -TypeDefinition $code

$progman = [Lat]::GetShellWindow()   # FindWindow/FindWindowEx cannot see Progman on this build
if ($progman -eq [IntPtr]::Zero) { $progman = [Lat]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, 'Progman', $null) }
$defView = [Lat]::FindWindowExW($progman, [IntPtr]::Zero, 'SHELLDLL_DefView', $null)
$listView = [Lat]::FindWindowExW($defView, [IntPtr]::Zero, 'SysListView32', 'FolderView')
$ours = [Lat]::FindOurs($progman)

Write-Output ("progman={0} defView={1} listView={2} ours={3} ({4})" -f $progman, $defView, $listView, $ours, [Lat]::Class($ours))

$rows = New-Object System.Collections.Generic.List[string]
$rows.Add("label,sample,elapsed_ms,progman_ms,defview_ms,listview_ms,ours_ms,note")

$sw = [System.Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $Samples; $i++) {
    $note = ''
    if ($i -eq $ClickAtSample -and $listView -ne [IntPtr]::Zero) {
        # Same point the earlier icon tests used: middle of the icon area, empty desktop spot.
        $menuStart = [System.Diagnostics.Stopwatch]::StartNew()
        [Lat]::ShowMenu($listView, 900, 500)
        $note = 'menu_send_returned_after_' + $menuStart.ElapsedMilliseconds + 'ms'
    }
    $p = [Lat]::Ping($progman, 3000)
    $d = [Lat]::Ping($defView, 3000)
    $l = [Lat]::Ping($listView, 3000)
    $o = if ($ours -ne [IntPtr]::Zero) { [Lat]::Ping($ours, 3000) } else { -3 }
    $row = [string]::Format([Globalization.CultureInfo]::InvariantCulture,
        '{0},{1},{2:0},{3:0.0},{4:0.0},{5:0.0},{6:0.0},{7}', $Label, $i, $sw.Elapsed.TotalMilliseconds, $p, $d, $l, $o, $note)
    $rows.Add($row)
    Write-Output $row
    if ($i -eq $ClickAtSample) {
        Start-Sleep -Milliseconds 1500      # let the menu sit, like a person looking at it
        [Lat]::CancelMenu($listView)
        $rows.Add("$Label,$i,menu_cancelled,,,,,")
    }
    Start-Sleep -Milliseconds $IntervalMs
}

[System.IO.File]::WriteAllLines($OutFile, $rows, [System.Text.Encoding]::UTF8)
Write-Output "wrote $OutFile"
