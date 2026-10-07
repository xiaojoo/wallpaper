@echo off
rem tools/mkdecode-build.bat - compile the frame-peek probe. Same reason as mkfixture-build.bat:
rem Git Bash rewrites MSVC arguments that start with a slash, so cl has to be run from a batch file.
call "D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d H:\wallpaper
if not exist build\tmp mkdir build\tmp
set TEMP=H:\wallpaper\build\tmp
set TMP=H:\wallpaper\build\tmp
chcp 65001 >nul
set VSLANG=1033
cl -nologo -EHsc -std:c++20 -O1 -utf-8 -W3 -Fobuild\tmp\ tools\mkdecode.cpp -Febuild\mkdecode.exe mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib windowscodecs.lib ole32.lib
