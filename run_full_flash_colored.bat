@echo off
setlocal EnableExtensions

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"

set "NOFULL="
if /I "%~2"=="--no-full-flash" set "NOFULL=-NoFullFlash"
if /I "%~2"=="nofull" set "NOFULL=-NoFullFlash"

set "SCRIPT=%~dp0tools\run_full_flash_colored.ps1"

if not exist "%SCRIPT%" (
    echo ERROR: PowerShell helper not found: %SCRIPT%
    echo.
    pause
    exit /b 1
)

echo === K210 + ESP8285 colored full flash ===
echo Port: %PORT%
if "%NOFULL%"=="" (
    echo Mode: --full-flash
) else (
    echo Mode: no full flash
)
echo.

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT%" -Port "%PORT%" %NOFULL%
set "RC=%ERRORLEVEL%"

echo.
if not "%RC%"=="0" (
    echo FAILED. Exit code: %RC%
) else (
    echo DONE. Exit code: 0
)
echo.
echo Window will not close automatically.
pause
exit /b %RC%
