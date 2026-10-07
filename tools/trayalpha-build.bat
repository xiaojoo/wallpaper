@echo off
rem tools/trayalpha-build.bat - compile the taskbar transparency probe. Same reason as
rem mkprobe-build.bat: Git Bash rewrites MSVC arguments that start with a slash, so cl has to be
rem invoked from a batch file.
call "D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d H:\wallpaper
if not exist build\tmp mkdir build\tmp
set TEMP=H:\wallpaper\build\tmp
set TMP=H:\wallpaper\build\tmp
chcp 65001 >nul
set VSLANG=1033
cl -nologo -EHsc -std:c++20 -O1 -utf-8 -W3 -Fobuild\tmp\ tools\trayalpha.cpp -Febuild\trayalpha.exe user32.lib dwmapi.lib advapi32.lib
