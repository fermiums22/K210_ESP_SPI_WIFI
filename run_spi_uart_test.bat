@echo off
setlocal EnableExtensions

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
set "ESP_DIR=%~dp0"
set "K210_DIR=D:\w_space\K210_AI_V7s_Plus"

echo === Pure UART/SPI test ===
echo ESP repo:  %ESP_DIR%
echo K210 repo: %K210_DIR%
echo Port:      %PORT%
echo Flow:      checkout test branches, flash K210 once, flash ESP test firmware once, then monitor.
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

echo === K210: build + flash pure SPI tester ===
call build_k210.bat || exit /b 20
call flash_k210.bat %PORT% --no-build || exit /b 21

cd /d "%ESP_DIR%" || exit /b 2
echo === ESP: checkout spi-uart-test ===
git fetch origin || exit /b 30
git checkout spi-uart-test || exit /b 31
git pull --ff-only origin spi-uart-test || exit /b 32

echo === ESP: build + flash pure SPISlave tester through K210/KSD ===
set "KESP_NO_PAUSE=1"
call run_full_flash_colored.bat %PORT% || exit /b 40

echo === Monitor pure SPI test ===
echo Watch for: kesp-spi-test: boot, [spi-test] GOOD mode=..., [spi-test] stat ...
echo.
call monitor_k210.bat %PORT%
exit /b %ERRORLEVEL%
