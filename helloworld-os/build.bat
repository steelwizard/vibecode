@echo off
:: build.bat - Build and (optionally) run the Hello World OS
:: Usage:
::   build.bat        -- assemble only
::   build.bat run    -- assemble then launch in QEMU
::
:: Requires: nasm, qemu-system-i386 (or qemu-system-x86_64) on PATH

setlocal

set SRC=boot.asm
set IMG=boot.img

:: ── Check for NASM ──────────────────────────────────────────────────
where nasm >nul 2>&1
if errorlevel 1 (
    echo ERROR: NASM not found. Install it from https://nasm.us and add it to your PATH.
    exit /b 1
)

:: ── Assemble ────────────────────────────────────────────────────────
echo Assembling %SRC% -^> %IMG% ...
nasm -f bin %SRC% -o %IMG%
if errorlevel 1 exit /b %errorlevel%

echo OK

:: ── Run in QEMU (only when first argument is "run") ─────────────────
if /i not "%1"=="run" goto :eof

where qemu-system-i386 >nul 2>&1
if not errorlevel 1 (
    set QEMU=qemu-system-i386
    goto :launch
)

where qemu-system-x86_64 >nul 2>&1
if not errorlevel 1 (
    set QEMU=qemu-system-x86_64
    goto :launch
)

echo ERROR: QEMU not found. Install it from https://www.qemu.org/download/ and add it to your PATH.
exit /b 1

:launch
echo.
echo Launching %QEMU% ...
echo Tip: press Ctrl+A then X inside the QEMU console to quit.
echo.
%QEMU% -drive format=raw,file=%IMG% -nographic
