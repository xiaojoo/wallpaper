# Drives real mouse input against the desktop icons and reports what the shell did.
#   -Mode list                     print every desktop icon with its rect and selection state
#   -Mode click -Index N           single click, then report selection
#   -Mode dblclick -Index N        double click, then report any window that appeared
#   -Mode drag -Index N -Dx -Dy    drag the icon, report the move, then drag it back
#   -Mode coverclick -Index N      NEGATIVE CONTROL: cover the icons with a full-screen window,
#                                  click, and confirm the click is NOT received
#   -Mode closehandle -Handle H    close a window handle reported by dblclick
# ASCII only: this file is parsed by PowerShell 5.1 under a non-UTF8 codepage.
param(
    [ValidateSet('unselect','list','click','dblclick','drag','coverclick','closehandle','hitpoint')][string]$Mode = 'list',
    [int]$Index = 0,
    [int]$Dx = 90,
    [int]$Dy = 90,
    [int]$Handle = 0
)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase, System.Windows.Forms, System.Drawing

$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Automation;

public static class Desk
{
    [StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out PT p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern uint GetDoubleClickTime();
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PT p);
    [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string c, string n);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr l);


    public static void Click(int x, int y)
    {
        SetCursorPos(x, y);
        Thread.Sleep(60);
        mouse_event(LEFTDOWN, 0, 0, 0, IntPtr.Zero);
        Thread.Sleep(45);
        mouse_event(LEFTUP, 0, 0, 0, IntPtr.Zero);
    }

    public static void DoubleClick(int x, int y)
    {
        Click(x, y);
        Thread.Sleep((int)(GetDoubleClickTime() / 3));
        mouse_event(LEFTDOWN, 0, 0, 0, IntPtr.Zero);
        Thread.Sleep(40);
        mouse_event(LEFTUP, 0, 0, 0, IntPtr.Zero);
    }

    // A ListView only starts a drag after the press has sat still past the drag threshold,
    // so the button-down is held before any movement happens.
    [StructLayout(LayoutKind.Sequential)]
    struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }

    [StructLayout(LayoutKind.Sequential)]
    struct INPUT { public uint type; public MOUSEINPUT mi; }

    [DllImport("user32.dll", SetLastError = true)]
    static extern uint SendInput(uint n, INPUT[] inputs, int cbSize);

    [DllImport("user32.dll")] static extern int GetSystemMetrics(int n);

    const uint INPUT_MOUSE = 0, MOVE = 0x0001, LEFTDOWN = 0x0002, LEFTUP = 0x0004, ABSOLUTE = 0x8000, VIRTUALDESK = 0x4000;

    // SendInput with absolute coordinates: mouse_event + SetCursorPos moves the cursor but the
    // shell's ListView drag threshold never fires (measured on this machine).
    public static void MoveAbs(int x, int y)
    {
        int vl = GetSystemMetrics(76), vt = GetSystemMetrics(77);
        int vw = GetSystemMetrics(78), vh = GetSystemMetrics(79);
        if (vw < 2) vw = GetSystemMetrics(0);
        if (vh < 2) vh = GetSystemMetrics(1);
        INPUT[] i = new INPUT[1];
        i[0].type = INPUT_MOUSE;
        i[0].mi.dx = (int)(((long)(x - vl) * 65535) / (vw - 1));
        i[0].mi.dy = (int)(((long)(y - vt) * 65535) / (vh - 1));
        i[0].mi.dwFlags = MOVE | ABSOLUTE | VIRTUALDESK;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void BtnDown()
    {
        INPUT[] i = new INPUT[1];
        i[0].type = INPUT_MOUSE;
        i[0].mi.dwFlags = LEFTDOWN;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void BtnUp()
    {
        INPUT[] i = new INPUT[1];
        i[0].type = INPUT_MOUSE;
        i[0].mi.dwFlags = LEFTUP;
        SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void DragDown(int x1, int y1)
    {
        MoveAbs(x1, y1);
        Thread.Sleep(150);
        BtnDown();
        Thread.Sleep(320);
    }

    public static void DragMoveTo(int x1, int y1, int x2, int y2)
    {
        for (int i = 1; i <= 20; i++)
        {
            MoveAbs(x1 + (x2 - x1) * i / 20, y1 + (y2 - y1) * i / 20);
            Thread.Sleep(30);
        }
        Thread.Sleep(400);
    }

    public static void DragUp()
    {
        BtnUp();
        Thread.Sleep(200);
    }

    public static void Drag(int x1, int y1, int x2, int y2)
    {
        DragDown(x1, y1);
        DragMoveTo(x1, y1, x2, y2);
        DragUp();
    }

    public static string OwnerAt(int x, int y)
    {
        PT p = new PT(); p.X = x; p.Y = y;
        IntPtr h = WindowFromPoint(p);
        if (h == IntPtr.Zero) return "none";
        var cls = new StringBuilder(300); GetClassNameW(h, cls, 300);
        var txt = new StringBuilder(300); GetWindowTextW(h, txt, 300);
        return string.Format("0x{0:X}:{1}:{2}", (long)h, cls, txt);
    }

    public static List<string> TopHandles()
    {
        var o = new List<string>();
        EnumWindows((h, l) => {
            var cls = new StringBuilder(300); GetClassNameW(h, cls, 300);
            var txt = new StringBuilder(300); GetWindowTextW(h, txt, 300);
            if (IsWindowVisible(h) && (cls.ToString() == "CabinetWClass" || cls.ToString() == "ExploreWClass"))
                o.Add(((long)h).ToString() + "|" + txt.ToString());
            return true;
        }, IntPtr.Zero);
        return o;
    }

    public static bool Close(IntPtr h)
    {
        return PostMessageW(h, 0x0010 /*WM_CLOSE*/, IntPtr.Zero, IntPtr.Zero);
    }
}

public static class Icons
{
    public class Item
    {
        public string Name;
        public int X, Y, W, H;
        public bool Selected;
        public AutomationElement El;
        public int CX { get { return X + W / 2; } }
        public int CY { get { return Y + H / 2; } }
    }

    static AutomationElement listEl;

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);

    static string WinClass(int hwnd)
    {
        if (hwnd == 0) return "";
        var sb = new StringBuilder(300);
        int n = GetClassNameW((IntPtr)hwnd, sb, 300);
        return n > 0 ? sb.ToString(0, n) : "";
    }

    // The desktop icon list is the SysListView32 named FolderView. Anything else that looks like a
    // list (a file panel inside some app window) must not be mistaken for it.
    static AutomationElement FindList()
    {
        if (listEl != null) return listEl;
        var cond = new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.List);
        AutomationElementCollection all = AutomationElement.RootElement.FindAll(TreeScope.Descendants, cond);
        AutomationElement best = null;
        int bestCount = -1;
        foreach (AutomationElement e in all)
        {
            var items = e.FindAll(TreeScope.Children,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem));
            bool isFolderView = WinClass(e.Current.NativeWindowHandle) == "SysListView32";
            if (isFolderView && items.Count > bestCount) { best = e; bestCount = items.Count; }
        }
        listEl = best;
        return listEl;
    }

    public static List<Item> Read()
    {
        var outp = new List<Item>();
        AutomationElement l = FindList();
        if (l == null) return outp;
        var items = l.FindAll(TreeScope.Children,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem));
        foreach (AutomationElement e in items)
        {
            var r = e.Current.BoundingRectangle;
            if (r.Width <= 0 || r.Height <= 0) continue;
            Item it = new Item();
            it.Name = e.Current.Name;
            it.X = (int)r.X; it.Y = (int)r.Y; it.W = (int)r.Width; it.H = (int)r.Height;
            it.El = e;
            object pat;
            if (e.TryGetCurrentPattern(SelectionItemPattern.Pattern, out pat))
                it.Selected = ((SelectionItemPattern)pat).Current.IsSelected;
            outp.Add(it);
        }
        return outp;
    }

    public static void Unselect(Item it)
    {
        object pat;
        if (it.El.TryGetCurrentPattern(SelectionItemPattern.Pattern, out pat))
            ((SelectionItemPattern)pat).RemoveFromSelection();
    }

    public static void Select(Item it)
    {
        object pat;
        if (it.El.TryGetCurrentPattern(SelectionItemPattern.Pattern, out pat))
            ((SelectionItemPattern)pat).Select();
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies UIAutomationClient, UIAutomationTypes, WindowsBase, System.Runtime.InteropServices

function Show-List([int]$limit) {
    $items = [Icons]::Read()
    $i = 0
    foreach ($it in $items) {
        if ($i -ge $limit) { break }
        Write-Output ("[{0}] '{1}' rect={2},{3} {4}x{5} center={6},{7} selected={8}" -f $i, $it.Name, $it.X, $it.Y, $it.W, $it.H, $it.CX, $it.CY, $it.Selected)
        $i++
    }
    Write-Output ("total items = " + $items.Count)
}

function Center-Of([int]$idx) {
    $items = [Icons]::Read()
    if ($idx -ge $items.Count) { throw "index $idx out of range ($($items.Count) items)" }
    return $items[$idx]
}

$script:origCursor = New-Object Desk+PT
[void][Desk]::GetCursorPos([ref]$script:origCursor)

switch ($Mode) {
    'unselect' {
        foreach ($s in [Icons]::Read()) { if ($s.Selected) { [Icons]::Unselect($s) } }
        $left = @([Icons]::Read() | Where-Object { $_.Selected })
        Write-Output ("still selected = " + $left.Count)
    }

    'list' { Show-List 60 }

    'click' {
        $it = Center-Of $Index
        Write-Output ("target [{0}] '{1}' center={2},{3}" -f $Index, $it.Name, $it.CX, $it.CY)
        # clear any existing selection first, otherwise "selected=true" proves nothing
        foreach ($s in [Icons]::Read()) { if ($s.Selected) { [Icons]::Unselect($s) } }
        [Desk]::Click($it.CX, $it.CY)
        Start-Sleep -Milliseconds 450
        $after = Center-Of $Index
        Write-Output ("after click: selected={0}" -f $after.Selected)
        Write-Output ("VERDICT click: " + $(if ($after.Selected) { 'ICON RECEIVED THE CLICK' } else { 'CLICK DID NOT REACH THE ICON' }))
    }

    'coverclick' {
        $it = Center-Of $Index
        Write-Output ("negative control: covering the desktop, then clicking [{0}] '{1}'" -f $Index, $it.Name)
        $form = New-Object System.Windows.Forms.Form
        $form.FormBorderStyle = 'None'
        $form.StartPosition = 'Manual'
        $form.Bounds = [System.Drawing.Rectangle]::new(0, 0, 3000, 2000)
        $form.TopMost = $true
        $form.ShowInTaskbar = $false
        $form.BackColor = [System.Drawing.Color]::Gray
        $form.Show()
        [System.Windows.Forms.Application]::DoEvents()
        Start-Sleep -Milliseconds 300
        foreach ($s in [Icons]::Read()) { if ($s.Selected) { [Icons]::Unselect($s) } }
        [Desk]::Click($it.CX, $it.CY)
        Start-Sleep -Milliseconds 450
        $after = Center-Of $Index
        $form.Close()
        Write-Output ("after click under cover: selected={0}" -f $after.Selected)
        Write-Output ("VERDICT coverclick: " + $(if (-not $after.Selected) { 'instrument discriminates (blocked as expected)' } else { 'INSTRUMENT IS BROKEN - selection happened through an opaque window' }))
    }

    'dblclick' {
        $it = Center-Of $Index
        $before = [Desk]::TopHandles()
        Write-Output ("target [{0}] '{1}' center={2},{3}" -f $Index, $it.Name, $it.CX, $it.CY)
        [Desk]::DoubleClick($it.CX, $it.CY)
        # Explorer on 24H2+ takes longer than a second to show its window; poll instead of one long sleep.
        $new = @()
        for ($t = 0; $t -lt 12; $t++) {
            Start-Sleep -Milliseconds 500
            $after = [Desk]::TopHandles()
            $new = @($after | Where-Object { $before -notcontains $_ })
            if ($new.Count -gt 0) { break }
        }
        if ($new.Count -eq 0) { Write-Output 'VERDICT dblclick: NO WINDOW APPEARED in 6 s' }
        foreach ($n in $new) {
            $h = ($n -split '\|')[0]
            Write-Output ("new window handle=" + $h + " title=" + (($n -split '\|')[1]))
            Write-Output "VERDICT dblclick: ICON ACTIVATED (close it with -Mode closehandle -Handle $h)"
        }
    }

    'hitpoint' {
        $items = [Icons]::Read()
        $iconOwner = @{}
        foreach ($it in $items) {
            $o = [Desk]::OwnerAt($it.CX, $it.Y + 20)
            if (-not $iconOwner.ContainsKey($o)) { $iconOwner[$o] = 0 }
            $iconOwner[$o] = $iconOwner[$o] + 1
        }
        Write-Output ("icon points (n=" + $items.Count + ") owned by:")
        foreach ($k in $iconOwner.Keys) { Write-Output ("  " + $iconOwner[$k] + " x  " + $k) }
        $empty = @(600,300), @(1200,1500), @(2400,700), @(3000,1800), @(900,1900), @(1800,400)
        $empOwner = @{}
        foreach ($e in $empty) {
            $o = [Desk]::OwnerAt($e[0], $e[1])
            if (-not $empOwner.ContainsKey($o)) { $empOwner[$o] = 0 }
            $empOwner[$o] = $empOwner[$o] + 1
        }
        Write-Output "empty desktop points owned by:"
        foreach ($k in $empOwner.Keys) { Write-Output ("  " + $empOwner[$k] + " x  " + $k) }
    }

    'closehandle' {
        $ok = [Desk]::Close([IntPtr]$Handle)
        Write-Output ("WM_CLOSE posted = " + $ok)
    }

    'drag' {
        $it = Center-Of $Index
        $gx = $it.X + [Math]::Min(30, [int]($it.W / 2))
        $gy = $it.Y + 20
        Write-Output ("target [{0}] '{1}' glyph at {2},{3} (item rect {4},{5} {6}x{7})" -f $Index, $it.Name, $gx, $gy, $it.X, $it.Y, $it.W, $it.H)
        # Read the position while the button is still held: that separates "the drag never started"
        # from "the shell snapped it back on drop" (auto-arrange).
        [Desk]::DragDown($gx, $gy)
        [Desk]::DragMoveTo($gx, $gy, $gx + $Dx, $gy + $Dy)
        $held = Center-Of $Index
        Write-Output ("while held: at {0},{1} (delta {2},{3})" -f $held.CX, $held.CY, ($held.CX - $it.CX), ($held.CY - $it.CY))
        [Desk]::DragUp()
        Start-Sleep -Milliseconds 900
        $moved = Center-Of $Index
        Write-Output ("after drag: at {0},{1} (delta {2},{3})" -f $moved.CX, $moved.CY, ($moved.CX - $it.CX), ($moved.CY - $it.CY))
        if (($moved.CX - $it.CX) -ne 0 -or ($moved.CY - $it.CY) -ne 0) {
            [Desk]::Drag($moved.X + 30, $moved.Y + 20, $gx, $gy)
            Start-Sleep -Milliseconds 700
            $back = Center-Of $Index
            Write-Output ("after return: at {0},{1} (residual {2},{3})" -f $back.CX, $back.CY, ($back.CX - $it.CX), ($back.CY - $it.CY))
        }
        $dragStarted = ($held.CX - $it.CX) -ne 0 -or ($held.CY - $it.CY) -ne 0
        $stayed = ($moved.CX - $it.CX) -ne 0 -or ($moved.CY - $it.CY) -ne 0
        $verdict = if ($stayed) { 'ICON MOVED AND STAYED (drag works)' }
                   elseif ($dragStarted) { 'DRAG STARTED BUT THE SHELL SNAPPED IT BACK (auto-arrange is on)' }
                   else { 'DRAG NEVER STARTED - synthesized button-down+move did not initiate a shell drag' }
        Write-Output ("VERDICT drag: " + $verdict)
    }
}
[void][Desk]::SetCursorPos($script:origCursor.X, $script:origCursor.Y)
