# tools/probe-icon-restore.ps1 - can the user get his desktop icons back without our process running?
#
# Task #31's whole design decision. The ring hides the shell's real icon list, and anything we do to
# another process's window is state that outlives us if we are killed - so the question is not "can we
# restore it" (we can, on any exit we get to run) but "is there a way back that does not need us at
# all". The only candidate is the desktop's own context menu: right-click -> View -> Show desktop
# icons. If that menu still appears while the list is hidden, and its item still owns the visibility,
# then a killed or crashed renderer leaves the user one click from his icons. If it does not appear,
# hiding the list is not acceptable and the ring has to cover or clip instead.
#
#   -Mode state     what the icon list looks like right now, counted three ways
#   -Mode menu      hide it, right-click empty desktop, report whether a menu appeared and what is in
#                   it, then dismiss it. Restores the icons in a finally block either way.
#   -Mode restore   as menu, but then drive the shell's own Show-desktop-icons item and time how long
#                   the icons take to come back - the measurement the task is judged on
#   -Mode control   the same right-click WITHOUT hiding anything: the menu must also appear. If this
#                   one fails, the menu test in the other modes means nothing.
#
# Synthesised input goes to the desktop only, and only after the window at that point has been checked
# to be the shell's own (Progman / SHELLDLL_DefView / SysListView32 / WorkerW) - never into whatever
# application the user happens to have open there.
# ASCII only: PowerShell 5.1 parses this file under a non-UTF8 codepage.
param(
    [ValidateSet('state','menu','restore','control','inspect','clickback')][string]$Mode = 'state',
    [int]$PX = 3500,
    [int]$PY = 300
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase

$native = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class Shell
{
    [StructLayout(LayoutKind.Sequential)] public struct PPOINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);

    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PPOINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint f);

    const uint RIGHTDOWN = 0x0008, RIGHTUP = 0x0010, GA_ROOT = 2;
    public const int SW_HIDE = 0, SW_SHOW = 5;

    public static string ClassOf(IntPtr h) { var sb=new StringBuilder(256); GetClassNameW(h,sb,256); return sb.ToString(); }
    public static string TitleOf(IntPtr h) { var sb=new StringBuilder(256); GetWindowTextW(h,sb,256); return sb.ToString(); }

    static List<IntPtr> Collect(EnumProc cb) { var l=new List<IntPtr>(); EnumWindows(cb, GCHandle.ToIntPtr(GCHandle.Alloc(l))); return l; }

    static EnumProc keepTop;
    static GCHandle keepHandle;
    public static List<IntPtr> Tops()
    {
        var l = new List<IntPtr>();
        keepHandle = GCHandle.Alloc(l);
        keepTop = (h, arg) => { l.Add(h); return true; };
        EnumWindows(keepTop, GCHandle.ToIntPtr(keepHandle));
        keepHandle.Free();
        return l;
    }

    static List<IntPtr> kids;
    static EnumProc keepKid;
    public static List<IntPtr> Kids(IntPtr root)
    {
        kids = new List<IntPtr>();
        keepKid = (h, arg) => { kids.Add(h); return true; };
        EnumChildWindows(root, keepKid, IntPtr.Zero);
        return kids;
    }

    // Progman -> SHELLDLL_DefView -> SysListView32, found by walking because FindWindowW("Progman")
    // returns 0 on this build and FindWindowExW only sees direct children.
    public static IntPtr ListView()
    {
        foreach (var h in Tops()) {
            if (ClassOf(h) != "Progman") continue;
            foreach (var k in Kids(h)) if (ClassOf(k) == "SysListView32") return k;
        }
        return IntPtr.Zero;
    }

    public static string Name(IntPtr h)
    {
        if (h == IntPtr.Zero) return "<none>";
        RECT r; GetWindowRect(h, out r);
        return ClassOf(h) + " @" + r.L + "," + r.T + "-" + r.R + "," + r.B;
    }

    public static string OwnerAt(int x, int y)
    {
        var p = new PPOINT { X = x, Y = y };
        return Name(WindowFromPoint(p));
    }

    public static IntPtr HwndAt(int x, int y)
    {
        var p = new PPOINT { X = x, Y = y };
        return WindowFromPoint(p);
    }

    public static bool IsShell(IntPtr h)
    {
        if (h == IntPtr.Zero) return false;
        var c = ClassOf(h);
        if (c == "Progman" || c == "SHELLDLL_DefView" || c == "SysListView32" || c == "WorkerW") return true;
        var root = GetAncestor(h, GA_ROOT);
        return root != IntPtr.Zero && ClassOf(root) == "Progman";
    }

    public static void RightClick(int x, int y)
    {
        SetCursorPos(x, y);
        Thread.Sleep(120);
        mouse_event(RIGHTDOWN, 0, 0, 0, IntPtr.Zero);
        Thread.Sleep(60);
        mouse_event(RIGHTUP, 0, 0, 0, IntPtr.Zero);
        Thread.Sleep(450);
    }

    public static void PressEsc()
    {
        keybd_event(0x1B, 0, 0, IntPtr.Zero);
        keybd_event(0x1B, 0, 2, IntPtr.Zero);
    }
    [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);

    // What a context menu is on this build is not assumed: the tray's is a native #32768 but the
    // desktop's on Windows 11 is drawn by explorer's XAML stack, so the instrument reports whatever
    // visible top-level window appeared, whatever it is called.
    public static List<string> Snapshot()
    {
        var o = new List<string>();
        foreach (var h in Tops()) {
            if (!IsWindowVisible(h)) continue;
            o.Add(((long)h).ToString() + "|" + Name(h) + "|" + TitleOf(h));
        }
        return o;
    }
}
'@
Add-Type -TypeDefinition $native -Language CSharp -ReferencedAssemblies System.Runtime.InteropServices

