#include "kstream_master.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/spi.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/task.h"

#include "kstream_v2.h"

#define KSTREAM_READY_GPIO GPIO_NUM_0

static const char *TAG = "kstream-spi";
static kstream_master_buffers_t *s_buffers;
static uint32_t s_sequence;
static uint32_t s_faults;
static TaskHandle_t s_transport_task;
static uint32_t s_ready_level = 1u;
static uint8_t s_burst[KSTREAM_V2_BURST_BYTES] __attribute__((aligned(4)));

static void IRAM_ATTR ready_gpio_isr(void *arg)
{
    (void)arg;
    BaseType_t task_woken = pdFALSE;
    if (s_transport_task)
        vTaskNotifyGiveFromISR(s_transport_task, &task_woken);
    if (task_woken == pdTRUE)
        portYIELD_FROM_ISR();
}

static void ready_wait_next(void)
{
    while ((uint32_t)gpio_get_level(KSTREAM_READY_GPIO) == s_ready_level)
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_ready_level ^= 1u;
}

static void ready_gpio_init(void)
{
    gpio_config_t config;
    memset(&config, 0, sizeof(config));
    config.pin_bit_mask = 1ULL << KSTREAM_READY_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = 1u;
    config.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_ERROR_CHECK(gpio_install_isr_service(0u));
    ESP_ERROR_CHECK(gpio_isr_handler_add(KSTREAM_READY_GPIO,
                                         ready_gpio_isr, NULL));
}

static void spi_transfer(bool read, void *buffer, size_t length)
{
    uint8_t *bytes = (uint8_t *)buffer;
    while (length != 0u) {
        size_t count = length > 64u ? 64u : length;
        spi_trans_t trans;
        memset(&trans, 0, sizeof(trans));
        if (read) {
            trans.miso = (uint32_t *)bytes;
            trans.bits.miso = count * 8u;
        } else {
            trans.mosi = (uint32_t *)bytes;
            trans.bits.mosi = count * 8u;
        }
        ESP_ERROR_CHECK(spi_trans(HSPI_HOST, &trans));
        bytes += count;
        length -= count;
    }
}

static void master_int_complete(void)
{
    static uint32_t low __attribute__((aligned(4))) = 0x00000000u;
    static uint32_t high __attribute__((aligned(4))) = 0xffffffffu;
    ready_wait_next();
    spi_transfer(false, &low, sizeof(low));
    spi_transfer(false, &high, sizeof(high));
}

static void command_send(kstream_v2_command_t *command)
{
    command->magic = KSTREAM_V2_MAGIC_CMD;
    command->version = KSTREAM_V2_VERSION;
    command->sequence = ++s_sequence;
    kstream_v2_command_finalize(command);
    spi_transfer(false, command, sizeof(*command));
    ready_wait_next();
}

static void activation_command_send(kstream_v2_command_t *command)
{
    command->magic = KSTREAM_V2_MAGIC_CMD;
    command->version = KSTREAM_V2_VERSION;
    command->opcode = KSTREAM_V2_OP_ACTIVATE_INT;
    command->flags = KSTREAM_V2_INT_MODE_TOGGLE;
    command->arg0 = KSTREAM_V2_INT_EVENT_PHASE_ARMED;
    command->arg1 = KSTREAM_V2_INT_BOOT_LEVEL_HIGH;
    command->sequence = ++s_sequence;
    kstream_v2_command_finalize(command);

    /* K210 has this first RX DMA armed while it holds ESP GPIO0 HIGH.  This
     * descriptor, not elapsed boot time, transfers IO15 to phase signalling. */
    spi_transfer(false, command, sizeof(*command));
    ready_wait_next();
}

static bool response_read(uint32_t sequence, kstream_v2_response_t *response)
{
    memset(response, 0, sizeof(*response));
    spi_transfer(true, response, sizeof(*response));
    master_int_complete();
    ready_wait_next();
    uint32_t calculated_crc = kstream_v2_crc32(
        response, offsetof(kstream_v2_response_t, crc32));
    if (!kstream_v2_response_valid(response) || response->sequence != sequence ||
        response->result != KSTREAM_V2_RESULT_OK) {
        ++s_faults;
        ESP_LOGE(TAG,
                 "response fault want_seq=%lu got_seq=%lu magic=%08lx ver=%u result=%u crc=%08lx calc=%08lx faults=%lu",
                 (unsigned long)sequence, (unsigned long)response->sequence,
                 (unsigned long)response->magic, (unsigned)response->version,
                 (unsigned)response->result, (unsigned long)response->crc32,
                 (unsigned long)calculated_crc, (unsigned long)s_faults);
        return false;
    }
    return true;
}

static bool query(uint8_t opcode, kstream_v2_response_t *response)
{
    kstream_v2_command_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = opcode;
    command_send(&command);
    return response_read(command.sequence, response);
}

