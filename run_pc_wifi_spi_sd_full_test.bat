@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
set "SIZE=%~2"
if "%SIZE%"=="" set "SIZE=65536"

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

echo === PC to WiFi to SPI to SD strict full test ===
echo Repo: %CD%
echo KSD:  %PORT% @ 921600
echo Size: %SIZE% bytes
echo Official ESP8266_RTOS_SDK path only.
echo.

call run_esp8285_rtos_hello_build.bat
if errorlevel 1 exit /b 1

%PY% tools\upload_rtos_hello_via_k210.py --sd-uart %PORT%
if errorlevel 1 exit /b 1

%PY% tools\pc_wifi_spi_sd_rw_test.py --sd-uart %PORT% --size %SIZE%
exit /b %ERRORLEVEL%
