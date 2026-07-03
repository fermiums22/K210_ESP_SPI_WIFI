@echo off
echo ERROR: disabled.
echo PC -^> WiFi -^> SPI -^> SD is not implemented on ESP8266_RTOS_SDK yet.
echo Do not use Arduino/PlatformIO fallback for this project.
echo Current valid RTOS/K210 test:
echo   run_esp8285_rtos_hello_via_k210.bat COM12
exit /b 1
