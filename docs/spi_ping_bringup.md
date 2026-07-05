# ESP8285 RTOS SPI ping bring-up

This note records the first minimal RTOS-SDK SPI link stage for the K210 + ESP8285 pair.

## Scope of this stage

Goal: prove that the ESP8285 firmware built with official `ESP8266_RTOS_SDK v3.4` can expose a repeatable SPI-slave hello frame to the already existing K210 SPI scanner.

This is not Wi-Fi yet and not the final TCP/SD bridge yet.

## ESP side

Current project:

```text
esp8266_rtos_clean/hello_uart/
```

The application still prints the UART bring-up markers:

```text
BOOT: ESP8285 / ESP8266 RTOS SDK hello_uart
alive seq=...
```

It now also initializes ESP8266 HSPI in slave mode and logs:

```text
kesp: spi slave ready hspi gpio12=miso gpio13=mosi gpio14=clk gpio15=cs mode=0
```

The SPI MISO frame contains the diagnostic magic `KESP`. Several byte/bit-order variants are included in the frame while the physical link is being validated.

Expected ESP HSPI pins from the ESP8266 RTOS SDK driver:

```text
ESP GPIO15 -> HSPI CS0
ESP GPIO14 -> HSPI CLK
ESP GPIO13 -> HSPI MOSI
ESP GPIO12 -> HSPI MISO
```

## K210 side used by this stage

No new K210 SPI protocol was invented for this stage. The current K210 firmware already has:

```text
KSD command: RUN_SPI
src/esp_spi_link.c
src/esp_uart_log.c
```

`RUN_SPI` does two things:

1. starts the ESP UART log bridge at 115200 baud;
2. starts the K210 SPI scanner.

The scanner waits until the ESP UART log contains:

```text
kesp: spi slave ready
```

Then it scans:

```text
SPI modes: 0, 1, 2, 3
speeds:    100 kHz, 500 kHz, 1 MHz
MOSI/MISO mapping: normal and swapped D0/D1
CS handling: hardware SS0 and GPIOHS manual CS
wire length: 32 and 34 bytes
```

A successful first result looks like:

```text
[pure-spi] best mode=<n> hz=<rate> map=<mapping> cs=<cs> len=<len> good=<nonzero> bad=<...> off=<...>
```

## One-command test

From Windows `cmd.exe` after updating both repos to the selected commits:

```bat
cd /d D:\w_space\K210_ESP_SPI_WIFI && run_esp8285_rtos_spi_ping_via_k210.bat COM12
```

The runner performs:

1. ESP RTOS build through MSYS2;
2. upload of ESP flash artifacts to K210 SD through KSD;
3. `FLASH_ESP` through K210;
4. `RUN_SPI` through KSD;
5. waits for a K210 scanner line with `good > 0`.

## If the test fails

Do not guess from memory. Use the log.

Most important checks:

1. ESP UART must show `kesp: spi slave ready`.
2. K210 must show `[pure-spi] ESP ready marker seen; scanning modes`.
3. If all scan cases show `good=0`, confirm the physical SPI wiring:

```text
K210 IO0 -> ESP GPIO15 / HSPI CS0
K210 IO1 -> ESP GPIO14 / HSPI CLK
K210 IO2 -> ESP GPIO12 / HSPI MISO
K210 IO3 -> ESP GPIO13 / HSPI MOSI
GND common
3.3 V logic only
```

4. If the ESP does not boot after flashing, debug ESP UART/BOOT/EN flashing first. Do not continue to SPI.
5. If K210 build fails, do not flash K210.

## Protected working baseline

The K210 camera path is intentionally not touched by this ESP-side stage.

Known working camera state to preserve:

```text
GC0328 VGA 640x480 RGB565
CAM_CAPTURE capture.rgb565
KSD:CAPTURE_OK capture.rgb565 614400 640 480 RGB565
```
