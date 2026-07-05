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

set "K210_REPO=%~dp0..\K210_AI_V7s_Plus"


echo === PC to WiFi to SPI to SD strict full test ===
echo Repo: %CD%
echo KSD:  %PORT% @ 921600
echo Size: %SIZE% bytes
echo Official ESP8266_RTOS_SDK path only.
echo.

if not exist "%K210_REPO%\tools\ksd_console_rw_test.py" (
  echo ERROR: missing K210 console smoke: %K210_REPO%\tools\ksd_console_rw_test.py
  echo Run the full fetch/reset command so K210_AI_V7s_Plus is on origin/main.
  exit /b 1
)

echo Running K210 KSD console/read-write smoke...
%PY% "%K210_REPO%\tools\ksd_console_rw_test.py" --port %PORT% --baud 921600 --size 4096
if errorlevel 1 exit /b 1

echo Running ESP8285 RTOS SDK hello build...
call run_esp8285_rtos_hello_build.bat
if errorlevel 1 exit /b 1

%PY% tools\upload_rtos_hello_via_k210.py --sd-uart %PORT%
if errorlevel 1 exit /b 1

%PY% tools\pc_wifi_spi_sd_rw_test.py --sd-uart %PORT% --size %SIZE%
exit /b %ERRORLEVEL%
