# Who owns a window by class, and where it is - used to dismiss a stray message box without
# guessing at coordinates. ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param([string]$Class = '#32770', [string]$Act = 'report')

$src = @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class Dlg
{
    [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string c, string t);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr a, string c, string t);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);

    public static void Run(string cls, string act)
    {
        // FindWindowW is unreliable here (the P/Invoke default marshals the class as ANSI), so the
        // top-level list is walked and compared the way list-windows.ps1 does it.
        EnumWindows((h, l) =>
        {
            var want = new StringBuilder(200);
            GetClassNameW(h, want, 200);
            if (want.ToString() != cls) return true;
            Report(h, act);
            return true;
        }, IntPtr.Zero);
        Console.WriteLine("done");
    }

    static void Report(IntPtr h, string act)
    {
        IntPtr first = FindWindowExW(h, IntPtr.Zero, null, null);
        {
            RECT r; GetWindowRect(h, out r);
            uint pid; GetWindowThreadProcessId(h, out pid);
            var t = new StringBuilder(300); GetWindowTextW(h, t, 300);
            var cn = new StringBuilder(200); GetClassNameW(h, cn, 200);
            Console.WriteLine("hwnd=0x" + ((long)h).ToString("X") + " pid=" + pid + " class=" + cn +
                              " title=" + t + " rect=" + r.L + "," + r.T + " " + (r.R - r.L) + "x" + (r.B - r.T));
        }
        IntPtr b = first;
        while (b != IntPtr.Zero)
        {
            RECT br; GetWindowRect(b, out br);
            var bt = new StringBuilder(200); GetWindowTextW(b, bt, 200);
            var bc = new StringBuilder(200); GetClassNameW(b, bc, 200);
            Console.WriteLine("    child class=" + bc + " '" + bt + "' rect=" + br.L + "," + br.T + " " +
                              (br.R - br.L) + "x" + (br.B - br.T));
            if (act == "close" && bc.ToString() == "Button") PostMessageW(b, 0x00F5, IntPtr.Zero, IntPtr.Zero);  // BM_CLICK
            b = FindWindowExW(h, b, null, null);
        }
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp
[Dlg]::Run($Class, $Act)
