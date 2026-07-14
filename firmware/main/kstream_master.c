#include "kstream_master.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "driver/spi.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp8266/eagle_soc.h"
#include "esp8266/pin_mux_register.h"
#include "freertos/task.h"

#include "kstream_v2.h"

#define KSTREAM_READY_GPIO GPIO_NUM_0
#define KSTREAM_MASTER_PHASE_GPIO GPIO_NUM_1
#define KSTREAM_CS_GPIO GPIO_NUM_15

static const char *TAG = "kstream-spi";
static kstream_master_buffers_t *s_buffers;
static uint32_t s_sequence;
static uint32_t s_faults;
static TaskHandle_t s_transport_task;
static volatile bool s_quiesce_requested;
static volatile bool s_quiesced;
static volatile uint32_t s_diag_stage;
static volatile uint32_t s_diag_got_crc;
static volatile uint32_t s_diag_calculated_crc;
static volatile uint32_t s_diag_sequence;
static volatile uint8_t s_diag_opcode;
static volatile uint8_t s_diag_stream;
static uint64_t s_diag_push_bytes;
static uint64_t s_diag_pull_bytes;
static uint32_t s_diag_response_words[KSTREAM_V2_FRAME_BYTES / 4u];
static uint8_t s_burst[KSTREAM_V2_BURST_BYTES] __attribute__((aligned(4)));

static void master_phase_init(void)
{
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0TXD_U, FUNC_GPIO1);
    ESP_ERROR_CHECK(gpio_set_level(KSTREAM_MASTER_PHASE_GPIO, 1u));
    ESP_ERROR_CHECK(gpio_set_direction(KSTREAM_MASTER_PHASE_GPIO,
                                       GPIO_MODE_OUTPUT));
}

static void master_phase_set(uint32_t level)
{
    ESP_ERROR_CHECK(gpio_set_level(KSTREAM_MASTER_PHASE_GPIO, level));
    while ((uint32_t)gpio_get_level(KSTREAM_MASTER_PHASE_GPIO) != level)
        taskYIELD();
}

static void master_cs_init(void)
{
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_GPIO15);
    ESP_ERROR_CHECK(gpio_set_level(KSTREAM_CS_GPIO, 1u));
    ESP_ERROR_CHECK(gpio_set_direction(KSTREAM_CS_GPIO, GPIO_MODE_OUTPUT));
}

static void IRAM_ATTR ready_gpio_isr(void *arg)
{
    (void)arg;
    BaseType_t task_woken = pdFALSE;
    if (s_transport_task)
        vTaskNotifyGiveFromISR(s_transport_task, &task_woken);
    if (task_woken == pdTRUE)
        portYIELD_FROM_ISR();
}

static void ready_wait_level(uint32_t expected)
{
    while ((uint32_t)gpio_get_level(KSTREAM_READY_GPIO) != expected)
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
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
    uint8_t *start = bytes;
    size_t total = length;
    ESP_ERROR_CHECK(gpio_set_level(KSTREAM_CS_GPIO, 0u));
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

    uint32_t guard[2] = {UINT32_MAX, UINT32_MAX};
    spi_trans_t trans;
    memset(&trans, 0, sizeof(trans));
    if (read) {
        trans.miso = guard;
        trans.bits.miso = 32u;
    } else {
        trans.mosi = guard;
        trans.bits.mosi = 64u;
    }
    ESP_ERROR_CHECK(spi_trans(HSPI_HOST, &trans));
    ESP_ERROR_CHECK(gpio_set_level(KSTREAM_CS_GPIO, 1u));
    if (read) {
        memmove(start, start + sizeof(uint32_t), total - sizeof(uint32_t));
        memcpy(start + total - sizeof(uint32_t), guard, sizeof(uint32_t));
    }
}

static void master_write_begin(void)
{
    master_phase_set(0u);
    ready_wait_level(1u);
}

static void master_write_complete(void)
{
    master_phase_set(1u);
    ready_wait_level(0u);
}

static void master_read_begin(void)
{
    s_diag_stage = 3u;
    if (s_sequence == 1u) ESP_LOGI(TAG, "HS DMA_WAIT");
    master_phase_set(0u);
    ready_wait_level(1u);
    s_diag_stage = 5u;
    if (s_sequence == 1u) ESP_LOGI(TAG, "HS DMA_READY");
}

static void master_read_complete(void)
{
    s_diag_stage = 7u;
    if (s_sequence == 1u) ESP_LOGI(TAG, "HS COMPLETE_REQUEST");
    master_phase_set(1u);
    ready_wait_level(0u);
    s_diag_stage = 8u;
    if (s_sequence == 1u) ESP_LOGI(TAG, "HS COMPLETE_GRANTED");
}

