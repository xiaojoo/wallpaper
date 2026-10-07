# Starts the settings window the way tools/run-ui.sh does (Qt's bin on PATH) but detached, so it
# outlives the shell that launched it. Without Qt6Widgets.dll on PATH the process dies with a
# "找不到 Qt6Widgets.dll" system error box. ASCII only: PowerShell 5.1 reads .ps1 as GBK.
param([string]$CmdArgs = "--settings-tab general")

$qt = 'D:\Program\Qt\6.11.2\msvc2022_64\bin'
$bin = 'H:\wallpaper\bld\bin\RelWithDebInfo'
$env:PATH = $qt + ';' + $env:PATH
$env:QML_IMPORT_PATH = 'D:\Program\Qt\6.11.2\msvc2022_64\qml'
& 'D:\Program\CMake\bin\cmake.exe' -E copy_directory 'H:\wallpaper\Wallpaper\qml' ($bin + '\qml') | Out-Null
$p = Start-Process -FilePath ($bin + '\Wallpaper.exe') -ArgumentList $CmdArgs.Split(' ') -WorkingDirectory $bin -PassThru
Start-Sleep -Seconds 6
Write-Output ("started pid=" + $p.Id + " hasWindow=" + ($p.MainWindowHandle -ne 0))
