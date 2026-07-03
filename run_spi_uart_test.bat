@echo off
setlocal EnableExtensions

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
set "ESP_DIR=%~dp0"
set "K210_DIR=D:\w_space\K210_AI_V7s_Plus"

echo === Pure UART/GPIO test ===
echo ESP repo:  %ESP_DIR%
echo K210 repo: %K210_DIR%
echo Port:      %PORT%
echo Flow:      checkout test branches, flash K210 once, build/upload ESP with K210 auto-reset, then monitor.
echo.

if not exist "%K210_DIR%\.git" (
  echo ERROR: K210 repo not found: %K210_DIR%
  exit /b 2
)

cd /d "%K210_DIR%" || exit /b 2
echo === K210: checkout spi-uart-test ===
git fetch origin || exit /b 10
git checkout spi-uart-test || exit /b 11
git pull --ff-only origin spi-uart-test || exit /b 12

echo === K210: build + flash GPIO tester ===
call build_k210.bat || exit /b 20
call flash_k210.bat %PORT% --no-build || exit /b 21

cd /d "%ESP_DIR%" || exit /b 2
echo === ESP: checkout spi-uart-test ===
git fetch origin || exit /b 30
git checkout spi-uart-test || exit /b 31
git pull --ff-only origin spi-uart-test || exit /b 32

echo === ESP: build GPIO tester ===
call build_esp_payload.bat || exit /b 40

echo === ESP: upload/flash through K210 KSD with K210 auto-reset ===
call upload_esp_payload_uart.bat %PORT% --auto-reset dan || exit /b 41

echo === Monitor GPIO link test ===
echo Watch for: kesp-gpio-test RESULT and [gpio-test] RESULT.
echo.
call monitor_k210.bat %PORT%
exit /b %ERRORLEVEL%
