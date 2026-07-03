@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "HOST=%~1"
set "FILE=%~2"
set "REMOTE=%~3"

if "%HOST%"=="" set "HOST=192.168.4.1"
if "%FILE%"=="" (
  echo Usage:
  echo   wifi_put_file.bat 192.168.4.1 path\to\file.bin remote.bin
  echo.
  echo For fallback AP: connect PC WiFi to KESP-xxxxxx, password 12345678, then use 192.168.4.1
  exit /b 1
)
if "%REMOTE%"=="" set "REMOTE=%~nx2"

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

%PY% tools\tcp_put_file.py "%HOST%" "%FILE%" "%REMOTE%"
exit /b %ERRORLEVEL%
