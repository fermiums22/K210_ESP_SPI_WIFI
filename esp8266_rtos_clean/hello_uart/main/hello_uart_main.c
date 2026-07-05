#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "driver/spi.h"

#define KESP_FRAME_WORDS       16u
#define KESP_FRAME_BITS        (KESP_FRAME_WORDS * 32u)
#define KESP_MAGIC_LE          0x5053454bUL /* bytes in ESP RAM / on normal wire: "KESP" */
#define KESP_MAGIC_WORD_SWAP   0x4b455350UL
#define KESP_MAGIC_BITREV_LE   0x0acaa2d2UL /* bit-reversed bytes: K/E/S/P -> D2/A2/CA/0A */

static const char *TAG = "esp8285_hello";

static uint32_t s_spi_miso[KESP_FRAME_WORDS] __attribute__((aligned(4)));
static uint32_t s_spi_mosi[KESP_FRAME_WORDS] __attribute__((aligned(4)));
static volatile uint32_t s_spi_irq_events;

static void spi_event_cb(int event, void *arg)
{
    (void)arg;
    if (event == SPI_TRANS_DONE_EVENT) {
        s_spi_irq_events++;
    }
}

static void prepare_spi_reply(uint32_t seq)
{
    memset(s_spi_miso, 0, sizeof(s_spi_miso));

    /* Keep several sync variants in the first bytes so the K210 scanner can
     * still find KESP while we are validating ESP8266 RTOS SDK bit/byte order.
     */
    s_spi_miso[0] = KESP_MAGIC_LE;
    s_spi_miso[1] = seq;
    s_spi_miso[2] = KESP_MAGIC_WORD_SWAP;
    s_spi_miso[3] = KESP_MAGIC_BITREV_LE;
    s_spi_miso[4] = 0x31495053UL; /* "SPI1" in little-endian RAM */
    s_spi_miso[5] = s_spi_irq_events;
}

static esp_err_t spi_slave_start(void)
{
    spi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.interface.val = SPI_DEFAULT_INTERFACE;
    cfg.interface.cpol = SPI_CPOL_LOW;
    cfg.interface.cpha = SPI_CPHA_LOW;
    cfg.interface.bit_tx_order = SPI_BIT_ORDER_MSB_FIRST;
    cfg.interface.bit_rx_order = SPI_BIT_ORDER_MSB_FIRST;
    cfg.interface.byte_tx_order = SPI_BYTE_ORDER_MSB_FIRST;
    cfg.interface.byte_rx_order = SPI_BYTE_ORDER_MSB_FIRST;
    cfg.interface.mosi_en = 1;
    cfg.interface.miso_en = 1;
    cfg.interface.cs_en = 1;
    cfg.intr_enable.val = SPI_SLAVE_DEFAULT_INTR_ENABLE;
    cfg.event_cb = spi_event_cb;
    cfg.mode = SPI_SLAVE_MODE;
    cfg.clk_div = SPI_2MHz_DIV; /* ignored by ESP8266 driver in slave mode */

    return spi_init(HSPI_HOST, &cfg);
}

static void spi_slave_task(void *arg)
{
    (void)arg;

    uint32_t seq = 0;
    uint16_t cmd = 0;
    uint32_t addr = 0;
    const TickType_t delay_ticks = 10 / portTICK_PERIOD_MS;

    for (;;) {
        spi_trans_t trans;
        memset(&trans, 0, sizeof(trans));
        memset(s_spi_mosi, 0, sizeof(s_spi_mosi));
        prepare_spi_reply(seq);

        trans.cmd = &cmd;
        trans.addr = &addr;
        trans.mosi = s_spi_mosi;
        trans.miso = s_spi_miso;
        trans.bits.cmd = 8;
        trans.bits.addr = 8;
        trans.bits.mosi = KESP_FRAME_BITS;
        trans.bits.miso = KESP_FRAME_BITS;

        esp_err_t err = spi_trans(HSPI_HOST, &trans);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "kesp: spi_trans failed err=0x%x", err);
        }

        if (seq == 0) {
            ESP_LOGI(TAG, "kesp: spi slave ready hspi gpio12=miso gpio13=mosi gpio14=clk gpio15=cs mode=0");
        } else if ((seq % 100u) == 0u) {
            ESP_LOGI(TAG,
                     "kesp: spi alive seq=%lu irq=%lu cmd=0x%04x addr=0x%08lx mosi0=0x%08lx",
                     (unsigned long)seq,
                     (unsigned long)s_spi_irq_events,
                     (unsigned)cmd,
                     (unsigned long)addr,
                     (unsigned long)s_spi_mosi[0]);
        }

        seq++;
        vTaskDelay(delay_ticks ? delay_ticks : 1);
    }
}

static void start_spi_hello(void)
{
    esp_err_t err = spi_slave_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "kesp: spi slave init failed err=0x%x", err);
        return;
    }

    xTaskCreate(spi_slave_task, "kesp_spi", 2048, NULL, 5, NULL);
}

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

    start_spi_hello();

    uint32_t seq = 0;

    while (1) {
        ESP_LOGI(TAG, "alive seq=%lu tick=%lu spi_irq=%lu",
                 (unsigned long)seq++,
                 (unsigned long)xTaskGetTickCount(),
                 (unsigned long)s_spi_irq_events);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
