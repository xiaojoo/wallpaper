@echo off
rem tools/mfprobe-build.bat - compile the Media Foundation capability probe. cl has to run from a
rem batch file because Git Bash rewrites MSVC arguments that start with a slash.
call "D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d H:\wallpaper
if not exist build\tmp mkdir build\tmp
set TEMP=H:\wallpaper\build\tmp
set TMP=H:\wallpaper\build\tmp
chcp 65001 >nul
set VSLANG=1033
cl -nologo -EHsc -std:c++20 -O1 -utf-8 -W3 -Fobuild\tmp\ tools\mfprobe.cpp -Febuild\mfprobe.exe mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib d3d11.lib ole32.lib
