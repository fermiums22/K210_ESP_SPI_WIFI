@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "ESP_CLEAN_BUILD=0"
set "ESP_RECONFIGURE=0"
if /i "%~1"=="--full" (
  set "ESP_CLEAN_BUILD=1"
  set "ESP_RECONFIGURE=1"
)

set "SDK_ROOT=%ESP_SDK_ROOT%"
if "%SDK_ROOT%"=="" if exist "D:\w_space\esp8266_sdk\ESP8266_RTOS_SDK\make\project.mk" set "SDK_ROOT=D:\w_space\esp8266_sdk"
if "%SDK_ROOT%"=="" if exist "C:\ESP8266\sdk\ESP8266_RTOS_SDK\make\project.mk" set "SDK_ROOT=C:\ESP8266\sdk"
if "%SDK_ROOT%"=="" (
  echo ERROR: ESP_SDK_ROOT is not configured.
  exit /b 2
)

set "SDK_DIR=%SDK_ROOT%\ESP8266_RTOS_SDK"
set "BASH=%MSYS2_BASH%"
if "%BASH%"=="" set "BASH=C:\msys64\usr\bin\bash.exe"
if not exist "%SDK_DIR%\make\project.mk" (
  echo ERROR: ESP8266_RTOS_SDK missing: %SDK_DIR%
  exit /b 2
)
if not exist "%BASH%" (
  echo ERROR: MSYS2 bash missing: %BASH%
  exit /b 2
)

set "PROJECT=%CD%\firmware"
set "HELPER=%CD%\tools\build_esp.sh"

"%BASH%" -lc "export MSYSTEM=MINGW32; source /etc/profile; export IDF_PATH=\"$(cygpath -u '%SDK_DIR%')\"; export ESP_SDK_ROOT=\"$(cygpath -u '%SDK_ROOT%')\"; export ESP_PROJECT=\"$(cygpath -u '%PROJECT%')\"; export ESP_CLEAN_BUILD='%ESP_CLEAN_BUILD%'; export ESP_RECONFIGURE='%ESP_RECONFIGURE%'; bash \"$(cygpath -u '%HELPER%')\""
exit /b %ERRORLEVEL%
