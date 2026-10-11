# tools/probe-orbit-fallthrough.ps1 - does the desktop still get the clicks the ring did not claim?
#
# Why not WindowFromPoint: that call reports the window it finds by geometry and never runs
# WM_NCHITTEST, so our full-monitor click layer answers for every desktop pixel no matter what the
# hit test returns. Measured that way the stop-loss reads "the layer owns everything" whether it does
# or not - an instrument that cannot report the bad case. This one judges by EFFECT, through the
# shell's own selection state:
#
#   select a desktop icon through UI Automation,
#   left-click an EMPTY desktop point that is outside every ring box,
#   the shell clears the selection only if that click actually reached the desktop.
#
# -Mode control buries the desktop under an opaque topmost window and repeats: the selection must then
# SURVIVE. If it clears in that configuration too, this script proves nothing and the -Mode fallthrough
# result is worthless, so run both and read them together.
#
# The icon itself sits under whatever application window happens to be open on this desktop right now,
# which is fine: nothing here clicks the icon, only selects it programmatically and reads it back.
# ASCII only: PowerShell 5.1 parses this file under a non-UTF8 codepage.
param(
    [ValidateSet('fallthrough','control')][string]$Mode = 'fallthrough',
    [int]$Icon = 1,
    [int]$PX = 3400,
    [int]$PY = 1000
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase, System.Windows.Forms, System.Drawing

$src = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;

public static class Fall
{
    [StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PT p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);

    const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;

    public static string OwnerAt(int x, int y)
    {
        PT p = new PT(); p.X = x; p.Y = y;
        IntPtr h = WindowFromPoint(p);
        if (h == IntPtr.Zero) return "none";
        var cls = new StringBuilder(300); int n = GetClassNameW(h, cls, 300);
        return n > 0 ? cls.ToString(0, n) : "?";
    }

    public static void Click(int x, int y)
    {
        SetCursorPos(x, y);
        Thread.Sleep(80);
        mouse_event(LEFTDOWN, 0, 0, 0, IntPtr.Zero);
        Thread.Sleep(60);
        mouse_event(LEFTUP, 0, 0, 0, IntPtr.Zero);
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Runtime.InteropServices, System.Drawing

$uiSrc = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows.Automation;

public static class DeskIcons
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassNameW(IntPtr h, StringBuilder s, int m);

    static string WinClass(int hwnd)
    {
        if (hwnd == 0) return "";
        var sb = new StringBuilder(300);
        int n = GetClassNameW((IntPtr)hwnd, sb, 300);
        return n > 0 ? sb.ToString(0, n) : "";
    }

    static AutomationElement FindList()
    {
        var cond = new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.List);
        var all = AutomationElement.RootElement.FindAll(TreeScope.Descendants, cond);
        AutomationElement best = null; int bestCount = -1;
        foreach (AutomationElement e in all)
        {
            var items = e.FindAll(TreeScope.Children,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem));
            if (WinClass(e.Current.NativeWindowHandle) == "SysListView32" && items.Count > bestCount)
            { best = e; bestCount = items.Count; }
        }
        return best;
    }

    static List<AutomationElement> Items()
    {
        var l = FindList();
        var o = new List<AutomationElement>();
        if (l == null) return o;
        foreach (AutomationElement e in l.FindAll(TreeScope.Children,
                 new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem)))
            o.Add(e);
        return o;
    }

    public static string Select(int idx)
    {
        var items = Items();
        if (idx >= items.Count) return "index out of range (" + items.Count + " items)";
        object pat;
        if (!items[idx].TryGetCurrentPattern(SelectionItemPattern.Pattern, out pat)) return "no selection pattern";
        ((SelectionItemPattern)pat).Select();
        return items[idx].Current.Name;
    }

    public static string Selected(int idx)
    {
        var items = Items();
        if (idx >= items.Count) return "out of range";
        object pat;
        if (!items[idx].TryGetCurrentPattern(SelectionItemPattern.Pattern, out pat)) return "no pattern";
        return ((SelectionItemPattern)pat).Current.IsSelected ? "SELECTED" : "not selected";
    }

    public static void UnselectAll()
    {
        foreach (AutomationElement e in Items())
        {
            object pat;
            if (e.TryGetCurrentPattern(SelectionItemPattern.Pattern, out pat))
                if (((SelectionItemPattern)pat).Current.IsSelected) ((SelectionItemPattern)pat).RemoveFromSelection();
        }
    }
}
'@
Add-Type -TypeDefinition $uiSrc -Language CSharp -ReferencedAssemblies UIAutomationClient, UIAutomationTypes, WindowsBase, System.Runtime.InteropServices

