# Clicks one screen point with real mouse events (self-drawn Qt controls are invisible to UIA,
# so this is the only way to drive them). Waits between moves and events: the input queue is
# asynchronous and moving the cursor back too soon makes the click land somewhere else.
param([int]$X = 0, [int]$Y = 0, [switch]$MoveOnly, [switch]$Right, [switch]$Stay, [switch]$Nudge)

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

    public static string Go(int x, int y, bool click, bool right, bool stay, bool nudge)
    {
        PT before; GetCursorPos(out before);
        SetCursorPos(x, y);
        Thread.Sleep(150);
        // SetCursorPos alone produces no WM_MOUSEMOVE, so a Qt popup never enters its hover
        // state. One relative mouse_event is what actually moves the highlight.
        if (nudge) mouse_event(0x0001, 2, 0, 0, IntPtr.Zero);
        Thread.Sleep(120);
        PT at; GetCursorPos(out at);
        IntPtr w = WindowFromPoint(at);
        System.Text.StringBuilder sb = new System.Text.StringBuilder(128);
        GetClassNameW(w, sb, 128);
        string who = at.X + "," + at.Y + " -> " + sb.ToString();
        if (click) {
            // LEFTDOWN 0x2 / LEFTUP 0x4, RIGHTDOWN 0x8 / RIGHTUP 0x10
            uint down = right ? 0x0008u : 0x0002u, up = right ? 0x0010u : 0x0004u;
            mouse_event(down, 0, 0, 0, IntPtr.Zero);
            Thread.Sleep(120);
            mouse_event(up, 0, 0, 0, IntPtr.Zero);
            Thread.Sleep(120);
        }
        if (!stay) SetCursorPos(before.X, before.Y);   // the shell flyout closes when the pointer leaves it
        return who;
    }
}
'@

Add-Type -TypeDefinition $code
Write-Output ([Cl]::Go($X, $Y, (-not $MoveOnly), [bool]$Right, [bool]$Stay, [bool]$Nudge))
