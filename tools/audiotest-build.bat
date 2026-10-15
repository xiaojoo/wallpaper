@echo off
rem tools/audiotest-build.bat - build the standalone WASAPI tone test (build\audiotest.exe).
rem cl runs from a batch file because Git Bash rewrites MSVC arguments that start with a slash.
call "D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d H:\wallpaper
if not exist build\tmp mkdir build\tmp
set TEMP=H:\wallpaper\build\tmp
set TMP=H:\wallpaper\build\tmp
chcp 65001 >nul
set VSLANG=1033
cl -nologo -EHsc -std:c++20 -O1 -utf-8 -W3 ^
   -Fobuild\tmp\ tools\audiotest.cpp ^
   -Febuild\audiotest.exe ^
   ole32.lib user32.lib gdi32.lib comctl32.lib
if errorlevel 1 goto :eof
if not exist build\tmp mkdir build\tmp
echo built build\audiotest.exe
