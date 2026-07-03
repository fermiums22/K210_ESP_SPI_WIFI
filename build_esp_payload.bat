@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo === Build ESP8285 payload: official ESP8266_RTOS_SDK via MSYS ===
echo Repo: %CD%
echo No Arduino. No PlatformIO.
echo.

call run_esp8285_rtos_hello_build.bat %*
exit /b %ERRORLEVEL%
