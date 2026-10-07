$ErrorActionPreference = "Stop"
foreach ($n in @("WallpaperRenderer.exe", "Wallpaper.exe")) {
    Get-CimInstance Win32_Process -Filter "Name='$n'" | ForEach-Object {
        Write-Output ("PID=" + $_.ProcessId + " CMD=" + $_.CommandLine)
    }
}
