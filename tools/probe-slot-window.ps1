# Where is the wallpaper's own child window, and what rect does it actually have?
# The engine reports "M0 has an empty client area" 25 times a second; this reads the window itself.
# ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param([string]$OutFile = 'H:\wallpaper\build\tmp\slot.txt')
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;
public class SlotProbe {
    public struct R { public int L, T, Rt, B; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
    [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int i);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    public static List<string> hits = new List<string>();
    public static EnumProc keep = Collect;          // keep the delegate alive: a collected one stops the walk
    static bool Collect(IntPtr h, IntPtr l) {
        StringBuilder sb = new StringBuilder(128);
        GetClassNameW(h, sb, 128);
        if (sb.ToString() == "SmartWallpaperDesktop") {
            R r; GetWindowRect(h, out r);
            IntPtr p = GetParent(h);
            string ps = "none";
            if (p != IntPtr.Zero) {
                StringBuilder cb = new StringBuilder(128);
                GetClassNameW(p, cb, 128);
                R pr; GetWindowRect(p, out pr);
                ps = cb + " (" + pr.L + "," + pr.T + " " + (pr.Rt - pr.L) + "x" + (pr.B - pr.T) + ")";
            }
            hits.Add("hwnd=" + h + " title=\"" + Title(h) + "\" rect=(" + r.L + "," + r.T + " "
                + (r.Rt - r.L) + "x" + (r.B - r.T) + ") visible=" + IsWindowVisible(h)
                + " style=0x" + GetWindowLong(h, -16).ToString("X8")
                + " ex=0x" + GetWindowLong(h, -20).ToString("X8") + " parent=" + ps);
        }
        return true;
    }
    static string Title(IntPtr h) {
        StringBuilder sb = new StringBuilder(128);
        GetWindowTextW(h, sb, 128);
        return sb.ToString();
    }
    public static int Run() {
        hits.Clear();
        EnumWindows(keep, IntPtr.Zero);            // top level, and Collect recurses via EnumChildWindows
        foreach (IntPtr top in Tops()) EnumChildWindows(top, keep, IntPtr.Zero);
        return hits.Count;
    }
    static List<IntPtr> Tops() {
        List<IntPtr> t = new List<IntPtr>();
        EnumProc f = delegate (IntPtr h, IntPtr l) { t.Add(h); return true; };
        EnumWindows(f, IntPtr.Zero);
        return t;
    }
}
"@
$n = [SlotProbe]::Run()
$lines = [SlotProbe]::hits
if ($n -eq 0) { $lines = @("no SmartWallpaperDesktop window found") }
[System.IO.File]::WriteAllLines($OutFile, $lines, [System.Text.Encoding]::UTF8)
Write-Output ("slots=" + $n)
