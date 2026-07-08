@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "HOST=%~1"
set "APPBIN=%~2"

if "%HOST%"=="" set "HOST=192.168.4.1"
if "%APPBIN%"=="" set "APPBIN=..\K210_AI_V7s_Plus\build_app_slot0\k210_app_slot0.bin"

if not exist "%APPBIN%" (
  echo ERROR: K210 app bin not found: %APPBIN%
  echo Build it first in ..\K210_AI_V7s_Plus with build_k210_app_slot0.bat
  exit /b 1
)

echo === WiFi flash K210 app slot0 ===
echo ESP:  %HOST%:18080
echo BIN:  %APPBIN%
echo Name: app_slot0.bin
echo.
echo K210 app must be running RUN_SPI. It will save to SD, flash SPI3 slot0, verify, then reset.
echo.

call wifi_put_file.bat "%HOST%" "%APPBIN%" app_slot0.bin
exit /b %ERRORLEVEL%
