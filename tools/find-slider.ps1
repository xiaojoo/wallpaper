# Finds the self-drawn slider in the settings window through UIA and prints its rect, so a scripted
# click can land on a known fraction of the track. ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param([switch]$All)

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase

$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Slider)
$items = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
Write-Output ("sliders found: " + $items.Count)
foreach ($s in $items) {
    $r = $s.Current.BoundingRectangle
    Write-Output ("name='" + $s.Current.Name + "' rect=" + [int]$r.X + "," + [int]$r.Y + " " +
                  [int]$r.Width + "x" + [int]$r.Height)
    if ($All) {
        $p = $s.GetCurrentPattern([System.Windows.Automation.RangeValuePattern]::Pattern)
        if ($null -ne $p) {
            Write-Output ("  RangeValue present: value=" + $p.Current.Value + " min=" + $p.Current.Minimum)
        } else {
            Write-Output "  no RangeValue pattern (the control is self-drawn, only its rect is exposed)"
        }
    }
}
