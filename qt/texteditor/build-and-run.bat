@echo off
setlocal

rem Qt 6.6.1 MinGW kit used by this project.
set "PATH=C:\Qt\Tools\mingw1120_64\bin;C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;C:\Qt\6.6.1\mingw_64\bin;%PATH%"

rem %~dp0 has a trailing backslash, which would escape the closing quote.
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "BUILD=%SRC%\build"

echo Configuring...
cmake -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.6.1/mingw_64 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :failed

echo Building...
cmake --build "%BUILD%"
if errorlevel 1 goto :failed

echo Copying Qt and MinGW libraries...
windeployqt --no-translations --compiler-runtime "%BUILD%\texteditor.exe"
if errorlevel 1 goto :failed

echo Starting Text Editor...
start "" "%BUILD%\texteditor.exe" %*
exit /b 0

:failed
echo.
echo Build failed.
pause
exit /b 1
