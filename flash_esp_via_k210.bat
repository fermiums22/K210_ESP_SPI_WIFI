@echo off
setlocal
cd /d "%~dp0"

echo === K210 ESP8285 WiFi-SPI flash helper ===
echo.

python tools\send_flash_payload.py %*
if errorlevel 1 (
  echo.
  echo FAILED. Send console output and logs\flash_payload_*.log here.
  exit /b 1
)

echo.
echo OK. Log is in logs\flash_payload_*.log
exit /b 0
