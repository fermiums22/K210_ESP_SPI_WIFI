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

set "SDK_DIR=D:\w_space\esp8266_sdk\ESP8266_RTOS_SDK"
set "HELPER=/d/w_space/K210_ESP_SPI_WIFI/tools/esp8266_rtos_flash_hello.sh"
set "BASH=C:\msys64\usr\bin\bash.exe"

if not exist "%SDK_DIR%\export.sh" (
    echo ERROR: ESP8266_RTOS_SDK not found at %SDK_DIR%
    exit /b 2
)

if not exist "%BASH%" (
    echo ERROR: MSYS2 bash not found at %BASH%
    exit /b 2
)

echo Flashing ESP8285 RTOS SDK hello to %ESPPORT% at %ESPBAUD%...
echo Make sure ESP GPIO0 is LOW during reset and UART0 is connected directly.
"%BASH%" -lc "export MSYSTEM=MINGW32; source /etc/profile; bash '%HELPER%' '%ESPPORT%' '%ESPBAUD%'"
exit /b %ERRORLEVEL%
