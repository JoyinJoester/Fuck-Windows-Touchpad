@echo off
rem Touchpad Gestures -- build + package
rem
rem   build.bat            compile to build\TouchpadGestures.exe
rem   build.bat dist       also copy a clean single-file package to dist\
rem
rem Needs MinGW-w64 g++. If it is not on PATH the msys64 default location is used.

setlocal enabledelayedexpansion

set "ROOT=%~dp0"
cd /d "%ROOT%"

set "CXX=g++"
where g++ >nul 2>&1
if errorlevel 1 (
    if exist "C:\msys64\mingw64\bin\g++.exe" (
        set "CXX=C:\msys64\mingw64\bin\g++.exe"
    ) else (
        echo [x] g++ not found. Install MSYS2 / MinGW-w64, or put g++ on PATH.
        exit /b 1
    )
)

echo [1/2] compiling
if not exist build mkdir build
"%CXX%" -std=c++17 -O2 -Wall -Wextra -Isrc -DUNICODE -D_UNICODE -finput-charset=UTF-8 ^
    src\main.cpp src\Touchpad.cpp src\GestureEngine.cpp src\RawInputSource.cpp ^
    src\Preflight.cpp src\WindowManager.cpp src\Config.cpp src\Action.cpp ^
    src\EventLog.cpp src\UiWindow.cpp ^
    -o build\TouchpadGestures.exe ^
    -mwindows -municode ^
    -lhid -lsetupapi -luser32 -ladvapi32 -ldwmapi -lshell32 -lgdiplus -lole32
if errorlevel 1 (
    echo [x] build failed.
    exit /b 1
)

echo [2/2] done: build\TouchpadGestures.exe

if /i "%~1"=="dist" (
    if not exist dist\TouchpadGestures mkdir dist\TouchpadGestures
    copy /y build\TouchpadGestures.exe dist\TouchpadGestures\ >nul
    if exist tp_gestures.ini copy /y tp_gestures.ini dist\TouchpadGestures\ >nul
    echo     packaged: dist\TouchpadGestures\
)

endlocal
