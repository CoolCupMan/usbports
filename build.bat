@echo off
rem Build USB Port Viewer with MinGW-w64 (MSYS2). Run from this folder.
cd /d "%~dp0"
windres app.rc -O coff -o app.res || goto :error
g++ ACTIVEUSBPORTS.cpp app.res -std=c++17 -O2 -Wall -municode -mwindows -static -lsetupapi -lcfgmgr32 -lcomctl32 -o ACTIVEUSBPORTS.exe || goto :error
echo Built ACTIVEUSBPORTS.exe
exit /b 0
:error
echo Build failed.
exit /b 1
