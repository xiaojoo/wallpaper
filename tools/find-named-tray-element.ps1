# Look for one tray icon by name anywhere in the taskbar tree, whatever control type the shell
# chose for it (promoted button, overflow item, image...). Written because the hidden-icons flyout
# has to be open before its buttons appear in UIA, and a click cannot be injected right now.
param([string]$Match = "Wallpaper", [string]$OutFile = "$env:TEMP\tray-name.txt")

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE = [System.Windows.Automation.AutomationElement]
$TS = [System.Windows.Automation.TreeScope]

$root = $AE::RootElement
$lines = @()
foreach ($cls in @("Shell_TrayWnd", "NotifyIconOverflowWindow", "TopLevelWindowForOverflowXamlIsland")) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ClassNameProperty, $cls)
    foreach ($bar in $root.FindAll($TS::Children, $cond)) {
        foreach ($e in $bar.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) {
            $n = ""
            try { $n = $e.Current.Name } catch { }
            $help = ""
            try { $help = $e.Current.HelpText } catch { }
            if ($n -notmatch $Match -and $help -notmatch $Match) { continue }
            $r = $e.Current.BoundingRectangle
            $lines += ("{0} | ct={1} | name=[{2}] | help=[{3}] | x={4} y={5} w={6} h={7} offscreen={8}" -f `
                $cls, $e.Current.ControlType.ProgrammaticName, $n, $help,
                [math]::Round($r.X), [math]::Round($r.Y), [math]::Round($r.Width), [math]::Round($r.Height),
                $e.Current.IsOffscreen)
        }
    }
}
$lines | Set-Content -Path $OutFile -Encoding UTF8
Write-Output ("MATCHED=" + $lines.Count)
