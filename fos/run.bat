@echo off
:: run.bat — boot FOS in QEMU on Windows (same devices as `make run`)
setlocal EnableExtensions
cd /d "%~dp0"

if not exist "boot.img" (
  echo ERROR: boot.img not found. Run build.bat first.
  exit /b 1
)
if not exist "data.img" (
  echo ERROR: data.img not found. Run build.bat first.
  exit /b 1
)

set "QEMU="
for /f "delims=" %%Q in ('where qemu-system-x86_64 2^>nul') do (
  if not defined QEMU set "QEMU=%%Q"
)
if not defined QEMU if exist "%ProgramFiles%\qemu\qemu-system-x86_64.exe" set "QEMU=%ProgramFiles%\qemu\qemu-system-x86_64.exe"
if not defined QEMU if exist "%ProgramFiles%\QEMU\qemu-system-x86_64.exe" set "QEMU=%ProgramFiles%\QEMU\qemu-system-x86_64.exe"
if not defined QEMU if exist "%SystemDrive%\msys64\ucrt64\bin\qemu-system-x86_64.exe" set "QEMU=%SystemDrive%\msys64\ucrt64\bin\qemu-system-x86_64.exe"
if not defined QEMU if exist "%SystemDrive%\msys64\mingw64\bin\qemu-system-x86_64.exe" set "QEMU=%SystemDrive%\msys64\mingw64\bin\qemu-system-x86_64.exe"
if not defined QEMU (
  echo ERROR: qemu-system-x86_64 not found.
  echo Install QEMU, or add it to PATH.
  exit /b 1
)

:: Windows QEMU has no PipeWire/PulseAudio. dsound, sdl, none, and wav work.
if not defined QEMU_AUDIO set "QEMU_AUDIO=dsound"
set "AUDIODEV=-audiodev %QEMU_AUDIO%,id=snd0"
if /i "%QEMU_AUDIO%"=="wav" set "AUDIODEV=-audiodev wav,id=snd0,path=fos-sb16.wav"

echo FOS — type in this window ^(COM1^). The QEMU window is the display.
"%QEMU%" ^
  -name FOS ^
  -drive "format=raw,file=%cd%\boot.img,index=0,media=disk" ^
  -drive "format=raw,file=%cd%\data.img,index=1,media=disk" ^
  -device bochs-display,vgamem=32M ^
  -device vmmouse,i8042=i8042 ^
  %AUDIODEV% ^
  -device sb16,audiodev=snd0,iobase=0x220,irq=5,dma=1 ^
  -m 512M ^
  -serial stdio ^
  -display gtk
if errorlevel 1 (
  echo QEMU exited with error %ERRORLEVEL%.
  pause
)
