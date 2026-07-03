#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi.h"
#include "driver/uart.h"

#ifndef KESP_VERSION
#define KESP_VERSION "rtos-spi-test"
#endif

#define UART0_BAUD          115200
#define SPI_FRAME_BYTES     32u
#define SPI_FRAME_WORDS     16u  /* ESP8266 SPI slave FIFO is 16 x 32-bit = 64 bytes. */
#define HSPI_IF_MODE0_RAW   0x1c0u

static volatile uint32_t s_rx_count;
static volatile uint32_t s_rd_done_count;
static volatile uint32_t s_wr_done_count;
static volatile uint32_t s_seq;
static volatile uint32_t s_last_done;
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

static void IRAM_ATTR spi_load_next_pattern(void)
{
    spi_trans_t trans;
    uint32_t seq = s_seq++;

    make_pattern(seq, s_tx_words);

    memset(&trans, 0, sizeof(trans));
    trans.cmd = NULL;
    trans.addr = NULL;
    trans.bits.val = 0;
    trans.miso = s_tx_words;
    trans.bits.miso = SPI_FRAME_BYTES * 8;

    spi_trans(HSPI_HOST, &trans);
}

static void IRAM_ATTR spi_event_cb(int event, void *arg)
{
    if (event != SPI_TRANS_DONE_EVENT || arg == NULL) {
        return;
    }

    uint32_t done = *(uint32_t *)arg;
    s_last_done = done;

    if (done & SPI_SLV_WR_BUF_DONE) {
        s_wr_done_count++;
        s_rx_count++;
    }

    if (done & SPI_SLV_RD_BUF_DONE) {
        s_rd_done_count++;
        spi_load_next_pattern();
    }
}

static void spi_slave_init(void)
{
    spi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    /* Old PlatformIO ESP8266 RTOS SDK exposes the HSPI slave driver through
     * driver/spi.h but does not expose esp8266/spi_struct.h.  Keep this test on
     * the public driver API only: no direct SPI1 register access here. */
    cfg.interface.val = HSPI_IF_MODE0_RAW;
    cfg.intr_enable.val = SPI_SLAVE_DEFAULT_INTR_ENABLE;
    cfg.mode = SPI_SLAVE_MODE;
    cfg.event_cb = spi_event_cb;

    spi_init(HSPI_HOST, &cfg);
    spi_load_next_pattern();
}

void app_main(void)
{
    uart_set_baudrate(UART_NUM_0, UART0_BAUD);

    printf("\n");
    printf("kesp-rtos-spi-test: boot version=%s baud=%u\n", KESP_VERSION, UART0_BAUD);
    printf("kesp-rtos-spi-test: ESP8266 RTOS SDK, no Arduino, no WiFi, no TCP\n");
    printf("kesp-rtos-spi-test: HSPI slave pins GPIO15=CS GPIO14=CLK GPIO12=MISO GPIO13=MOSI\n");
    printf("kesp-rtos-spi-test: raw 32-byte MISO frame starts with 4b 45 53 50\n");

    spi_slave_init();

    printf("kesp-rtos-spi-test: spi slave init done\n");
    printf("kesp: spi slave ready\n");
    printf("kesp-rtos-spi-test: ready\n");

    uint32_t last_rx = 0;
    uint32_t last_rd = 0;
    uint32_t last_wr = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t rx = s_rx_count;
        uint32_t rd = s_rd_done_count;
        uint32_t wr = s_wr_done_count;
        printf("kesp-rtos-spi-test: alive rx=%lu/+%lu rd=%lu/+%lu wr=%lu/+%lu next_seq=%lu done=0x%08lx\n",
               (unsigned long)rx, (unsigned long)(rx - last_rx),
               (unsigned long)rd, (unsigned long)(rd - last_rd),
               (unsigned long)wr, (unsigned long)(wr - last_wr),
               (unsigned long)s_seq,
               (unsigned long)s_last_done);
        last_rx = rx;
        last_rd = rd;
        last_wr = wr;
    }
}
