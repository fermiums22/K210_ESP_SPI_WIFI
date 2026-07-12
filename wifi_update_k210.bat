@echo off
setlocal EnableExtensions
cd /d "%~dp0"

if "%~1"=="" (
  echo Usage: wifi_update_k210.bat ESP_STA_IP K210_SLOT_IMAGE
  exit /b 2
)
if "%~2"=="" (
  echo Usage: wifi_update_k210.bat ESP_STA_IP K210_SLOT_IMAGE
  exit /b 2
)
if not exist "%~2" (
  echo ERROR: image not found: %~2
  exit /b 2
)

py -3 tools\kupdate_v2.py "%~2" --host "%~1" --port 21002
exit /b %ERRORLEVEL%
