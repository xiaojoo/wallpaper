# Find the notification-area buttons on the taskbar and report name + rect.
# Results go to a UTF-8 file: PS 5.1 reads this script as GBK, so keep the code ASCII and
# let the Chinese names land in the output file instead of the console.
param([string]$OutFile = "$env:TEMP\tray-icons.txt", [switch]$IncludeHidden)

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]
$CT = [System.Windows.Automation.ControlType]

$root = $AE::RootElement
$lines = @()

foreach ($cls in @("Shell_TrayWnd", "NotifyIconOverflowWindow", "TopLevelWindowForOverflowXamlIsland")) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, $cls)
    $bars = $root.FindAll($TS::Children, $cond)
    foreach ($bar in $bars) {
        $bc = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Button)
        if (-not $IncludeHidden) {
            $bc = New-Object System.Windows.Automation.AndCondition($bc,
                (New-Object System.Windows.Automation.PropertyCondition($AE::IsOffscreenProperty, $false)))
        }
        foreach ($b in $bar.FindAll($TS::Descendants, $bc)) {
            $r = $b.Current.BoundingRectangle
            $lines += ("{0} | name=[{1}] | x={2} y={3} w={4} h={5}" -f $cls, $b.Current.Name,
                       [math]::Round($r.X), [math]::Round($r.Y), [math]::Round($r.Width), [math]::Round($r.Height))
        }
    }
}

$lines | Set-Content -Path $OutFile -Encoding UTF8
Write-Output ("FOUND=" + $lines.Count)
