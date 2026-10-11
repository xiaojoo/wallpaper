# Starts the settings window the way tools/run-ui.sh does (Qt's bin on PATH) but detached, so it
# outlives the shell that launched it. Without Qt6Widgets.dll on PATH the process dies with a
# "找不到 Qt6Widgets.dll" system error box. ASCII only: PowerShell 5.1 reads .ps1 as GBK.
#
# -Tab is its own named parameter rather than a piece of a quoted $CmdArgs, because
# `powershell -File this.ps1 '--settings-tab shortcuts'` does not bind a multi-word positional: the
# script keeps its default and the window opens on 通用 while the caller believes it asked for another
# page. Found the hard way with two instances running, both on general.
param([string]$Tab = 'general', [switch]$KillFirst, [string]$CmdArgs = '')

$qt = 'D:\Program\Qt\6.11.2\msvc2022_64\bin'
$bin = 'H:\wallpaper\bld\bin\RelWithDebInfo'
$env:PATH = $qt + ';' + $env:PATH
$env:QML_IMPORT_PATH = 'D:\Program\Qt\6.11.2\msvc2022_64\qml'
if ($KillFirst) {
    Stop-Process -Name Wallpaper -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}
& 'D:\Program\CMake\bin\cmake.exe' -E copy_directory 'H:\wallpaper\Wallpaper\qml' ($bin + '\qml') | Out-Null
if ($CmdArgs -eq '') { $CmdArgs = '--settings-tab ' + $Tab }
$p = Start-Process -FilePath ($bin + '\Wallpaper.exe') -ArgumentList $CmdArgs.Split(' ') -WorkingDirectory $bin -PassThru
Start-Sleep -Seconds 6
$alive = @(Get-Process Wallpaper -ErrorAction SilentlyContinue).Count
Write-Output ("started pid=" + $p.Id + " hasWindow=" + ($p.MainWindowHandle -ne 0) + " tab=" + $Tab `
        + " instances=" + $alive + $(if ($alive -gt 1) { "  <- more than one window is polling the pipe" } else { '' }))
