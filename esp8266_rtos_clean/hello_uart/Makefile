PROJECT_NAME := esp8285-hello-uart

# Keep the first RTOS SDK bring-up build small.
# ESP8266_RTOS_SDK Make build otherwise compiles nearly all SDK components.
# This set is enough for app_main(), FreeRTOS, logs, ESP system APIs, SPI flash,
# ESP8266 driver/HAL including Wi-Fi/SPI sources, partition table and image tools.
COMPONENTS := \
    app_update \
    bootloader \
    bootloader_support \
    esp8266 \
    esp_common \
    esptool_py \
    freertos \
    heap \
    log \
    main \
    newlib \
    partition_table \
    spi_flash

include $(IDF_PATH)/make/project.mk