$uia = @'
using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;
using System.Windows.Automation;

public static class Menu
{
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    static string Cls(IntPtr h) { var sb=new StringBuilder(64); GetClassNameW(h,sb,64); return sb.ToString(); }

    public static IntPtr TopMenu()
    {
        foreach (AutomationElement e in AutomationElement.RootElement.FindAll(TreeScope.Children,
                 new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Menu)))
            return e.Current.NativeWindowHandle != 0 ? (IntPtr)e.Current.NativeWindowHandle : IntPtr.Zero;
        return IntPtr.Zero;
    }

    static List<AutomationElement> Items(AutomationElement root)
    {
        var o = new List<AutomationElement>();
        foreach (AutomationElement e in root.FindAll(TreeScope.Descendants,
                 new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem)))
            o.Add(e);
        return o;
    }

    public static List<string> Names(IntPtr hwnd)
    {
        var o = new List<string>();
        if (hwnd == IntPtr.Zero) return o;
        AutomationElement el;
        try { el = AutomationElement.FromHandle(hwnd); } catch { return o; }
        if (el == null) return o;
        foreach (var m in Items(el)) {
            string state = "";
            object p;
            if (m.TryGetCurrentPattern(TogglePattern.Pattern, out p))
                state = " [" + ((TogglePattern)p).Current.ToggleState + "]";
            o.Add(m.Current.Name + state);
        }
        return o;
    }

    // Invoke the first item whose name contains any of these needles. Returns what was clicked.
    // These menu items are XAML, and a checkable one exposes Toggle rather than Invoke - the first
    // version of this only looked for Invoke and reported "(no invoke pattern)" without clicking.
    public static string InvokeFirst(IntPtr hwnd, string[] needles)
    {
        if (hwnd == IntPtr.Zero) return "<no menu>";
        AutomationElement el;
        try { el = AutomationElement.FromHandle(hwnd); } catch { return "<unreadable>"; }
        if (el == null) return "<no element>";
        foreach (var m in Items(el)) {
            foreach (var n in needles) {
                if (m.Current.Name.IndexOf(n, StringComparison.OrdinalIgnoreCase) < 0) continue;
                object p;
                if (m.TryGetCurrentPattern(InvokePattern.Pattern, out p)) {
                    ((InvokePattern)p).Invoke();
                    return m.Current.Name + " (invoked)";
                }
                if (m.TryGetCurrentPattern(TogglePattern.Pattern, out p)) {
                    var was = ((TogglePattern)p).Current.ToggleState;
                    ((TogglePattern)p).Toggle();
                    return m.Current.Name + " (toggled from " + was + ")";
                }
                return m.Current.Name + " (no invoke and no toggle)";
            }
        }
        return "<none of: " + string.Join(",", needles) + ">";
    }

    public static int DesktopIconCount()
    {
        foreach (AutomationElement e in AutomationElement.RootElement.FindAll(TreeScope.Descendants,
                 new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.List)))
        {
            if (Cls((IntPtr)e.Current.NativeWindowHandle) != "SysListView32") continue;
            var items = e.FindAll(TreeScope.Children,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem));
            if (items.Count > 0) return items.Count;
        }
        return -1;
    }
}
'@
Add-Type -TypeDefinition $uia -Language CSharp -ReferencedAssemblies UIAutomationClient, UIAutomationTypes, WindowsBase, System.Runtime.InteropServices

$list = [Shell]::ListView()
if ($list -eq [IntPtr]::Zero) { Write-Output 'ABORT: no SysListView32 under Progman, nothing touched'; exit 1 }

function State([string]$tag) {
    $vis = [Shell]::IsWindowVisible($list)
    Write-Output ("{0,-9} list={1} visible={2} items={3}" -f $tag, [Shell]::Name($list), $vis, ([Menu]::DesktopIconCount()))
}

if ($Mode -eq 'state') { State 'state'; exit 0 }

$owner = [Shell]::OwnerAt($PX, $PY)
Write-Output ("right-click point $PX,$PY -> $owner")
if (-not [Shell]::IsShell([Shell]::HwndAt($PX, $PY))) {
    Write-Output 'ABORT: that point is not owned by the shell desktop - pick -PX/-PY over empty desktop'
    exit 1
}

