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

echo === Automatic ESP8285 payload upload via K210 UART ===
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
echo Flow: open COM, auto reset K210, KSD handshake, SD write, K210 reset, ESP flash log.
echo.
%PY% tools\send_flash_payload_auto.py --no-build --sd-uart %PORT% --auto-reset dan %EXTRA%
if errorlevel 1 (
  echo.
  echo FAILED. Send console output and logs\flash_payload_*.log here.
  exit /b 1
)

echo.
echo OK. Log is in logs\flash_payload_*.log
exit /b 0
