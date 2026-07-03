@echo off
setlocal
cd /d "%~dp0"

echo === K210 ESP8285 WiFi-SPI flash helper ===
echo Repo: %CD%
echo.

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

echo Python command: %PY%
%PY% tools\send_flash_payload.py %*
if errorlevel 1 (
  echo.
  echo FAILED. Send console output and logs\flash_payload_*.log here.
  exit /b 1
)

echo.
echo OK. Log is in logs\flash_payload_*.log
exit /b 0
