# ESP8285 Wi-Fi bridge

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
py -3 tools\stream_speed_test.py 192.168.0.103 --seconds 10 --uplink 12000000 --downlink 500000
```

`build_esp.bat` выполняет инкрементальную сборку приложения. `build_esp.bat --full` нужен только после изменения SDK, partition table или bootloader.

Требование к bulk uplink: не меньше `1.5 MB/s` полезных данных. Текущий стабильный SPI control transport этого не обеспечивает; bulk должен быть вынесен в отдельные DMA-окна поверх линий I2S, а SPI оставить для команд и кредитов.
