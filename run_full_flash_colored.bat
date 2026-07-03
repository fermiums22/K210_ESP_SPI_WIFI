@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"

echo === ESP8285 official ESP8266_RTOS_SDK hello via K210 ===
echo Port: %PORT%
echo No Arduino. No PlatformIO.
echo.

call run_esp8285_rtos_hello_via_k210.bat %PORT%
exit /b %ERRORLEVEL%
