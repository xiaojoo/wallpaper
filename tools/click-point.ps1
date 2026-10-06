# Clicks one screen point with real mouse events (self-drawn Qt controls are invisible to UIA,
# so this is the only way to drive them). Waits between moves and events: the input queue is
# asynchronous and moving the cursor back too soon makes the click land somewhere else.
param([int]$X = 0, [int]$Y = 0, [switch]$MoveOnly)

$code = @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class Cl
{
    [StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out PT p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PT p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int m);

    public static string Go(int x, int y, bool click)
    {
        PT before; GetCursorPos(out before);
        SetCursorPos(x, y);
        Thread.Sleep(150);
        PT at; GetCursorPos(out at);
        IntPtr w = WindowFromPoint(at);
        System.Text.StringBuilder sb = new System.Text.StringBuilder(128);
        GetClassNameW(w, sb, 128);
        string who = at.X + "," + at.Y + " -> " + sb.ToString();
        if (click) {
            mouse_event(0x0002, 0, 0, 0, IntPtr.Zero);
            Thread.Sleep(120);
            mouse_event(0x0004, 0, 0, 0, IntPtr.Zero);
            Thread.Sleep(120);
        }
        SetCursorPos(before.X, before.Y);
        return who;
    }
}
'@

Add-Type -TypeDefinition $code
Write-Output ([Cl]::Go($X, $Y, (-not $MoveOnly)))
