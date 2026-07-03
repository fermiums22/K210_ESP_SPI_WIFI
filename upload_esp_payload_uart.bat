@echo off
setlocal
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
shift /1

echo === Upload existing ESP8285 payload via K210 UART ===
echo Repo: %CD%
echo Port: %PORT%
echo.

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

echo Python command: %PY%
echo Start this script, then press RESET on K210 if it is already showing the normal screen.
echo.
%PY% tools\send_flash_payload.py --no-build --sd-uart %PORT% %*
if errorlevel 1 (
  echo.
  echo FAILED. Send console output and logs\flash_payload_*.log here.
  exit /b 1
)

echo.
echo OK. Log is in logs\flash_payload_*.log
exit /b 0