@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PORT=%~1"
set "HOST=%~2"
set "SIZE=%~3"

if "%PORT%"=="" set "PORT=COM12"
if "%HOST%"=="" set "HOST=192.168.0.132"
if "%SIZE%"=="" set "SIZE=1048576"

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

echo === PC -^> WiFi -^> SPI -^> SD fast read/write test ===
echo Repo: %CD%
echo KSD:  %PORT% @ 921600
echo ESP:  %HOST%:18080
echo Size: %SIZE%
echo.

%PY% tools\pc_wifi_spi_sd_rw_test.py --sd-uart %PORT% --host %HOST% --size %SIZE% --remote t1m.bin
exit /b %ERRORLEVEL%
