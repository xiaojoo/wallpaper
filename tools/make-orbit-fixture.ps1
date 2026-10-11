$ErrorActionPreference = 'Stop'
$desk = [Environment]::GetFolderPath('Desktop')
$pub  = [Environment]::GetFolderPath('CommonDesktopDirectory')
Write-Output ("user desktop : " + $desk)
Write-Output ("public desktop: " + $pub)
Write-Output ("user .lnk count : " + (Get-ChildItem -LiteralPath $desk -Filter *.lnk -File -EA SilentlyContinue).Count)
Write-Output ("public .lnk count: " + (Get-ChildItem -LiteralPath $pub  -Filter *.lnk -File -EA SilentlyContinue).Count)

$log = 'H:\wallpaper\build\orbit-click.log'
Set-Content -LiteralPath $log -Value 'seed' -Encoding ascii

$ws = New-Object -ComObject WScript.Shell
$lnk = $ws.CreateShortcut((Join-Path $desk '!SWorbit-test.lnk'))
$lnk.TargetPath = 'C:\Windows\System32\cmd.exe'
$lnk.Arguments  = '/c echo ring-click>>' + '"' + $log + '"'
$lnk.WorkingDirectory = 'H:\wallpaper\build'
$lnk.WindowStyle = 7
$lnk.Save()
Write-Output ("fixture written: " + (Join-Path $desk '!SWorbit-test.lnk'))
Write-Output ("exists: " + (Test-Path -LiteralPath (Join-Path $desk '!SWorbit-test.lnk')))
