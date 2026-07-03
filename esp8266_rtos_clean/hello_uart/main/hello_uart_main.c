#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_spi_flash.h"

static const char *TAG = "esp8285_hello";

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    const uint32_t flash_size = spi_flash_get_chip_size();

    ESP_LOGI(TAG, "BOOT: ESP8285 / ESP8266 RTOS SDK hello_uart");
    ESP_LOGI(TAG, "SDK version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "Chip cores=%d revision=%d", chip_info.cores, chip_info.revision);
    ESP_LOGI(TAG, "Flash size=%lu bytes (%lu MB)",
             (unsigned long)flash_size,
             (unsigned long)(flash_size / (1024UL * 1024UL)));
    ESP_LOGI(TAG, "UART log baud must be 115200 for this bring-up test");

    uint32_t seq = 0;

    while (1) {
        ESP_LOGI(TAG, "alive seq=%lu tick=%lu",
                 (unsigned long)seq++,
                 (unsigned long)xTaskGetTickCount());
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
