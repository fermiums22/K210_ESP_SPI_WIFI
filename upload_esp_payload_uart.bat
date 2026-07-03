@echo off
setlocal
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
if not "%~1"=="" shift

set "EXTRA="
:collect_args
if "%~1"=="" goto args_done
set "EXTRA=%EXTRA% %~1"
shift
goto collect_args
:args_done

echo === Automatic ESP8285 RTOS SPI payload upload via K210 diagnostic-loader ===
echo Repo: %CD%
echo Port: %PORT%
echo Extra:%EXTRA%
echo.

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

echo Python command: %PY%
echo Flow: KSD handshake, SD write, FLASH_ESP, RUN_SPI, monitor pure SPI verdict.
echo.
%PY% tools\send_flash_payload_rtos_spi.py --no-build --sd-uart %PORT% %EXTRA%
if errorlevel 1 (
  echo.
  echo FAILED. Send console output and logs\flash_payload_*.log here.
  exit /b 1
)

echo.
echo OK. Log is in logs\flash_payload_*.log
exit /b 0
