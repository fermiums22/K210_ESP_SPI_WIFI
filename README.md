# ESP8285 Wi-Fi bridge

> [!IMPORTANT]
> Разработка остановлена 16 июля 2026 года. Проект заменён на
> `ESP32S3_V7s_Plus`, где ESP32-S3 самостоятельно выполняет сетевые,
> командные и аудиозадачи, а GoPro HERO12 передаёт видео на домашний сервер.
> От отдельного SPI-моста ESP8285 отказались из-за сложности транспорта с
> K210, ограничений производительности и слишком высокой стоимости поддержки
> стабильных Wi-Fi и OTA.

Одна production-прошивка ESP8285 для Wi-Fi, OTA и обмена с K210.

ESP — SPI master. Линии: GPIO15 CS, GPIO14 CLK, GPIO13 MOSI, GPIO12 MISO, GPIO3 PHASE, GPIO0 READY от K210.

## Структура

- `firmware/main` — приложение ESP.
- `protocol` — KSTREAM/KNET/KUPDATE.
- `tools` — сборка, OTA и два protocol-теста.
- `docs` — аппаратные заметки.

## Команды

```bat
build_esp.bat
py -3 tools\esp_ota.py 192.168.0.103 firmware\build\esp8285-sta-klink.ota.bin
py -3 tools\stream_speed_test.py 192.168.0.103 --seconds 10 --uplink 1500000 --downlink 500000
```

`build_esp.bat` выполняет инкрементальную сборку приложения. `build_esp.bat --full` нужен только после изменения SDK, partition table или bootloader.

Требование к bulk uplink: не меньше `1.5 MB/s` полезных данных. ESP является SPI master; команды открывают отдельные длинные bulk-окна, а GPIO0/GPIO3 подтверждают готовность и границы DMA. Командные кадры не вставляются внутрь окна данных.
