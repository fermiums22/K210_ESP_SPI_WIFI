@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PORT=%~1"
set "HOST=%~2"
set "SIZE=%~3"

if "%PORT%"=="" set "PORT=COM12"
if "%HOST%"=="" set "HOST=192.168.0.132"
if "%SIZE%"=="" set "SIZE=1048576"

set "KESP_NO_PAUSE=1"

echo === Full ESP flash + PC -^> WiFi -^> SPI -^> SD RW test ===
echo Repo: %CD%
echo KSD:  %PORT%
echo ESP:  %HOST%:18080
echo Size: %SIZE%
echo.

call run_full_flash_colored.bat %PORT%
if errorlevel 1 exit /b 1

call run_pc_wifi_spi_sd_fast_test.bat %PORT% %HOST% %SIZE%
exit /b %ERRORLEVEL%
