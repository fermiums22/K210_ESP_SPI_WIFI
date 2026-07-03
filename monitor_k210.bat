@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"

echo === K210 UART monitor, no reset ===
echo Port: %PORT%
echo Baud: 921600
echo DTR/RTS: forced inactive, should not reset/boot K210.
echo Press Ctrl+C to exit.
echo.

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  py -3 tools\k210_monitor.py %PORT% --baud 921600
) else (
  python tools\k210_monitor.py %PORT% --baud 921600
)
