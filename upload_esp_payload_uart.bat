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

echo === Upload existing ESP8285 payload via K210 UART ===
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
echo STEP 1: PC opens COM and listens for K210 boot SD service.
echo No SD write and no ESP flash at this stage.
echo Follow the action line printed by Python.
echo.
%PY% tools\send_flash_payload.py --no-build --sd-uart %PORT% %EXTRA%
if errorlevel 1 (
  echo.
  echo FAILED. Send console output and logs\flash_payload_*.log here.
  exit /b 1
)

echo.
echo OK. Log is in logs\flash_payload_*.log
exit /b 0
