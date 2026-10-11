# tools/probe-icon-user-restore.ps1 - how many clicks of the shell's own menu get the icons back.
#
# Task #31's acceptance question, asked of the state the ENGINE left rather than of a hide this script
# performed itself. The earlier probe hid the list with ShowWindow and then reached for the menu; that
# answered "does a menu exist while hidden" (it does) but not "does clicking it undo what our process
# did", which is the property a killed renderer has to leave behind.
#
# So: the caller hides the icons first (`--ctl icons on`), this right-clicks the desktop, opens 查看,
# reports what the icons item claims, clicks it, and waits for the list to become visible again - once
# per round, up to -Rounds, because the item may need an Off click and then an On one.
#
#   -Rounds 3        how many times to click it at most
#   Leaves the desktop in whatever state the shell's own clicks produced, and says so.
# ASCII only: PowerShell 5.1 parses this file under a non-UTF8 codepage.
param(
    [int]$Rounds = 3,
    [int]$PX = 3500,
    [int]$PY = 300
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase

$src = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

public static class U {
  [StructLayout(LayoutKind.Sequential)] public struct PPOINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(PPOINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
  [DllImport("user32.dll")] static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint f);

  const uint RIGHTDOWN = 0x0008, RIGHTUP = 0x0010, GA_ROOT = 2;

  public static string Cls(IntPtr h) { var sb=new StringBuilder(256); GetClassNameW(h,sb,256); return sb.ToString(); }
  public static string Title(IntPtr h) { var sb=new StringBuilder(256); GetWindowTextW(h,sb,256); return sb.ToString(); }
  public static string Name(IntPtr h) {
    if (h == IntPtr.Zero) return "<none>";
    RECT r; GetWindowRect(h, out r);
    return Cls(h) + " @" + r.L + "," + r.T + "-" + r.R + "," + r.B;
  }
  static EnumProc keep; static List<IntPtr> sink;
  public static List<IntPtr> Tops() {
    sink = new List<IntPtr>();
    keep = (h, a) => { sink.Add(h); return true; };
    EnumWindows(keep, IntPtr.Zero);
    return sink;
  }
  static EnumProc keepKid; static List<IntPtr> kidSink;
  public static List<IntPtr> Kids(IntPtr root) {
    kidSink = new List<IntPtr>();
    keepKid = (h, a) => { kidSink.Add(h); return true; };
    EnumChildWindows(root, keepKid, IntPtr.Zero);
    return kidSink;
  }
  public static IntPtr ListView() {
    foreach (var h in Tops()) { if (Cls(h) != "Progman") continue;
      foreach (var k in Kids(h)) if (Cls(k) == "SysListView32") return k; }
    return IntPtr.Zero;
  }
  public static List<string> Popups() {
    var o = new List<string>();
    foreach (var h in Tops()) if (IsWindowVisible(h) && Cls(h).StartsWith("Microsoft.UI.Content.Popup"))
      o.Add(((long)h).ToString() + "|" + Name(h));
    return o;
  }
  public static bool IsShell(IntPtr h) {
    if (h == IntPtr.Zero) return false;
    var c = Cls(h);
    if (c == "Progman" || c == "SHELLDLL_DefView" || c == "SysListView32" || c == "WorkerW") return true;
    var root = GetAncestor(h, GA_ROOT);
    return root != IntPtr.Zero && Cls(root) == "Progman";
  }
  public static IntPtr HwndAt(int x, int y) { var p = new PPOINT { X = x, Y = y }; return WindowFromPoint(p); }
  public static void Dismiss() { for (int i = 0; i < 3; i++) { keybd_event(0x1B,0,0,IntPtr.Zero); keybd_event(0x1B,0,2,IntPtr.Zero); Thread.Sleep(150); } }
  public static void RightClick(int x, int y) {
    SetCursorPos(x, y); Thread.Sleep(120);
    mouse_event(RIGHTDOWN,0,0,0,IntPtr.Zero); Thread.Sleep(60);
    mouse_event(RIGHTUP,0,0,0,IntPtr.Zero); Thread.Sleep(500);
  }
}
'@
Add-Type -TypeDefinition $src -Language CSharp -ReferencedAssemblies System.Runtime.InteropServices

$uia = @'
using System;
using System.Collections.Generic;
using System.Windows.Automation;

public static class M {
  static List<AutomationElement> Items(AutomationElement root) {
    var o = new List<AutomationElement>();
    foreach (AutomationElement e in root.FindAll(TreeScope.Descendants,
             new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))) o.Add(e);
    return o;
  }
  public static List<string> Names(IntPtr h) {
    var o = new List<string>();
    if (h == IntPtr.Zero) return o;
    AutomationElement el; try { el = AutomationElement.FromHandle(h); } catch { return o; }
    if (el == null) return o;
    foreach (var m in Items(el)) {
      string st = ""; object p;
      if (m.TryGetCurrentPattern(TogglePattern.Pattern, out p)) st = " [" + ((TogglePattern)p).Current.ToggleState + "]";
      o.Add(m.Current.Name + st);
    }
    return o;
  }
  public static string ToggleFirst(IntPtr h, string[] needles) {
    if (h == IntPtr.Zero) return "<no window>";
    AutomationElement el; try { el = AutomationElement.FromHandle(h); } catch { return "<unreadable>"; }
    if (el == null) return "<no element>";
    foreach (var m in Items(el))
      foreach (var n in needles) {
        if (m.Current.Name.IndexOf(n, StringComparison.OrdinalIgnoreCase) < 0) continue;
        object p;
        if (m.TryGetCurrentPattern(TogglePattern.Pattern, out p)) { var w = ((TogglePattern)p).Current.ToggleState; ((TogglePattern)p).Toggle(); return m.Current.Name + " was " + w; }
        if (m.TryGetCurrentPattern(InvokePattern.Pattern, out p)) { ((InvokePattern)p).Invoke(); return m.Current.Name + " invoked (no toggle)"; }
        return m.Current.Name + " has neither";
      }
    return "<not found>";
  }
}
'@
Add-Type -TypeDefinition $uia -Language CSharp -ReferencedAssemblies UIAutomationClient, UIAutomationTypes, WindowsBase

