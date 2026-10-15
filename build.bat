@echo off
REM Build gtree.exe on Windows using MinGW-w64 gcc.
REM
REM   build.bat           build gtree.exe
REM   build.bat test      build, then run the whole test suite
REM
REM Needs gcc on PATH. MSYS2 (https://www.msys2.org) and TDM-GCC both provide
REM one; any MinGW-w64 toolchain will do. MSVC is not supported -- the sources
REM are C99 and use no compiler extensions, so cl.exe /std:c11 would very
REM likely work, but nobody has tried it.

where gcc >nul 2>nul
if errorlevel 1 (
    echo ERROR: gcc was not found on PATH.
    echo Install MinGW-w64 ^(e.g. via MSYS2: https://www.msys2.org^) and
    echo make sure its bin folder is on PATH, then re-run build.bat.
    exit /b 1
)

gcc -std=c99 -O2 -Wall -Wextra -o gtree.exe src\main.c src\gitignore.c src\treeio.c
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Build succeeded: gtree.exe

if /i "%~1"=="test" (
    echo.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tests\run.ps1"
    exit /b %errorlevel%
)

echo See README.md for instructions on adding this folder to PATH.
