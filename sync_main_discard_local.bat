@echo off
setlocal
cd /d "%~dp0"

echo === FORCE sync local checkout to origin/main ===
echo Repo: %CD%
echo.
echo WARNING: this will discard ALL local tracked changes and remove untracked ignored generated files.
echo It is intended for this hardware-test checkout where all real changes are committed in GitHub.
echo.
choice /C YN /N /M "Continue? [Y/N] "
if errorlevel 2 (
  echo Aborted.
  exit /b 1
)

echo.
echo Fetching origin...
git fetch origin
if errorlevel 1 exit /b 1

echo.
echo Resetting tracked files to origin/main...
git reset --hard origin/main
if errorlevel 1 exit /b 1

echo.
echo Cleaning ignored generated files only...
git clean -fdX
if errorlevel 1 exit /b 1

echo.
echo OK. Local checkout now matches origin/main.
echo Next:
echo   build_esp_payload.bat
echo   upload_esp_payload_uart.bat COM12
exit /b 0
