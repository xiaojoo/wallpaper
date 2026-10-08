@echo off
rem tools/dxvaspike-build.bat - compile the D3D11VA zero-copy probe. Same reason as the other probe
rem build scripts: Git Bash rewrites MSVC arguments that start with a slash, so cl runs from a batch file.
call "D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d H:\wallpaper
if not exist build\tmp mkdir build\tmp
set TEMP=H:\wallpaper\build\tmp
set TMP=H:\wallpaper\build\tmp
chcp 65001 >nul
set VSLANG=1033
cl -nologo -EHsc -std:c++20 -O1 -utf-8 -W3 ^
   -Ibuild\ffm-lgpl\include -Fobuild\tmp\ tools\dxvaspike.cpp ^
   -Febuild\dxvaspike.exe ^
   build\ffm-lgpl\lib\avcodec.lib build\ffm-lgpl\lib\avformat.lib build\ffm-lgpl\lib\avutil.lib ^
   d3d11.lib dxgi.lib ole32.lib user32.lib
