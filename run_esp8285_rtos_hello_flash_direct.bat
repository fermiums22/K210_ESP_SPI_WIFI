@echo off
setlocal EnableExtensions

cd /d "%~dp0"

if "%~1"=="" (
    echo Usage: run_esp8285_rtos_hello_flash_direct.bat COMxx [baud]
    echo Example: run_esp8285_rtos_hello_flash_direct.bat COM13 460800
    echo.
    echo This is for DIRECT ESP UART0 flashing only, not K210 COM12.
    exit /b 2
)

set "ESPPORT=%~1"
set "ESPBAUD=%~2"
if "%ESPBAUD%"=="" set "ESPBAUD=460800"

set "SDK_ROOT=D:\w_space\esp8266_sdk"
set "SDK_DIR=%SDK_ROOT%\ESP8266_RTOS_SDK"
set "TOOLCHAIN_DIR=%SDK_ROOT%\xtensa-lx106-elf"
set "TOOLCHAIN_GCC=%TOOLCHAIN_DIR%\bin\xtensa-lx106-elf-gcc.exe"
set "TOOLCHAIN_ZIP=%SDK_ROOT%\xtensa-lx106-elf-gcc8_4_0-esp-2020r3-win32.zip"
set "TOOLCHAIN_URL=https://dl.espressif.com/dl/xtensa-lx106-elf-gcc8_4_0-esp-2020r3-win32.zip"
set "HELPER=/d/w_space/K210_ESP_SPI_WIFI/tools/esp8266_rtos_flash_hello.sh"
set "BASH=C:\msys64\usr\bin\bash.exe"

if not exist "%SDK_DIR%\make\project.mk" (
    echo ERROR: ESP8266_RTOS_SDK not found at %SDK_DIR%
    exit /b 2
)

if not exist "%BASH%" (
    echo ERROR: MSYS2 bash not found at %BASH%
    exit /b 2
)

if not exist "%TOOLCHAIN_GCC%" (
    echo ESP8266 xtensa toolchain not found. Downloading official Espressif gcc8_4_0 toolchain...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $root='%SDK_ROOT%'; $zip='%TOOLCHAIN_ZIP%'; $url='%TOOLCHAIN_URL%'; $target='%TOOLCHAIN_DIR%'; New-Item -ItemType Directory -Force -Path $root | Out-Null; if (!(Test-Path $zip)) { Invoke-WebRequest -Uri $url -OutFile $zip }; Expand-Archive -Force -Path $zip -DestinationPath $root; $gcc=Get-ChildItem -Path $root -Recurse -Filter xtensa-lx106-elf-gcc.exe | Select-Object -First 1; if ($null -eq $gcc) { throw 'xtensa-lx106-elf-gcc.exe not found after extraction' }; $bin=Split-Path -Parent $gcc.FullName; $tool=Split-Path -Parent $bin; if ((Resolve-Path $tool).Path -ne (Resolve-Path $target -ErrorAction SilentlyContinue).Path) { if (Test-Path $target) { Remove-Item -Recurse -Force $target }; Move-Item -Force $tool $target }; Write-Host ('Toolchain ready: ' + $target)"
    if errorlevel 1 exit /b 3
)

echo Flashing ESP8285 RTOS SDK hello to %ESPPORT% at %ESPBAUD%...
echo Make sure ESP GPIO0 is LOW during reset and UART0 is connected directly.
"%BASH%" -lc "export MSYSTEM=MINGW32; source /etc/profile; export PATH=/d/w_space/esp8266_sdk/xtensa-lx106-elf/bin:^$PATH; export IDF_PATH=/d/w_space/esp8266_sdk/ESP8266_RTOS_SDK; bash '%HELPER%' '%ESPPORT%' '%ESPBAUD%'"
exit /b %ERRORLEVEL%