static void command_send(kstream_v2_command_t *command)
{
    command->magic = KSTREAM_V2_MAGIC_CMD;
    command->version = KSTREAM_V2_VERSION;
    command->sequence = ++s_sequence;
    kstream_v2_command_finalize(command);
    s_diag_sequence = command->sequence;
    s_diag_opcode = command->opcode;
    s_diag_stream = command->stream;
    master_write_begin();
    spi_transfer(false, command, sizeof(*command));
    master_write_complete();
}

static void activation_command_send(kstream_v2_command_t *command)
{
    s_diag_stage = 100u;
    command->magic = KSTREAM_V2_MAGIC_CMD;
    command->version = KSTREAM_V2_VERSION;
    command->opcode = KSTREAM_V2_OP_ACTIVATE_INT;
    command->flags = KSTREAM_V2_INT_MODE_LEVEL;
    command->arg0 = KSTREAM_V2_INT_EVENT_DMA_READY;
    command->arg1 = KSTREAM_V2_INT_BOOT_LEVEL_HIGH;
    command->sequence = ++s_sequence;
    kstream_v2_command_finalize(command);
    s_diag_sequence = command->sequence;
    s_diag_opcode = command->opcode;
    s_diag_stream = command->stream;

    /* K210 has this first RX DMA armed while it holds ESP GPIO0 HIGH.  This
     * descriptor, not elapsed boot time, transfers IO15 to phase signalling. */
    spi_transfer(false, command, sizeof(*command));
    s_diag_stage = 101u;
    ready_wait_level(0u);
    s_diag_stage = 103u;
    ESP_LOGI(TAG, "HS RESPONSE_ARMED");
}

