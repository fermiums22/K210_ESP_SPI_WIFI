# K210_ESP_SPI_WIFI

ESP8285 firmware for the Wi-Fi/SPI bridge used by `K210_AI_V7s_Plus`.

Текущая практическая цель: собрать ESP8285 firmware на Windows, положить payload на SD-карту K210 через debug UART (`KSD1` protocol), перезагрузить K210 и дать K210 прошить ESP8285 из SD.

## Текущий режим разработки с ИИ / оператором

Работаем короткими итерациями:

1. ИИ правит код/скрипты прямо в GitHub и коммитит в `main`.
2. Виктор у себя делает `git pull`, запускает одну-две команды и сразу смотрит реальный вывод железа.
3. Если видно, что пошло не туда, Виктор сразу дропает попытку и присылает console output / `logs\flash_payload_*.log`.
4. ИИ не пытается "сам всё выполнить в фоне" и не держит длинные терминальные сессии. Это быстрее, потому что оператор видит железо, экран, COM-порты, reset/boot поведение и может остановить ошибочный путь раньше.
5. Скрипты должны быть разделены по смыслу: отдельно сборка, отдельно upload/terminal/monitor. Не надо ради просмотра UART окна пересобирать весь PlatformIO project.

Правило для новых правок: build не должен менять tracked-файлы. Generated outputs должны лежать только в ignored директориях (`.pio/`, `out/`, `logs/`).

## Preferred flashing chain: PC -> K210 SD UART -> SD -> K210 flashes ESP

```text
Windows PC
  |
  | debug UART COMx, KSD1 protocol
  v
K210_AI_V7s_Plus firmware
  |
  | writes payload to SD: esp8285_at.bin + flash.json
  v
K210 RESET
  |
  | flash.json enables one-shot ESP flashing
  v
K210 disarms flash.json first, then flashes ESP8285 through UART2 + EN/BOOT pins
```

`flash.json` is disarmed by K210 before flashing, so the ESP is not flashed again on every boot.

## One-time Windows setup

From the repository folder:

```bat
cd /d D:\w_space\K210_ESP_SPI_WIFI
py -3 -m pip install -r requirements.txt
```

If `py -3` is missing, install current Python for Windows and enable `Add python.exe to PATH` during installation.

## Current recommended workflow: split build and upload

### 1. Build ESP payload only

```bat
cd /d D:\w_space\K210_ESP_SPI_WIFI
git pull
build_esp_payload.bat
```

This runs PlatformIO and prepares files in:

```text
out\flash_payload\
  esp8285_at.bin
  flash.json
```

### 2. Upload existing payload through K210 UART, without rebuild

```bat
upload_esp_payload_uart.bat COM12
```

`COM12` заменить на реальный K210/CH340 COM-порт.

Important timing:

1. Start `upload_esp_payload_uart.bat COM12`.
2. If K210 is already showing `no host` / normal screen, press **RESET** on K210.
3. The script must catch the K210 boot window and connect to `KSD:READY` / `KSD:HELLO`.

Expected upload log:

```text
K210: KSD:READY
K210: KSD:HELLO
sd-uart: connected
sd-uart GET flash.json
sd-uart PUT esp8285_at.bin
sd-uart PUT flash.json
sd-uart RESET
K210: KSD:RESETTING
```

After reset, K210 should read `flash.json`, disarm it, and flash ESP8285. Good final signs:

```text
[esp-flash] config flash_once ESP job found
[esp-flash] connected target=ESP8266/ESP8285
[esp-flash] progress ...
ESP flash result: OK
```

## All-in-one command

This command still exists:

```bat
flash_esp_via_k210.bat --sd-uart COM12
```

But it means **build + upload**. For debugging K210 boot-window / UART timing, prefer the split workflow above. Otherwise PlatformIO build can take longer than K210's 5-second SD UART window, and the script will miss the service.

## Legacy TCP/Wi-Fi bridge mode

Older flow is still available after ESP bridge firmware is already alive and prints an IP:

```text
kesp: ip=192.168.1.123 status=3 spi=1 rx=0 B/s
```

Then run:

```bat
flash_esp_via_k210.bat --host 192.168.1.123 --monitor COM12
```

Replace:

- `192.168.1.123` with the IP printed by the ESP bridge or shown by your router.
- `COM12` with the K210 USB/CH340 COM port.

This mode is not the preferred bring-up path right now, because the current target is to flash/recover ESP8285 through K210 SD UART even when ESP Wi-Fi firmware is broken.

## If you only want to prepare files, without upload

```bat
flash_esp_via_k210.bat --dry-run
```

or the clearer wrapper:

```bat
build_esp_payload.bat
```

Generated files will be in:

```text
out/flash_payload/
```

## If you already have a ready `.bin`

Upload one explicit firmware image without PlatformIO rebuild:

```bat
upload_esp_payload_uart.bat COM12 --firmware firmware\esp8285_at.bin --remote-name esp8285_at.bin --offset 0x0
```

This sends one image and patches `flash.json` with one part at offset `0x00000000`.

## What log to send back

Send the newest file from:

```text
logs\flash_payload_*.log
```

Also useful: copy the K210 serial console if it shows `[esp-flash]` errors.

Good signs:

```text
KSD:READY
KSD:HELLO
sd-uart PUT esp8285_at.bin
sd-uart PUT flash.json
KSD:RESETTING
[esp-flash] config flash_once ESP job found
[esp-flash] connected target=ESP8266/ESP8285
[esp-flash] progress ...
ESP flash result: OK
```

Bad signs to send here:

```text
K210 SD UART service did not answer
[esp-flash] image missing
[esp-flash] connect failed
[esp-flash] write failed
ESP flash result: FAIL
```

## Repo files

```text
platformio.ini                  PlatformIO ESP8285 project
src/main.cpp                    ESP8285 Wi-Fi/TCP to SPI bridge
tools/merge_image.py            PlatformIO post-build helper
tools/send_flash_payload.py     Python sender + payload/log collector
flash_esp_via_k210.bat          all-in-one build + upload wrapper
build_esp_payload.bat           build/prepare payload only
upload_esp_payload_uart.bat     upload existing payload through K210 UART, no rebuild
requirements.txt                Python tools for helper script
.pio/                           generated PlatformIO build output, ignored
out/flash_payload/              generated payload files, ignored
logs/                           generated run logs, ignored
```
