@echo off
setlocal EnableExtensions

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
set "ESP_DIR=%~dp0"
set "K210_DIR=D:\w_space\K210_AI_V7s_Plus"

echo === Safe WiFi SPI pin GPIOHS test ===
echo Port:      %PORT%
echo ESP repo:  %ESP_DIR%
echo K210 repo: %K210_DIR%
echo.
echo Flow:
echo   1. Build ESP GPIO pin tester.
echo   2. Upload/flash ESP while K210 MAIN/KSD is alive.
echo   3. Flash minimal K210 pin tester: no LCD, no audio, no SD, no KSD.
echo   4. Monitor pin-test result.
echo.
echo Requirement: K210 must currently run MAIN/KSD. If not, flash K210 main first.
echo.

if not exist "%K210_DIR%\.git" (
  echo ERROR: K210 repo not found: %K210_DIR%
  exit /b 2
)

cd /d "%ESP_DIR%" || exit /b 2
echo === ESP: checkout spi-uart-test ===
git fetch origin || exit /b 10
git checkout spi-uart-test || exit /b 11
git pull --ff-only origin spi-uart-test || exit /b 12

echo === ESP: build GPIO tester ===
call build_esp_payload.bat || exit /b 20

echo === ESP: upload/flash through current K210 MAIN/KSD, no reset ===
echo If this fails with KSD timeout, run recovery main flash first; do not use --auto-reset dan here.
call upload_esp_payload_uart.bat %PORT% || exit /b 21

cd /d "%K210_DIR%" || exit /b 2
echo === K210: checkout spi-uart-test ===
git fetch origin || exit /b 30
git checkout spi-uart-test || exit /b 31
git pull --ff-only origin spi-uart-test || exit /b 32

echo === K210: build + flash minimal GPIOHS tester ===
call build_k210.bat || exit /b 40
call flash_k210.bat %PORT% --no-build || exit /b 41

cd /d "%ESP_DIR%" || exit /b 2
echo === Monitor safe pin test ===
echo Expected:
echo   kesp-gpio-test: RESULT ESP_SEES_K210_OK ...
echo   [pin-test] RESULT K210_SEES_ESP_MISO_OK ...
echo.
call monitor_k210.bat %PORT%
exit /b %ERRORLEVEL%