static size_t push(uint8_t stream, StreamBufferHandle_t source, uint32_t credit,
                   kstream_v2_response_t *status)
{
    size_t available = xStreamBufferBytesAvailable(source);
    size_t count = available;
    if (credit < KSTREAM_V2_FRAME_BYTES)
        return 0u;
    if (count > credit)
        count = credit;
    if (count > sizeof(s_burst))
        count = sizeof(s_burst);
    if (count == 0u)
        return 0u;
    size_t wire_count = (count + 3u) & ~(size_t)3u;
    if (wire_count < KSTREAM_V2_FRAME_BYTES)
        wire_count = KSTREAM_V2_FRAME_BYTES;
    if (wire_count > credit)
        count = credit & ~(size_t)3u;
    wire_count = (count + 3u) & ~(size_t)3u;
    if (wire_count < KSTREAM_V2_FRAME_BYTES)
        wire_count = KSTREAM_V2_FRAME_BYTES;
    memset(s_burst + count, 0, wire_count - count);
    if (xStreamBufferReceive(source, s_burst, count, 0) != count)
        abort();

    kstream_v2_command_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = KSTREAM_V2_OP_PUSH;
    command.stream = stream;
    command.length = (uint32_t)count;
    command.arg0 = (uint32_t)wire_count;
    command_send(&command);
    spi_transfer(false, s_burst, wire_count);
    ready_wait_next();
    if (!response_read(command.sequence, status))
        return 0u;
    return count;
}

static size_t pull(uint8_t stream, StreamBufferHandle_t destination,
                   uint32_t used, kstream_v2_response_t *status)
{
    size_t space = xStreamBufferSpacesAvailable(destination);
    size_t count = used;
    if (count > space)
        count = space;
    if (count > sizeof(s_burst))
        count = sizeof(s_burst);
    count &= ~(size_t)3u;
    if (count == 0u)
        return 0u;

    kstream_v2_command_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = KSTREAM_V2_OP_PULL;
    command.stream = stream;
    command.length = (uint32_t)count;
    command.arg0 = (uint32_t)count;
    command_send(&command);
    spi_transfer(true, s_burst, count);
    master_int_complete();
    ready_wait_next();
    if (!response_read(command.sequence, status))
        return 0u;
    if (xStreamBufferSend(destination, s_burst, count, 0) != count)
        abort();
    return count;
}

static void spi_init_master(void)
{
    spi_config_t config;
    memset(&config, 0, sizeof(config));
    config.interface.val = SPI_DEFAULT_INTERFACE;
    config.interface.byte_tx_order = SPI_BYTE_ORDER_MSB_FIRST;
    config.interface.byte_rx_order = SPI_BYTE_ORDER_MSB_FIRST;
    config.intr_enable.val = SPI_MASTER_DEFAULT_INTR_ENABLE;
    config.mode = SPI_MASTER_MODE;
    config.clk_div = SPI_40MHz_DIV;
    config.event_cb = NULL;
    ESP_ERROR_CHECK(spi_init(HSPI_HOST, &config));
}

static void transport_task(void *arg)
{
    (void)arg;
    s_transport_task = xTaskGetCurrentTaskHandle();
    ready_gpio_init();
    spi_init_master();
    ESP_LOGI(TAG, "MASTER clock=40MHz burst=%u ready=GPIO0 edge ack=MOSI low-high",
             (unsigned)KSTREAM_V2_BURST_BYTES);

    kstream_v2_response_t status;
    kstream_v2_command_t activation;
    memset(&activation, 0, sizeof(activation));
    activation_command_send(&activation);
    if (!response_read(activation.sequence, &status)) {
        ESP_LOGE(TAG, "K210 slave did not activate INT; transport halted");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "INT_ACTIVE mode=toggle event=phase-armed");

    if (!query(KSTREAM_V2_OP_HELLO, &status)) {
        ESP_LOGE(TAG, "K210 slave did not answer HELLO; transport halted");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "LINK_UP peer=%.*s", (int)sizeof(status.message), status.message);

    for (;;) {
        bool worked = false;
        size_t moved = pull(KSTREAM_V2_STREAM_CONSOLE_TX,
                            s_buffers->console_tx, status.console_tx_used,
                            &status);
        worked |= moved != 0u;

        /* Uplink gets three bursts for every downlink burst: video is the
         * dominant stream, while commands still have a bounded service time. */
        for (unsigned i = 0; i < 3u; ++i) {
            moved = pull(KSTREAM_V2_STREAM_UPLINK, s_buffers->uplink,
                         status.uplink_used, &status);
            worked |= moved != 0u;
            if (moved == 0u)
                break;
        }

        moved = push(KSTREAM_V2_STREAM_CONSOLE_RX, s_buffers->console_rx,
                     status.console_rx_free, &status);
        worked |= moved != 0u;
        moved = push(KSTREAM_V2_STREAM_DOWNLINK, s_buffers->downlink,
                     status.downlink_free, &status);
        worked |= moved != 0u;

        /* Every completed PUSH/PULL returns the next credits in its mandatory
         * response header.  STATUS is only the idle refresh for data produced
         * asynchronously by K210; it is never inserted between bulk bursts. */
        if (!worked) {
            if (!query(KSTREAM_V2_OP_STATUS, &status)) {
                ESP_LOGE(TAG, "STATUS failed; transport halted");
                vTaskDelete(NULL);
            }
            vTaskDelay(1);
        }
    }
}

void kstream_master_start(kstream_master_buffers_t *buffers)
{
    s_buffers = buffers;
    xTaskCreate(transport_task, "kstream_spi", 4096, NULL, 8, NULL);
}
