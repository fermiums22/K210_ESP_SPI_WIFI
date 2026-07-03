#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "spi_interface.h"

#ifndef KESP_VERSION
#define KESP_VERSION "rtos-spi-test"
#endif

#define SPI_FRAME_BYTES     32u
#define SPI_FRAME_WORDS     16u  /* ESP8266 SPI slave FIFO is 16 x 32-bit = 64 bytes. */

static volatile uint32_t s_seq;
static uint32_t s_tx_words[SPI_FRAME_WORDS];

static void make_pattern(uint32_t seq, uint32_t *out)
{
    uint8_t b[SPI_FRAME_WORDS * 4];
    memset(b, 0, sizeof(b));

    b[0] = 'K';
    b[1] = 'E';
    b[2] = 'S';
    b[3] = 'P';
    b[4] = (uint8_t)(seq >> 0);
    b[5] = (uint8_t)(seq >> 8);
    b[6] = (uint8_t)(seq >> 16);
    b[7] = (uint8_t)(seq >> 24);
    b[8] = 0xa5;
    b[9] = 0x5a;
    b[10] = 0xc3;
    b[11] = 0x3c;

    for (uint32_t i = 12; i < SPI_FRAME_BYTES; i++) {
        b[i] = (uint8_t)(0x30u + ((seq + i) & 0x3fu));
    }

    for (uint32_t w = 0; w < SPI_FRAME_WORDS; w++) {
        out[w] = ((uint32_t)b[w * 4 + 0]) |
                 ((uint32_t)b[w * 4 + 1] << 8) |
                 ((uint32_t)b[w * 4 + 2] << 16) |
                 ((uint32_t)b[w * 4 + 3] << 24);
    }
}

static void load_static_pattern(void)
{
    make_pattern(s_seq++, s_tx_words);

    /* Old ESP8266 RTOS SDK 1.5 uses driver_lib/spi_interface.* rather than
     * ESP-IDF-style driver/spi.h.  For the first bring-up we only preload one
     * fixed 32-byte MISO frame.  K210 checks the wire-level KESP magic. */
    SPISlaveSendData(SpiNum_HSPI, s_tx_words, SPI_FRAME_BYTES);
}

static void spi_slave_init(void)
{
    SpiAttr attr;
    memset(&attr, 0, sizeof(attr));
    attr.bitOrder = SpiBitOrder_MSBFirst;
    attr.mode = SpiOpMode_Slave;
    attr.subMode = SpiSubMode_0;
    attr.speed = 0;

    SPIInit(SpiNum_HSPI, &attr);
    load_static_pattern();
}

void app_main(void)
{
    printf("\n");
    printf("kesp-rtos-spi-test: boot version=%s\n", KESP_VERSION);
    printf("kesp-rtos-spi-test: ESP8266 RTOS SDK 1.5 driver_lib, no Arduino, no WiFi, no TCP\n");
    printf("kesp-rtos-spi-test: HSPI slave pins GPIO15=CS GPIO14=CLK GPIO12=MISO GPIO13=MOSI\n");
    printf("kesp-rtos-spi-test: static 32-byte MISO frame starts with 4b 45 53 50\n");

    spi_slave_init();

    printf("kesp-rtos-spi-test: spi slave init done\n");
    printf("kesp: spi slave ready\n");
    printf("kesp-rtos-spi-test: ready\n");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        printf("kesp-rtos-spi-test: alive next_seq=%lu tx0=%08lx tx1=%08lx\n",
               (unsigned long)s_seq,
               (unsigned long)s_tx_words[0],
               (unsigned long)s_tx_words[1]);
    }
}
