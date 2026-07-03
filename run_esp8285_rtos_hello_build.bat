@echo off
setlocal EnableExtensions

cd /d "%~dp0"

set "SDK_DIR=D:\w_space\esp8266_sdk\ESP8266_RTOS_SDK"
set "HELPER=/d/w_space/K210_ESP_SPI_WIFI/tools/esp8266_rtos_build_hello.sh"
set "BASH=C:\msys64\usr\bin\bash.exe"

if not exist "%SDK_DIR%\export.sh" (
    echo ERROR: ESP8266_RTOS_SDK not found at %SDK_DIR%
    echo Clone it first: git clone -b v3.4 --recursive https://github.com/espressif/ESP8266_RTOS_SDK.git
    exit /b 2
)

if not exist "%BASH%" (
    echo ERROR: MSYS2 bash not found at %BASH%
    echo Install MSYS2 to C:\msys64 or edit this BAT.
    exit /b 2
)

echo Running ESP8285 RTOS SDK hello build...
"%BASH%" -lc "export MSYSTEM=MINGW32; source /etc/profile; bash '%HELPER%'"
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
    echo.
    echo FAILED: hello build returned %RC%.
    exit /b %RC%
)

echo.
echo OK: ESP8285 RTOS SDK hello build finished.
exit /b 0
