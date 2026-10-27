@echo off
rem Launch Touchpad Gestures. Double-clicking the exe directly does the same
rem thing -- this file exists so the window can be started from a console when
rem you want to pass switches such as --log.

set "EXE=%~dp0build\TouchpadGestures.exe"
if not exist "%EXE%" set "EXE=%~dp0TouchpadGestures.exe"

if not exist "%EXE%" (
  echo.
  echo   [!] TouchpadGestures.exe not found.
  echo       Build it first:  build.bat
  echo.
  pause
  exit /b 1
)

start "" "%EXE%" %*
