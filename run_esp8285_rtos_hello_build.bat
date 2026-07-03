@echo off
setlocal EnableExtensions

cd /d "%~dp0"

set "SDK_ROOT=D:\w_space\esp8266_sdk"
set "SDK_DIR=%SDK_ROOT%\ESP8266_RTOS_SDK"
set "TOOLCHAIN_DIR=%SDK_ROOT%\xtensa-lx106-elf"
set "TOOLCHAIN_GCC=%TOOLCHAIN_DIR%\bin\xtensa-lx106-elf-gcc.exe"
set "TOOLCHAIN_ZIP=%SDK_ROOT%\xtensa-lx106-elf-gcc8_4_0-esp-2020r3-win32.zip"
set "TOOLCHAIN_URL=https://dl.espressif.com/dl/xtensa-lx106-elf-gcc8_4_0-esp-2020r3-win32.zip"
set "HELPER=/d/w_space/K210_ESP_SPI_WIFI/tools/esp8266_rtos_build_hello.sh"
set "BASH=C:\msys64\usr\bin\bash.exe"

if not exist "%SDK_DIR%\make\project.mk" (
    echo ERROR: ESP8266_RTOS_SDK not found at %SDK_DIR%
    echo Clone it first: git clone -b v3.4 https://github.com/espressif/ESP8266_RTOS_SDK.git
    exit /b 2
)

if not exist "%BASH%" (
    echo ERROR: MSYS2 bash not found at %BASH%
    echo Install MSYS2 to C:\msys64 or edit this BAT.
    exit /b 2
)

if not exist "%TOOLCHAIN_GCC%" (
    echo ESP8266 xtensa toolchain not found. Downloading official Espressif gcc8_4_0 toolchain...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $root='%SDK_ROOT%'; $zip='%TOOLCHAIN_ZIP%'; $url='%TOOLCHAIN_URL%'; $target='%TOOLCHAIN_DIR%'; New-Item -ItemType Directory -Force -Path $root | Out-Null; if (!(Test-Path $zip)) { Invoke-WebRequest -Uri $url -OutFile $zip }; Expand-Archive -Force -Path $zip -DestinationPath $root; $gcc=Get-ChildItem -Path $root -Recurse -Filter xtensa-lx106-elf-gcc.exe | Select-Object -First 1; if ($null -eq $gcc) { throw 'xtensa-lx106-elf-gcc.exe not found after extraction' }; $bin=Split-Path -Parent $gcc.FullName; $tool=Split-Path -Parent $bin; if ((Resolve-Path $tool).Path -ne (Resolve-Path $target -ErrorAction SilentlyContinue).Path) { if (Test-Path $target) { Remove-Item -Recurse -Force $target }; Move-Item -Force $tool $target }; Write-Host ('Toolchain ready: ' + $target)"
    if errorlevel 1 exit /b 3
)

echo Running ESP8285 RTOS SDK hello build...
"%BASH%" -lc "export MSYSTEM=MINGW32; source /etc/profile; export IDF_PATH=/d/w_space/esp8266_sdk/ESP8266_RTOS_SDK; bash '%HELPER%'"
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
    echo.
    echo FAILED: hello build returned %RC%.
    exit /b %RC%
)

echo.
echo OK: ESP8285 RTOS SDK hello build finished.
exit /b 0
