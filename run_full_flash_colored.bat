@echo off
setlocal EnableExtensions

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"

cd /d "%~dp0"

echo === K210 + ESP8285 ESP-only flash ===
echo Port: %PORT%
echo Flow: ESP build, SD upload, K210 reset, ESP boot monitor.
echo K210 app build/flash is intentionally skipped.
echo.

call build_esp_payload.bat
if errorlevel 1 goto fail

call upload_esp_payload_uart.bat %PORT%
if errorlevel 1 goto fail

echo.
echo DONE. Exit code: 0
echo.
echo Window will not close automatically.
pause
exit /b 0

:fail
echo.
echo FAILED. Exit code: 1
echo.
echo Window will not close automatically.
pause
exit /b 1
