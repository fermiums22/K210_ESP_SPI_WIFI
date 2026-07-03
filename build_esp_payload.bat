@echo off
setlocal
cd /d "%~dp0"

echo === Build ESP8285 payload only ===
echo Repo: %CD%
echo.

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

echo Python command: %PY%
%PY% tools\send_flash_payload.py --dry-run %*
if errorlevel 1 (
  echo.
  echo FAILED. Send console output and logs\flash_payload_*.log here.
  exit /b 1
)

echo.
echo OK. Payload is in out\flash_payload\
echo Next: upload_esp_payload_uart.bat COMx
exit /b 0