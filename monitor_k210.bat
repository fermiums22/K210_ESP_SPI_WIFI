@echo off
setlocal EnableExtensions
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"

echo === K210 UART monitor ===
echo Port: %PORT%
echo Baud: 921600
echo Press Ctrl+] then q to exit miniterm.
echo.

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  py -3 -m serial.tools.miniterm %PORT% 921600 --raw
) else (
  python -m serial.tools.miniterm %PORT% 921600 --raw
)
