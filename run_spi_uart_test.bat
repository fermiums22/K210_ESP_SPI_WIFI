@echo off
setlocal EnableExtensions

echo STOP: run_spi_uart_test.bat is disabled.
echo Reason: the GPIO/SPI diagnostic branch can leave the K210 audio amplifier in a noisy state if the K210 app hangs.
echo.
echo Recovery command:
echo cd /d D:\w_space\K210_AI_V7s_Plus ^&^& git checkout main ^&^& git pull ^&^& build_k210.bat ^&^& flash_k210.bat COM12 --no-build
echo.
exit /b 99
