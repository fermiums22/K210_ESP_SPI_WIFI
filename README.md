# ESP8285 STA ↔ KLINK bridge

Один production-проект для связки ESP8285 и K210. Arduino, PlatformIO, AP-режима, SD-посредника и экспериментальных прошивок в репозитории нет.

## Аппаратная конфигурация

- ESP8285 XTAL: **40 MHz** — подтверждено измерением GPIO0 таймером K210.
- Flash: **1 MiB, DOUT, 40 MHz**.
- UART0: только лог, **115200 baud**.
- Wi-Fi: только STA, сеть `Fermiums_2.4`.
- HSPI slave: GPIO12 MISO, GPIO13 MOSI, GPIO14 CLK, GPIO15 CS.
- GPIO0: `READY` от ESP к K210; K210 видит его на IO15.
- KLINK cell: 64 байта, SPI mode 0, LSB first.
- UART2 на K210 остаётся открыт после получения `STA_READY`: это постоянный
  канал логов ESP, которым K210 владеет параллельно со SPI.

## Сборка

Используется официальный `ESP8266_RTOS_SDK v3.4`:

```text
D:\w_space\esp8266_sdk\ESP8266_RTOS_SDK
```

Запуск из Windows:

```bat
build_esp.bat
```

Результаты:

```text
firmware\build\bootloader\bootloader.bin
firmware\build\partitions_1mb_singleapp.bin
firmware\build\esp8285-sta-klink.bin
```

## Структура

```text
firmware/       ESP8285 STA + HSPI/KLINK firmware
protocol/       KLINK v1 и KUPDATE v2, общие с K210
tools/          SDK build/setup и Wi-Fi K210 updater
docs/           аппаратные схемы Maix Dock
```

При старте исправная прошивка обязана вывести на UART 115200:

```text
BOOT kesp-sta-klink-1 ... xtal=40MHz uart=115200 ... ap=disabled
HSPI_ARMED kesp-sta-klink-1 ...
STA_CONNECT ssid=Fermiums_2.4
STA_READY ssid=Fermiums_2.4 ip=... port=21002
```

Аппаратный прогон KLINK проверяет 4096 echo-пакетов на каждой частоте
2/4/8/10/16/20 МГц. Проверенная верхняя частота — 19,5 МГц фактически,
без битых ячеек и link fault.