function Get-RingBoxes {
    $dir = 'H:\wallpaper\bld\bin\RelWithDebInfo\logs'
    $log = Get-ChildItem -Path $dir -Filter 'renderer-*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $line = Select-String -Path $log.FullName -Pattern 'orbit rects hover=' | Select-Object -Last 1
    if (-not $line) { throw 'no orbit rects line - turn the ring on and arm the layer first' }
    $b = @()
    foreach ($m in [regex]::Matches($line.Line, ' (\d+):(\d+),(\d+)\+(\d+)')) {
        $b += [pscustomobject]@{ Slot = [int]$m.Groups[1].Value; CX = [int]$m.Groups[2].Value
                                 CY = [int]$m.Groups[3].Value; H = [int]$m.Groups[4].Value }
    }
    , $b
}

function Inside-Any($boxes, $x, $y) {
    foreach ($b in $boxes) {
        if ([Math]::Abs($x - $b.CX) -le $b.H -and [Math]::Abs($y - $b.CY) -le $b.H) { return $b.Slot }
    }
    return -1
}

$boxes = (Get-RingBoxes)[-1]
$inside = Inside-Any $boxes $PX $PY
Write-Output ("click point $PX,$PY is inside a ring box: slot " + $inside + "  (must be -1)")
if ($inside -ge 0) { throw 'pick a point outside every ring box - this measures the fallthrough, not a click' }
Write-Output ("WindowFromPoint there reports: " + ([Fall]::OwnerAt($PX, $PY)) + "  (geometry only, not what the click does)")

[DeskIcons]::UnselectAll()
$name = [DeskIcons]::Select($Icon)
Write-Output ("selected icon $Icon ('$name') through UIA without clicking it: " + [DeskIcons]::Selected($Icon))
if ([DeskIcons]::Selected($Icon) -ne 'SELECTED') { throw 'cannot start the measurement, the icon never got selected' }

$form = $null
if ($Mode -eq 'control') {
    $form = New-Object System.Windows.Forms.Form
    $form.FormBorderStyle = 'None'
    $form.StartPosition = 'Manual'
    $form.Bounds = [System.Drawing.Rectangle]::new(0, 0, 3840, 2160)
    $form.TopMost = $true
    $form.ShowInTaskbar = $false
    $form.BackColor = [System.Drawing.Color]::Gray
    $form.Show()
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 400
    Write-Output 'the desktop is now buried under an opaque topmost window'
}

[Fall]::Click($PX, $PY)
Start-Sleep -Milliseconds 500
$after = [DeskIcons]::Selected($Icon)
if ($form) { $form.Close() }

Write-Output ("after clicking an empty desktop point: icon $Icon is " + $after)
if ($Mode -eq 'fallthrough') {
    if ($after -eq 'not selected') { Write-Output 'VERDICT fallthrough: THE DESKTOP TOOK THE CLICK - selection cleared, the ring layer did not eat it' }
    else { Write-Output 'VERDICT fallthrough: THE CLICK WAS SWALLOWED - the desktop did not see it' }
} else {
    if ($after -eq 'SELECTED') { Write-Output 'VERDICT control: selection survived a blocked click - the instrument can report the bad case' }
    else { Write-Output 'VERDICT control: BROKEN - the selection cleared through an opaque window, so the fallthrough verdict means nothing' }
}
[DeskIcons]::UnselectAll()
