# Close a window the way Alt+F4 does - WM_CLOSE to its own HWND - without needing the mouse.
# Used to prove the settings window's close button path: the process must survive and the window
# must go invisible, because the tray icon is the only way back.
param([string]$Title = "Wallpaper", [switch]$Show)
$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class Cw
{
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public static string Run(string want, bool show)
    {
        IntPtr hit = IntPtr.Zero; uint hpid = 0;
        EnumWindows((h, l) => {
            var t = new StringBuilder(512); GetWindowTextW(h, t, 512);
            if (t.ToString().Trim() != want) return true;
            GetWindowThreadProcessId(h, out hpid);
            if (hpid == 0) return true;
            hit = h; return false;
        }, IntPtr.Zero);
        if (hit == IntPtr.Zero) return "no window titled [" + want + "]";
        RECT b0; GetWindowRect(hit, out b0);
        string before = "visible=" + IsWindowVisible(hit) + " rect=" + b0.L + "," + b0.T + "," + b0.R + "," + b0.B;
        if (show) ShowWindow(hit, 5);              // SW_SHOW
        else PostMessageW(hit, 0x0010, IntPtr.Zero, IntPtr.Zero);   // WM_CLOSE
        System.Threading.Thread.Sleep(1200);
        RECT b1; GetWindowRect(hit, out b1);
        string after = "visible=" + IsWindowVisible(hit) + " rect=" + b1.L + "," + b1.T + "," + b1.R + "," + b1.B;
        bool alive = false; string name = "?";
        try {
            var p = System.Diagnostics.Process.GetProcessById((int)hpid);
            alive = !p.HasExited; name = p.ProcessName;
        } catch (Exception) { alive = false; }
        return "hwnd=" + ((long)hit).ToString("X") + " pid=" + hpid + " image=" + name +
               (show ? " (SW_SHOW)" : " (WM_CLOSE)") +
               "\n  before: " + before + "\n  after : " + after + "\n  processAliveAfterClose=" + alive;
    }
}
'@
Add-Type -TypeDefinition $code
Write-Output ([Cw]::Run($Title, [bool]$Show))
