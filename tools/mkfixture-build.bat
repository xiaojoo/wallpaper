@echo off
rem tools/mkfixture-build.bat - compile the fixture generator with MSVC.
rem Run it as "cmd //c mkfixture-build.bat" from the tools directory: Git Bash rewrites arguments
rem that start with a slash (a "/nologo" becomes a path under D:\Program\Git), so the whole command
rem line lives here instead of in the shell script.
call "D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d H:\wallpaper
if not exist build\tmp mkdir build\tmp
set TEMP=H:\wallpaper\build\tmp
set TMP=H:\wallpaper\build\tmp
rem Without this MSVC writes its diagnostics in GBK and they come out as mojibake through bash.
chcp 65001 >nul
set VSLANG=1033
cl -nologo -EHsc -std:c++20 -O2 -utf-8 -W3 -Fobuild\tmp\ tools\mkfixture.cpp -Febuild\mkfixture.exe mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib ole32.lib
