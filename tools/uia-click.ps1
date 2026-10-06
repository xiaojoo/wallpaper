# Presses a button in the settings window through UIA (no focus steal, works while it is behind
# other windows). -Name matches Accessible.name.
param([Parameter(Mandatory=$true)][string]$Name, [switch]$List)
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, WindowsBase

$cond = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
    [System.Windows.Automation.ControlType]::Button)
$win = New-Object System.Windows.Automation.PropertyCondition(
    [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'Qt6112QWindowIcon')

$root = [System.Windows.Automation.AutomationElement]::RootElement
$scope = [System.Windows.Automation.TreeScope]::Descendants
$btns = $root.FindAll($scope, $cond)
$found = @()
foreach ($b in $btns) {
    if ($b.Current.Name -eq $Name) { $found += $b }
}
Write-Output ("matching buttons: " + $found.Count)
if ($List) {
    foreach ($b in $btns) { Write-Output ("button '" + $b.Current.Name + "'") }
    exit 0
}
if ($found.Count -eq 0) { Write-Output 'NOT FOUND'; exit 1 }
$b = $found[0]
$pat = $null
if (-not $b.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pat)) {
    Write-Output 'NO INVOKE PATTERN'
    exit 2
}
$pat.Invoke()
Write-Output ("invoked: " + $Name)
