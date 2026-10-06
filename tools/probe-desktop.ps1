# Passive probe of the desktop window hierarchy. No messages are sent, nothing is created.
$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class Dsk
{
    [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr FindWindowA(string c, string n);
    [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr FindWindowExA(IntPtr p, IntPtr after, string c, string n);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern int GetClassNameA(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern int GetWindowTextA(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr GetWindowLongPtrA(IntPtr h, int i);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassNameA(h, s, 256); return s.ToString(); }
    static string Txt(IntPtr h) { var s = new StringBuilder(256); GetWindowTextA(h, s, 256); return s.ToString(); }

    public static string Line(IntPtr h)
    {
        RECT r; GetWindowRect(h, out r);
        uint pid; GetWindowThreadProcessId(h, out pid);
        long style = (long)GetWindowLongPtrA(h, unchecked((int)0xFFFFFFEC)); // GWL_STYLE (-16)
        long ex = (long)GetWindowLongPtrA(h, unchecked((int)0xFFFFFFFB));    // GWL_EXSTYLE (-20)
        return string.Format("hwnd=0x{0:X} cls='{1}' txt='{2}' vis={3} rect={4},{5},{6}x{7} style=0x{8:X8} ex=0x{9:X8} pid={10}",
            (long)h, Cls(h), Txt(h), IsWindowVisible(h), r.L, r.T, r.R - r.L, r.B - r.T,
            style & 0xFFFFFFFF, ex & 0xFFFFFFFF, pid);
    }

    public static List<string> Children(IntPtr p)
    {
        var outp = new List<string>();
        EnumChildWindows(p, (h, l) => { outp.Add(Line(h)); return true; }, IntPtr.Zero);
        return outp;
    }

    // direct children only
    public static List<string> DirectChildren(IntPtr p)
    {
        var outp = new List<string>();
        IntPtr c = FindWindowExA(p, IntPtr.Zero, null, null);
        int guard = 0;
        while (c != IntPtr.Zero && guard++ < 64) { outp.Add(Line(c)); c = FindWindowExA(p, c, null, null); }
        return outp;
    }

    public static List<string> TopLevelWithClass(string cls)
    {
        var outp = new List<string>();
        EnumWindows((h, l) => { if (Cls(h) == cls) outp.Add(Line(h)); return true; }, IntPtr.Zero);
        return outp;
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp

$progman = [Dsk]::FindWindowA('Progman', $null)
Write-Output ("PROGMAN 0x{0:X}" -f [int64]$progman)
Write-Output "--- Progman direct children ---"
[Dsk]::DirectChildren($progman) | ForEach-Object { Write-Output $_ }
Write-Output "--- Progman full subtree ---"
[Dsk]::Children($progman) | ForEach-Object { Write-Output $_ }
Write-Output "--- top-level WorkerW ---"
[Dsk]::TopLevelWithClass('WorkerW') | ForEach-Object { Write-Output $_ }
Write-Output "--- WorkerW subtrees (first 3) ---"
$ww = [Dsk]::TopLevelWithClass('WorkerW')
$wwl = [Dsk]::TopLevelWithClass('WorkerW')
$handles = @()
foreach ($line in $wwl) { if ($line -match 'hwnd=0x([0-9A-F]+)') { $handles += [int64]("0x" + $matches[1]) } }
$n = 0
foreach ($h in $handles) {
  if ($n++ -ge 3) { break }
  Write-Output ("WorkerW 0x{0:X} children:" -f $h)
  [Dsk]::DirectChildren([IntPtr]$h) | ForEach-Object { Write-Output ("  " + $_) }
}
Write-Output "--- SHELLDLL_DefView locations ---"
foreach ($h in $handles) {
  $d = [Dsk]::FindWindowExA([IntPtr]$h, [IntPtr]::Zero, 'SHELLDLL_DefView', $null)
  if ($d -ne [IntPtr]::Zero) { Write-Output ("DefView under WorkerW 0x{0:X}" -f $h) }
}
$dv = [Dsk]::FindWindowExA($progman, [IntPtr]::Zero, 'SHELLDLL_DefView', $null)
if ($dv -ne [IntPtr]::Zero) { Write-Output ("DefView under Progman 0x{0:X}" -f [int64]$dv) }
