# Who owns the foreground window that is pinning the cursor? Prints class, title, pid and image name.
$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class Fg
{
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public static string Run()
    {
        IntPtr h = GetForegroundWindow();
        var c = new StringBuilder(256); GetClassNameW(h, c, 256);
        var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
        uint pid; GetWindowThreadProcessId(h, out pid);
        RECT r; GetWindowRect(h, out r);
        string img = "";
        try { img = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch (Exception e) { img = "?" + e.Message; }
        return "hwnd=" + ((long)h).ToString("X") + " class=[" + c + "] title=[" + t + "] rect=" +
               r.L + "," + r.T + "," + r.R + "," + r.B + " pid=" + pid + " image=" + img;
    }
}
'@
Add-Type -TypeDefinition $code
Write-Output ([Fg]::Run())