# Built from code points: a literal Chinese string in this file is read as GBK by PowerShell 5.1.
$view = ,([string][char]0x67E5 + [char]0x770B) + 'View'
$icons = ,([string][char]0x684C + [char]0x9762 + [char]0x56FE + [char]0x6807) + 'desktop icons'

$list = [U]::ListView()
if ($list -eq [IntPtr]::Zero) { Write-Output 'ABORT: no icon list'; exit 1 }

function Snap { @( [U]::Popups() ) }

for ($r = 1; $r -le $Rounds; $r++) {
    [U]::Dismiss()
    Start-Sleep -Milliseconds 300
    $before = Snap
    [U]::RightClick($PX, $PY)
    $after = Snap
    $new = @($after | Where-Object { $before -notcontains $_ })
    if ($new.Count -eq 0) { Write-Output ("round " + $r + ": no menu window appeared"); continue }
    $menu = [IntPtr]([int64](($new[0] -split '\|')[0]))
    Write-Output ("round " + $r + " menu: " + [U]::Name($menu))
    Write-Output ("  top items: " + (([M]::Names($menu)) -join ' | '))

    $b2 = Snap
    $opened = [M]::ToggleFirst($menu, $view)
    Start-Sleep -Milliseconds 600
    $n2 = @(Snap | Where-Object { $b2 -notcontains $_ })
    $sub = $menu
    if ($n2.Count -gt 0) { $sub = [IntPtr]([int64](($n2[0] -split '\|')[0])) }
    Write-Output ("  opened " + $opened + " -> " + (([M]::Names($sub)) -join ' | '))

    $t0 = Get-Date
    $clicked = [M]::ToggleFirst($sub, $icons)
    $ms = -1
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        if ([U]::IsWindowVisible($list)) { $ms = [int](((Get-Date) - $t0).TotalMilliseconds); break }
    }
    $reg = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced').HideIcons
    Write-Output ("  clicked " + $clicked + "  list visible=" + [U]::IsWindowVisible($list) +
                  "  HideIcons=" + $reg + "  after " + $ms + " ms")
    if ($ms -ge 0) {
        Write-Output ("VERDICT: the user gets his icons back in " + $r + " click(s) of the shell's own item, " + $ms + " ms after the last one")
        break
    }
}
if (-not [U]::IsWindowVisible($list)) { Write-Output 'VERDICT: the shell menu never brought the list back' }
[U]::Dismiss()
Write-Output ("final list visible=" + [U]::IsWindowVisible($list) + "  popups left=" + (Snap).Count)
