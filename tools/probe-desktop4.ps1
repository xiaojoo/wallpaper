# Probe 4: Progman subtree + monitor geometry + DComp swapchain feasibility. Read-only.
$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class D4
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string n);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrW(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
    [DllImport("shcore.dll")] public static extern int GetDpiForSystem(out uint dpi);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    static string Cls(IntPtr h) { var s = new StringBuilder(512); int n = GetClassNameW(h, s, 512); return n == 0 ? "" : s.ToString(0, n); }
    static string Txt(IntPtr h) { var s = new StringBuilder(512); int n = GetWindowTextW(h, s, 512); return n == 0 ? "" : s.ToString(0, n); }

    public static string Line(IntPtr h)
    {
        RECT r, cr; GetWindowRect(h, out r); GetClientRect(h, out cr);
        uint pid; GetWindowThreadProcessId(h, out pid);
        return string.Format("0x{0:X} cls='{1}' txt='{2}' vis={3} win={4},{5} {6}x{7} cli={8}x{9} style=0x{10:X8} ex=0x{11:X8} pid={12}",
            (long)h, Cls(h), Txt(h), IsWindowVisible(h), r.L, r.T, r.R - r.L, r.B - r.T, cr.R, cr.B,
            (long)GetWindowLongPtrW(h, -16) & 0xFFFFFFFF, (long)GetWindowLongPtrW(h, -20) & 0xFFFFFFFF, pid);
    }

    public static IntPtr ByEnum(string cls)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => { if (Cls(h) == cls) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }

    public static List<string> Subtree(IntPtr p, int depth)
    {
        var o = new List<string>();
        Action<IntPtr, int> walk = null;
        walk = (root, d) => {
            IntPtr c = FindWindowExW(root, IntPtr.Zero, null, null);
            int g = 0;
            while (c != IntPtr.Zero && g++ < 30) {
                o.Add(new string(' ', d * 2) + Line(c));
                if (d < depth) walk(c, d + 1);
                c = FindWindowExW(root, c, null, null);
            }
        };
        walk(p, 0);
        return o;
    }

    // top-level windows that sit BELOW Progman in z-order (candidates for wallpaper host)
    public static List<string> BelowProgman()
    {
        var o = new List<string>();
        IntPtr pm = ByEnum("Progman");
        IntPtr h = pm;
        int i = 0;
        while (h != IntPtr.Zero) { h = GetWindow(h, 2); /* GW_HWNDNEXT */ if (h != IntPtr.Zero) { o.Add(i + " " + Line(h)); i++; } }
        o.Add("ProgmanLine " + Line(pm));
        IntPtr t = pm; int j = 0;
        while (true) { t = GetWindow(t, 1); /* GW_HWNDPREV */ if (t == IntPtr.Zero) break; o.Add("ABOVE " + j + " " + Line(t)); j++; if (j > 5) break; }
        return o;
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

Write-Output ("Desktop  = " + [D4]::Line([D4]::GetDesktopWindow()))
Write-Output ("Shell    = " + [D4]::Line([D4]::GetShellWindow()))
$pmEnum = [D4]::ByEnum('Progman')
$pmFind = [D4]::FindWindowW('Progman', $null)
Write-Output ("Progman via Enum = 0x{0:X}   via FindWindowW = 0x{1:X}" -f [int64]$pmEnum, [int64]$pmFind)
Write-Output "--- Progman subtree (depth 2) ---"
[D4]::Subtree($pmEnum, 2) | ForEach-Object { Write-Output $_ }
Write-Output "--- z-order neighbours of Progman ---"
[D4]::BelowProgman() | ForEach-Object { Write-Output $_ }
$ww = [D4]::ByEnum('WorkerW')
Write-Output "--- first WorkerW subtree ---"
[D4]::Subtree($ww, 2) | ForEach-Object { Write-Output $_ }
Write-Output "--- monitors ---"
Add-Type -AssemblyName System.Windows.Forms
foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
  $b = $s.Bounds; $w = $s.WorkingArea
  Write-Output ("{0} bounds={1},{2} {3}x{4} work={5},{6} {7}x{8} primary={9}" -f $s.DeviceName, $b.X, $b.Y, $b.Width, $b.Height, $w.X, $w.Y, $w.Width, $w.Height, $s.Primary)
}
$d = 0; [void][D4]::GetDpiForSystem([ref]0)
Write-Output ("DPI info: " + (Get-ItemProperty 'HKCU:\Control Panel\Desktop' -Name LogPixels -ErrorAction SilentlyContinue).LogPixels)
Write-Output ("Scale(96 dpi base) via WinRT not probed here")
Write-Output ("Power: " + (Get-CimInstance -Win32_Battery -ErrorAction SilentlyContinue | Select-Object -First 1 | ForEach-Object { "$($_.EstimatedChargeRemaining)% present" }))