static bool response_validate(uint32_t sequence,
                              kstream_v2_response_t *response)
{
    uint32_t calculated_crc = kstream_v2_crc32(
        response, offsetof(kstream_v2_response_t, crc32));
    if (!kstream_v2_response_valid(response) || response->sequence != sequence ||
        response->result != KSTREAM_V2_RESULT_OK) {
        s_diag_stage = 0x80000000u;
        s_diag_got_crc = response->crc32;
        s_diag_calculated_crc = calculated_crc;
        memcpy(s_diag_response_words, response, sizeof(s_diag_response_words));
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

static bool response_read(uint32_t sequence, kstream_v2_response_t *response)
{
    memset(response, 0, sizeof(*response));
    master_read_begin();
    s_diag_stage = 6u;
    spi_transfer(true, response, sizeof(*response));
    master_read_complete();
    s_diag_stage = 9u;
    return response_validate(sequence, response);
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
    master_write_begin();
    spi_transfer(false, s_burst, wire_count);
    master_write_complete();
    if (!response_read(command.sequence, status))
        return 0u;
    s_diag_push_bytes += count;
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
    if (stream != KSTREAM_V2_STREAM_CONSOLE_TX)
        count &= ~(size_t)3u;
    if (count == 0u)
        return 0u;
    size_t wire_count = (count + 3u) & ~(size_t)3u;
    if (wire_count < KSTREAM_V2_FRAME_BYTES)
        wire_count = KSTREAM_V2_FRAME_BYTES;

    kstream_v2_command_t command;
    memset(&command, 0, sizeof(command));
    command.opcode = KSTREAM_V2_OP_PULL;
    command.stream = stream;
    command.length = (uint32_t)count;
    command.arg0 = (uint32_t)wire_count;
    command_send(&command);
    master_read_begin();
    spi_transfer(true, s_burst, wire_count);
    master_read_complete();
    if (stream == KSTREAM_V2_STREAM_UPDATE_TX) {
        if (xStreamBufferSend(destination, s_burst, count, 0) != count)
            abort();
        s_diag_pull_bytes += count;
        if (!query(KSTREAM_V2_OP_STATUS, status))
            return 0u;
        return count;
    }
    if (!response_read(command.sequence, status)) {
        return 0u;
    }
    if (xStreamBufferSend(destination, s_burst, count, 0) != count)
        abort();
    s_diag_pull_bytes += count;
    return count;
}

static void spi_init_master(void)
{
    spi_config_t config;
    memset(&config, 0, sizeof(config));
    config.interface.val = SPI_DEFAULT_INTERFACE;
    config.interface.byte_tx_order = SPI_BYTE_ORDER_MSB_FIRST;
    config.interface.byte_rx_order = SPI_BYTE_ORDER_MSB_FIRST;
    config.interface.cs_en = 0u;
    config.intr_enable.val = SPI_MASTER_DEFAULT_INTR_ENABLE;
    config.mode = SPI_MASTER_MODE;
    config.clk_div = SPI_10MHz_DIV;
    config.event_cb = NULL;
    ESP_ERROR_CHECK(spi_init(HSPI_HOST, &config));
}

static void transport_task(void *arg)
{
    (void)arg;
    s_transport_task = xTaskGetCurrentTaskHandle();
    ready_gpio_init();
    master_phase_init();
    spi_init_master();
    master_cs_init();
    ESP_LOGI(TAG, "MASTER clock=10MHz burst=%u ready=GPIO0 phase=GPIO1",
             (unsigned)KSTREAM_V2_BURST_BYTES);

    kstream_v2_response_t status;
    kstream_v2_command_t activation;
    memset(&activation, 0, sizeof(activation));
    activation_command_send(&activation);
    if (!response_read(activation.sequence, &status)) {
        ESP_LOGE(TAG, "K210 slave did not activate INT; transport halted");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "INT_ACTIVE mode=level event=dma-ready");

    if (!query(KSTREAM_V2_OP_HELLO, &status)) {
        ESP_LOGE(TAG, "K210 slave did not answer HELLO; transport halted");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "LINK_UP peer=%.*s", (int)sizeof(status.message), status.message);
    s_diag_stage = 10u;

    for (;;) {
        if (s_quiesce_requested) {
            master_write_begin();
            s_quiesced = true;
            __sync_synchronize();
            for (;;)
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        bool worked = false;
        size_t moved = pull(KSTREAM_V2_STREAM_UPDATE_TX,
                            s_buffers->update_tx, status.update_tx_used,
                            &status);
        worked |= moved != 0u;
        moved = push(KSTREAM_V2_STREAM_UPDATE_RX, s_buffers->update_rx,
                     status.update_rx_free, &status);
        worked |= moved != 0u;
        moved = pull(KSTREAM_V2_STREAM_CONSOLE_TX,
                            s_buffers->console_tx, status.console_tx_used,
                            &status);
        worked |= moved != 0u;

        /* Service both bulk directions in bounded batches so console traffic
         * is never held behind an unbounded stream drain. */
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
        for (unsigned i = 0; i < 3u; ++i) {
            moved = push(KSTREAM_V2_STREAM_DOWNLINK, s_buffers->downlink,
                         status.downlink_free, &status);
            worked |= moved != 0u;
            if (moved == 0u)
                break;
        }

        /* Every completed PUSH/PULL returns the next credits in its mandatory
         * response header.  STATUS is only the idle refresh for data produced
         * asynchronously by K210; it is never inserted between bulk bursts. */
        if (!worked) {
            if (!query(KSTREAM_V2_OP_STATUS, &status)) {
                ESP_LOGE(TAG, "STATUS failed; transport halted");
                vTaskDelete(NULL);
            }
            vTaskDelay(1);
        } else {
            taskYIELD();
        }
    }
}

void kstream_master_start(kstream_master_buffers_t *buffers)
{
    s_buffers = buffers;
    if (xTaskCreate(transport_task, "kstream_spi", 4096, NULL, 8, NULL) !=
        pdPASS)
        abort();
}

void kstream_master_quiesce(void)
{
    s_quiesce_requested = true;
    __sync_synchronize();
    if (s_transport_task)
        xTaskNotifyGive(s_transport_task);
    while (!s_quiesced)
        vTaskDelay(1u);
}

size_t kstream_master_diag(char *buffer, size_t size)
{
    int length = snprintf(
        buffer, size,
        "SPI stage=%lu op=%u stream=%u seq=%lu ready=%d phase=%d faults=%lu push=%lu pull=%lu crc=%08lx/%08lx words="
        "%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,"
        "%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx\r\n",
        (unsigned long)s_diag_stage, (unsigned)s_diag_opcode,
        (unsigned)s_diag_stream, (unsigned long)s_diag_sequence,
        gpio_get_level(KSTREAM_READY_GPIO),
        gpio_get_level(KSTREAM_MASTER_PHASE_GPIO),
        (unsigned long)s_faults,
        (unsigned long)s_diag_push_bytes,
        (unsigned long)s_diag_pull_bytes,
        (unsigned long)s_diag_got_crc,
        (unsigned long)s_diag_calculated_crc,
        (unsigned long)s_diag_response_words[0],
        (unsigned long)s_diag_response_words[1],
        (unsigned long)s_diag_response_words[2],
        (unsigned long)s_diag_response_words[3],
        (unsigned long)s_diag_response_words[4],
        (unsigned long)s_diag_response_words[5],
        (unsigned long)s_diag_response_words[6],
        (unsigned long)s_diag_response_words[7],
        (unsigned long)s_diag_response_words[8],
        (unsigned long)s_diag_response_words[9],
        (unsigned long)s_diag_response_words[10],
        (unsigned long)s_diag_response_words[11],
        (unsigned long)s_diag_response_words[12],
        (unsigned long)s_diag_response_words[13],
        (unsigned long)s_diag_response_words[14],
        (unsigned long)s_diag_response_words[15]);
    return length > 0 && (size_t)length < size ? (size_t)length : 0u;
}
