@echo off
setlocal EnableExtensions
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
set "ESP_DIR=%~dp0"
set "K210_DIR=D:\w_space\K210_AI_V7s_Plus"
set "K210_FLASH_BAUD=921600"
set "KSD_CONNECT_TIMEOUT=65"

echo === RTOS pure SPI test, one K210 diagnostic loader ===
echo Port: %PORT%
echo K210 flash baud: %K210_FLASH_BAUD%
echo KSD connect timeout: %KSD_CONNECT_TIMEOUT%s

if not exist "%K210_DIR%\.git" exit /b 2

cd /d "%ESP_DIR%" || exit /b 2
git fetch origin || exit /b 10
git checkout spi-uart-test || exit /b 11
git pull --ff-only origin spi-uart-test || exit /b 12
call build_esp_payload.bat || exit /b 20

cd /d "%K210_DIR%" || exit /b 2
git fetch origin || exit /b 30
git checkout spi-uart-test || exit /b 31
git pull --ff-only origin spi-uart-test || exit /b 32
py -3 "%ESP_DIR%tools\patch_k210_fast_esp_baud.py" "%K210_DIR%" --baud 921600 || exit /b 33
call build_k210.bat || exit /b 40
call flash_k210.bat %PORT% --no-build --baud %K210_FLASH_BAUD% || exit /b 41

cd /d "%ESP_DIR%" || exit /b 2
call upload_esp_payload_uart.bat %PORT% --connect-timeout %KSD_CONNECT_TIMEOUT% || exit /b 50
exit /b 0
