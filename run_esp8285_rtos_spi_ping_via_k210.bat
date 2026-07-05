@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

echo === ESP8285 official ESP8266_RTOS_SDK SPI ping via K210 ===
echo Repo: %CD%
echo KSD:  %PORT% @ 921600
echo Flow: MSYS build -^> KSD PUT -^> FLASH_ESP -^> KSD RUN_SPI -^> wait KESP frame
echo No Arduino. No PlatformIO.
echo.

call run_esp8285_rtos_hello_via_k210.bat %PORT%
if errorlevel 1 exit /b 1

%PY% tools\run_spi_ping_via_k210.py --sd-uart %PORT%
exit /b %ERRORLEVEL%
