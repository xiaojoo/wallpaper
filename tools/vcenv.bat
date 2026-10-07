@echo off
rem tools/vcenv.bat - dump the x64 MSVC environment so a bash script can import it.
rem cmd is called as "cmd //c vcenv.bat" from the directory the output file goes in: Git Bash
rem eats backslashes in arguments, so the script takes no arguments at all.
call "D:\Program\VisualStudio\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set