# The needles are built from code points rather than written into this file: PowerShell 5.1 reads a
# .ps1 as GBK, so a literal Chinese string here can break parsing without any error to read.
$iconWords = ,([string][char]0x684C + [char]0x9762 + [char]0x56FE + [char]0x6807)   # "desktop icons"
$iconWords += 'desktop icons'
$iconWords += 'icons'

$hid = $false
try {
    # inspect and clickback deliberately do NOT hide anything themselves: they read and drive whatever
    # state the engine left, which is the whole question - does the shell agree with us, and does one
    # click by the user undo what our process did?
    if ($Mode -eq 'menu' -or $Mode -eq 'restore') {
        [void][Shell]::ShowWindow($list, [Shell]::SW_HIDE); $hid = $true; Start-Sleep -Milliseconds 800
    }
    State $(if ($hid) { 'hidden' } else { 'as-is' })

    $was = @([Shell]::Snapshot())
    [Shell]::RightClick($PX, $PY)
    $now = @([Shell]::Snapshot())
    $new = @($now | Where-Object { $was -notcontains $_ })
    Write-Output ("top-level windows before=" + $was.Count + "  after=" + $now.Count + "  new=" + $new.Count)
    foreach ($n in $new) { Write-Output ("  new: " + $n) }

    $hwnd = [IntPtr]::Zero
    if ($new.Count -gt 0) { $hwnd = [IntPtr]([int64](($new[0] -split '\|')[0])) }
    $label = $(if ($hid) { 'while hidden' } else { 'control, not hidden' })
    if ($hwnd -eq [IntPtr]::Zero) {
        Write-Output ("VERDICT menu ($label): NO MENU APPEARED")
    } else {
        $names = [Menu]::Names($hwnd)
        Write-Output ("menu " + [Shell]::Name($hwnd) + " items (" + $names.Count + "): " + ($names -join ' | '))
        if ($Mode -eq 'restore' -or $Mode -eq 'clickback' -or $Mode -eq 'inspect') {
            # 查看 is a submenu: its items do not exist in the tree until it is opened.
            $viewWords = ,([string][char]0x67E5 + [char]0x770B)          # "View"
            $viewWords += 'View'
            $rounds = 2
            if ($Mode -eq 'clickback') { $rounds = 1 }
            if ($Mode -eq 'inspect') { $rounds = 0 }     # read the checkmark, touch nothing

            # Open 查看 once and print what is under it. inspect exists because the checkmark is the
            # claim being tested: if our hide did not move it, the shell does not know we hid anything
            # and the user's one-click recovery is not there.
            $before2 = @([Shell]::Snapshot())
            $opened = [Menu]::InvokeFirst($hwnd, $viewWords)
            Start-Sleep -Milliseconds 600
            $new2 = @(@([Shell]::Snapshot()) | Where-Object { $before2 -notcontains $_ })
            $sub = [IntPtr]::Zero
            if ($new2.Count -gt 0) { $sub = [IntPtr]([int64](($new2[0] -split '\|')[0])) }
            $where = $hwnd
            if ($sub -ne [IntPtr]::Zero) { $where = $sub }
            Write-Output ("submenu (" + $opened + "): " + (([Menu]::Names($where)) -join ' | '))
            $hide0 = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced').HideIcons
            Write-Output ("shell state before any click: HideIcons=" + $hide0 +
                          "  list visible=" + [Shell]::IsWindowVisible($list))

            for ($round = 1; $round -le $rounds; $round++) {
                $t0 = Get-Date
                $clicked = [Menu]::InvokeFirst($where, $iconWords)
                $back = -1
                for ($i = 0; $i -lt 40; $i++) {
                    Start-Sleep -Milliseconds 100
                    if ([Shell]::IsWindowVisible($list)) { $back = [int](((Get-Date) - $t0).TotalMilliseconds); break }
                }
                $hide = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced').HideIcons
                Write-Output ("round " + $round + ": clicked " + $clicked + "  list visible=" + [Shell]::IsWindowVisible($list) +
                              "  HideIcons=" + $hide + "  after " + $back + " ms")
                [Shell]::PressEsc()
                Start-Sleep -Milliseconds 400
                if ($back -ge 0) { break }
            }
            if ($Mode -eq 'inspect') {
                Write-Output 'VERDICT inspect: read only, nothing was clicked'
            } elseif ([Shell]::IsWindowVisible($list)) {
                $hid = $false   # the shell brought it back itself
                Write-Output 'VERDICT restore: the shell menu brought the icons back'
            } else {
                Write-Output 'VERDICT restore: the shell clicks did not bring the list back'
            }
        } else {
            Write-Output ("VERDICT menu ($label): a desktop menu is reachable")
        }
    }
    [Shell]::PressEsc()
    Start-Sleep -Milliseconds 300
} finally {
    if ($hid) { [void][Shell]::ShowWindow($list, [Shell]::SW_SHOW); Start-Sleep -Milliseconds 800 }
}
State 'final'
Write-Output ("windows still open that were not there at the start: " + (@(@([Shell]::Snapshot()) | Where-Object { $was -notcontains $_ }).Count))
