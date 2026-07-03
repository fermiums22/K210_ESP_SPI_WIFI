@echo off
setlocal EnableExtensions
cd /d "%~dp0"

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

%PY% tools\wifi_sd_smoke_test.py %*
exit /b %ERRORLEVEL%
