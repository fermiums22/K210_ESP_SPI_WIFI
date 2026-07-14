#include "kstream_master.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "klink_v1.h"
#include "klink_v1_link.h"
#include "kspi_v2.h"

#define KLINK_READY_GPIO GPIO_NUM_0
#define KLINK_ACK_GPIO   GPIO_NUM_3
#define WIRE_WRITE_OPCODE 2u
#define WIRE_READ_OPCODE  3u
#define ACTIVATION_TEXT   "KMASTER1"
#define PHASE_DEADLINE_MS 100u

static const char *TAG = "klink-spi";
static kstream_master_buffers_t *s_buffers;
static klink_v1_endpoint_t s_link;
static TaskHandle_t s_transport_task;
static SemaphoreHandle_t s_quiesced_event;
static volatile uint32_t s_spi_events;
static volatile uint32_t s_spi_event_flags;
static volatile uint32_t s_spi_pending_flags;
static volatile bool s_quiesce_requested;
static volatile bool s_quiesced;
static volatile bool s_link_active;
static volatile uint32_t s_stage;
static volatile uint32_t s_route_faults;
static volatile uint32_t s_descriptor_faults;
static volatile uint32_t s_phase_faults;
static volatile uint32_t s_last_descriptor;
static volatile uint32_t s_last_result;
static volatile uint8_t s_last_token;
static volatile bool s_stream_mode;
static volatile uint32_t s_stream_flags;
static volatile uint32_t s_stream_unexpected;
static volatile uint8_t s_stream_read_opcode;
static volatile uint8_t s_stream_read_address;
static volatile uint8_t s_stream_write_opcode;
static volatile uint8_t s_stream_write_address;
static volatile bool s_stream_result_pending;
static volatile bool s_stream_result_read;
static uint8_t s_exchange_token;
static uint64_t s_downlink_bytes;
static uint64_t s_uplink_bytes;
static uint64_t s_console_rx_bytes;
static uint64_t s_console_tx_bytes;
static uint64_t s_update_rx_bytes;
static uint64_t s_update_tx_bytes;
static klink_v1_cell_t s_tx __attribute__((aligned(4)));
static klink_v1_cell_t s_rx __attribute__((aligned(4)));

static uint8_t spi_slave_metadata(uint8_t *address);
static uint8_t spi_slave_capture(klink_v1_cell_t *cell, uint8_t *address);

static void notify_transport_from_isr(void)
{
    BaseType_t task_woken = pdFALSE;
    if (s_transport_task)
        vTaskNotifyGiveFromISR(s_transport_task, &task_woken);
    if (task_woken == pdTRUE)
        portYIELD_FROM_ISR();
}

static void IRAM_ATTR ack_event_cb(void *arg)
{
    (void)arg;
    if (s_stream_mode) {
        uint32_t level = (uint32_t)gpio_get_level(KLINK_ACK_GPIO);
        if (level == 0u && (s_stream_flags & SPI_SLV_RD_BUF_DONE) != 0u)
            (void)gpio_set_level(KLINK_READY_GPIO, 0u);
        if (level == 1u && (s_stream_flags & SPI_SLV_WR_BUF_DONE) != 0u)
            notify_transport_from_isr();
        return;
    }
    notify_transport_from_isr();
}

static void spi_event_cb(int event, void *arg)
{
    (void)arg;
    if (event != SPI_TRANS_DONE_EVENT)
        return;
    uint32_t flags = arg ? *(const uint32_t *)arg : 0u;
    s_spi_event_flags = flags;
    ++s_spi_events;
    if (!s_stream_mode) {
        s_spi_pending_flags |= flags;
        notify_transport_from_isr();
        return;
    }

    if ((flags & SPI_SLV_RD_BUF_DONE) != 0u) {
        uint8_t address = 0xffu;
        s_stream_read_opcode = spi_slave_metadata(&address);
        s_stream_read_address = address;
        s_stream_flags |= SPI_SLV_RD_BUF_DONE;
        if (gpio_get_level(KLINK_ACK_GPIO) == 0)
            (void)gpio_set_level(KLINK_READY_GPIO, 0u);
    }
    if ((flags & SPI_SLV_WR_BUF_DONE) != 0u) {
        uint8_t address = 0xffu;
        s_stream_write_opcode = spi_slave_capture(&s_rx, &address);
        s_stream_write_address = address;
        s_stream_flags |= SPI_SLV_WR_BUF_DONE;
        if (gpio_get_level(KLINK_ACK_GPIO) == 1)
            notify_transport_from_isr();
    }
    if ((flags & SPI_SLV_RD_STA_DONE) != 0u && s_stream_result_pending) {
        s_stream_result_read = true;
        notify_transport_from_isr();
    }
    uint32_t unexpected = flags & ~(SPI_SLV_RD_BUF_DONE |
                                    SPI_SLV_WR_BUF_DONE |
                                    SPI_SLV_RD_STA_DONE);
    if (unexpected != 0u) {
        s_stream_unexpected |= unexpected;
        notify_transport_from_isr();
    }
}

static void ready_takeover_high(void)
{
    ESP_ERROR_CHECK(gpio_set_level(KLINK_READY_GPIO, 1u));
    ESP_ERROR_CHECK(gpio_set_direction(KLINK_READY_GPIO, GPIO_MODE_OUTPUT));
}

static void ready_set(uint32_t level)
{
    ESP_ERROR_CHECK(gpio_set_level(KLINK_READY_GPIO, level));
}

static uint32_t ack_level(void)
{
    return (uint32_t)gpio_get_level(KLINK_ACK_GPIO);
}

static void ack_input_init(void)
{
    gpio_config_t config;
    memset(&config, 0, sizeof(config));
    config.intr_type = GPIO_INTR_ANYEDGE;
    config.mode = GPIO_MODE_INPUT;
    config.pin_bit_mask = 1ULL << KLINK_ACK_GPIO;
    config.pull_down_en = 0;
    config.pull_up_en = 1;
    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(KLINK_ACK_GPIO, ack_event_cb, NULL));
}

static void spi_slave_init(void)
{
    spi_config_t config;
    memset(&config, 0, sizeof(config));
    config.interface.val = SPI_DEFAULT_INTERFACE;
    /* Keep the SDK slave wire order.  Overriding the bit/byte-order fields
     * changes how the hardware command register and 32-bit data buffer map
     * an otherwise standard MSB-first SPI transaction. */
    config.intr_enable.val = SPI_SLAVE_DEFAULT_INTR_ENABLE;
    config.event_cb = spi_event_cb;
    config.mode = SPI_SLAVE_MODE;
    config.clk_div = SPI_2MHz_DIV;
    ESP_ERROR_CHECK(spi_init(HSPI_HOST, &config));
}

static void spi_slave_load(const klink_v1_cell_t *cell)
{
    uint16_t command = 0u;
    uint32_t address = 0u;
    spi_trans_t trans;
    memset(&trans, 0, sizeof(trans));
    trans.cmd = &command;
    trans.addr = &address;
    trans.miso = (uint32_t *)cell;
    trans.bits.cmd = 8u;
    trans.bits.addr = 8u;
    trans.bits.miso = KLINK_V1_CELL_BYTES * 8u;
    ESP_ERROR_CHECK(spi_trans(HSPI_HOST, &trans));
}

static uint8_t spi_slave_metadata(uint8_t *address)
{
    uint16_t command = 0u;
    uint32_t wire_address = 0u;
    spi_trans_t trans;
    memset(&trans, 0, sizeof(trans));
    trans.cmd = &command;
    trans.addr = &wire_address;
    trans.bits.cmd = 8u;
    trans.bits.addr = 8u;
    ESP_ERROR_CHECK(spi_trans(HSPI_HOST, &trans));
    *address = (uint8_t)wire_address;
    return (uint8_t)command;
}

static uint8_t spi_slave_capture(klink_v1_cell_t *cell, uint8_t *address)
{
    uint16_t command = 0u;
    uint32_t wire_address = 0u;
    spi_trans_t trans;
    memset(&trans, 0, sizeof(trans));
    trans.cmd = &command;
    trans.addr = &wire_address;
    trans.mosi = (uint32_t *)cell;
    trans.bits.cmd = 8u;
    trans.bits.addr = 8u;
    trans.bits.mosi = KLINK_V1_CELL_BYTES * 8u;
    ESP_ERROR_CHECK(spi_trans(HSPI_HOST, &trans));
    *address = (uint8_t)wire_address;
    return (uint8_t)command;
}

static void clear_spi_flag(uint32_t flag)
{
    s_spi_pending_flags &= ~flag;
}

static bool wait_spi_flag(uint32_t flag)
{
    while ((s_spi_pending_flags & flag) == 0u && !s_quiesce_requested)
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_spi_pending_flags &= ~flag;
    return !s_quiesce_requested;
}

static bool wait_phase(uint32_t expected_ack, uint32_t *observed_flags)
{
    TickType_t started = xTaskGetTickCount();
    TickType_t deadline = pdMS_TO_TICKS(PHASE_DEADLINE_MS);
    while (s_spi_pending_flags == 0u || ack_level() != expected_ack) {
        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= deadline)
            break;
        (void)ulTaskNotifyTake(pdTRUE, deadline - elapsed);
    }
    *observed_flags = s_spi_pending_flags;
    s_spi_pending_flags = 0u;
    return *observed_flags != 0u && ack_level() == expected_ack;
}

static void set_first_error(uint8_t *error, uint8_t value)
{
    if (*error == KSPI_V2_ERROR_NONE)
        *error = value;
}

static bool stream_to_cell(StreamBufferHandle_t stream, uint8_t channel,
                           uint16_t flags, uint64_t *counter)
{
    if ((s_link.tx_queued_mask & (1u << channel)) != 0u)
        return false;
    uint8_t payload[KLINK_V1_PAYLOAD_BYTES];
    size_t count = xStreamBufferReceive(stream, payload, sizeof(payload), 0u);
    if (count == 0u)
        return false;
    if (!klink_v1_queue(&s_link, channel, KLINK_T_DATA, flags,
                        payload, count)) {
        ++s_route_faults;
        return false;
    }
    *counter += count;
    return true;
}

static void prepare_tx(void)
{
    if (!s_link.inflight && s_link.tx_queued_mask == 0u) {
        if (!stream_to_cell(s_buffers->update_rx, KLINK_CH_BULK,
                            KLINK_F_RELIABLE, &s_update_rx_bytes) &&
            !stream_to_cell(s_buffers->console_rx, KLINK_CH_DIAG,
                            KLINK_F_RELIABLE, &s_console_rx_bytes))
            (void)stream_to_cell(s_buffers->downlink, KLINK_CH_AUDIO_OUT,
                                 0u, &s_downlink_bytes);
    }
    klink_v1_build_tx(&s_link, &s_tx);
}

static void stamp_stream_result(uint8_t error, uint8_t token)
{
    s_tx.flags = kspi_v2_cell_result_flags(s_tx.flags, error, token);
    klink_v1_cell_finalize(&s_tx);
}

static bool cell_to_stream(StreamBufferHandle_t stream,
                           const klink_v1_cell_t *cell, uint64_t *counter)
{
    size_t count = cell->payload_length;
    /* A full route buffer is flow control, not a corrupt KLINK cell.  Blocking
     * here lets the socket worker drain the exact bytes and prevents a
     * high-priority SPI loop from starving its own consumer. */
    if (xStreamBufferSend(stream, cell->payload, count, portMAX_DELAY) != count) {
        ++s_route_faults;
        return false;
    }
    *counter += count;
    return true;
}

static bool process_rx(void)
{
    klink_v1_event_t event;
    uint32_t flags = klink_v1_process_rx(&s_link, &s_rx, &event);
    if ((flags & KLINK_EVENT_FAULT) != 0u)
        return false;
    if ((flags & KLINK_EVENT_RX) == 0u)
        return true;

    bool accepted = false;
    if (event.rx_channel == KLINK_CH_AUDIO_IN)
        accepted = cell_to_stream(s_buffers->uplink, &s_rx, &s_uplink_bytes);
    else if (event.rx_channel == KLINK_CH_DIAG)
        accepted = cell_to_stream(s_buffers->console_tx, &s_rx,
                                  &s_console_tx_bytes);
    else if (event.rx_channel == KLINK_CH_BULK)
        accepted = cell_to_stream(s_buffers->update_tx, &s_rx,
                                  &s_update_tx_bytes);
    else {
        ++s_route_faults;
        return false;
    }
    if (accepted && (s_rx.flags & KLINK_F_RELIABLE) != 0u)
        (void)klink_v1_release_credit(&s_link, event.rx_channel, 1u);
    return accepted;
}

static bool activation_valid(const klink_v1_cell_t *cell, uint8_t opcode,
                             uint8_t address)
{
    return opcode == WIRE_WRITE_OPCODE && address == KSPI_V2_REGION_KLINK &&
           klink_v1_cell_validate(cell) &&
           KLINK_CHANNEL(cell->channel_type) == KLINK_CH_CONTROL &&
           KLINK_TYPE(cell->channel_type) == KLINK_T_OPEN &&
           cell->payload_length == sizeof(ACTIVATION_TEXT) - 1u &&
           memcmp(cell->payload, ACTIVATION_TEXT,
                  sizeof(ACTIVATION_TEXT) - 1u) == 0;
}

static void enter_quiesced(void)
{
    ready_set(0u);
    s_quiesced = true;
    __sync_synchronize();
    if (s_quiesced_event)
        (void)xSemaphoreGive(s_quiesced_event);
    for (;;)
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static void transport_task(void *arg)
{
    (void)arg;
    s_transport_task = xTaskGetCurrentTaskHandle();
    klink_v1_endpoint_init(&s_link, 31u);
    ack_input_init();
    spi_slave_init();
    memset(&s_tx, 0, sizeof(s_tx));
    memset(&s_rx, 0, sizeof(s_rx));
    spi_slave_load(&s_tx);
    s_stage = 1u;
    clear_spi_flag(SPI_SLV_WR_BUF_DONE);
    ESP_LOGI(TAG,
             "KLINK_ARMED role=slave hspi=mode0 gpio=12/13/14/15 ready=gpio0 ack=gpio3");

    if (!wait_spi_flag(SPI_SLV_WR_BUF_DONE))
        enter_quiesced();
    uint8_t address = 0u;
    uint8_t opcode = spi_slave_capture(&s_rx, &address);
    if (!activation_valid(&s_rx, opcode, address)) {
        const uint8_t *rx = (const uint8_t *)&s_rx;
        ++s_route_faults;
        ESP_LOGE(TAG,
                 "ACTIVATION_INVALID opcode=%u address=%u irq=%02lx rx=%02x%02x%02x%02x/%02x%02x%02x%02x",
                 (unsigned)opcode, (unsigned)address,
                 (unsigned long)s_spi_event_flags,
                 rx[0], rx[1], rx[2], rx[3], rx[4], rx[5], rx[6], rx[7]);
        vTaskDelete(NULL);
    }
    ready_takeover_high();
    s_link_active = true;
    prepare_tx();
    stamp_stream_result(KSPI_V2_ERROR_NONE, 0u);
    spi_slave_load(&s_tx);
    s_spi_pending_flags = 0u;
    ready_set(1u);
    s_stage = 3u;
    ESP_LOGI(TAG,
             "KLINK_ACTIVE role=slave clock=master-controlled cell=64 register-protocol=2");

    /* The master registers the known memory-window contract once. */
    uint8_t error = KSPI_V2_ERROR_NONE;
    uint32_t observed = 0u;
    uint32_t descriptor = 0u;
    if (!wait_phase(0u, &observed))
        set_first_error(&error, KSPI_V2_ERROR_DESCRIPTOR);
    if (observed == SPI_SLV_WR_STA_DONE) {
        ESP_ERROR_CHECK(spi_slave_get_status(HSPI_HOST, &descriptor));
    } else {
        set_first_error(&error, KSPI_V2_ERROR_DESCRIPTOR);
    }
    s_last_descriptor = descriptor;
    s_last_token = kspi_v2_descriptor_token(descriptor);
    if (!kspi_v2_descriptor_valid(descriptor))
        set_first_error(&error, KSPI_V2_ERROR_DESCRIPTOR);
    if (error != KSPI_V2_ERROR_NONE)
        ++s_descriptor_faults;
    uint32_t result = kspi_v2_result(error, KSPI_V2_PHASE_COMPLETE,
                                     s_last_token);
    s_last_result = result;
    ESP_ERROR_CHECK(spi_slave_set_status(HSPI_HOST, &result));
    ready_set(0u);
    observed = 0u;
    bool contract_result_ok = wait_phase(1u, &observed) &&
                              observed == SPI_SLV_RD_STA_DONE;
    if (!contract_result_ok || error != KSPI_V2_ERROR_NONE) {
        ready_set(1u);
        ++s_route_faults;
        s_stage = 0x80u | (uint32_t)error;
        ESP_LOGE(TAG,
                 "CONTRACT_FATAL error=%u desc=%08lx result=%08lx irq=%02lx ack=%lu",
                 (unsigned)error, (unsigned long)descriptor,
                 (unsigned long)result, (unsigned long)observed,
                 (unsigned long)ack_level());
        vTaskDelete(NULL);
    }

    s_exchange_token = 0u;
    s_stream_flags = 0u;
    s_stream_unexpected = 0u;
    s_stream_result_pending = false;
    s_stream_result_read = false;
    s_stream_mode = true;
    ready_set(1u);
    ESP_LOGI(TAG, "KLINK_CONTRACT region=0 operation=exchange bytes=64");

    for (;;) {
        s_stage = 3u;
        while ((((s_stream_flags & SPI_SLV_WR_BUF_DONE) == 0u) ||
                ack_level() != 1u) &&
               s_stream_unexpected == 0u)
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t stream_flags = s_stream_flags;
        uint32_t unexpected = s_stream_unexpected;
        error = KSPI_V2_ERROR_NONE;
        if (unexpected != 0u ||
            (stream_flags & (SPI_SLV_RD_BUF_DONE | SPI_SLV_WR_BUF_DONE)) !=
                (SPI_SLV_RD_BUF_DONE | SPI_SLV_WR_BUF_DONE) ||
            s_stream_read_opcode != WIRE_READ_OPCODE ||
            s_stream_read_address != KSPI_V2_REGION_KLINK) {
            ++s_phase_faults;
            set_first_error(&error, KSPI_V2_ERROR_READ_PHASE);
        }
        if (s_stream_write_opcode != WIRE_WRITE_OPCODE ||
            s_stream_write_address != KSPI_V2_REGION_KLINK) {
            ++s_phase_faults;
            set_first_error(&error, KSPI_V2_ERROR_WRITE_PHASE);
        }

        uint8_t expected_token = (uint8_t)(s_exchange_token + 1u);
        if (error == KSPI_V2_ERROR_NONE &&
            (!klink_v1_cell_validate(&s_rx) ||
             kspi_v2_cell_result(s_rx.flags) != KSPI_V2_CELL_RESULT_OK ||
             kspi_v2_cell_token(s_rx.flags) != expected_token)) {
            ++s_route_faults;
            set_first_error(&error, KSPI_V2_ERROR_CELL);
        }

        s_stage = 4u;
        if (error == KSPI_V2_ERROR_NONE && !process_rx()) {
            ++s_route_faults;
            set_first_error(&error, KSPI_V2_ERROR_CELL);
        }
        prepare_tx();
        if (error == KSPI_V2_ERROR_NONE && s_quiesce_requested)
            error = KSPI_V2_ERROR_QUIESCE;

        ++s_exchange_token;
        s_last_token = s_exchange_token;
        stamp_stream_result(error, s_exchange_token);
        spi_slave_load(&s_tx);
        result = kspi_v2_result(error, KSPI_V2_PHASE_COMPLETE,
                                s_exchange_token);
        s_last_result = result;
        ESP_ERROR_CHECK(spi_slave_set_status(HSPI_HOST, &result));

        s_stream_flags = 0u;
        s_stream_unexpected = 0u;
        bool terminal = error != KSPI_V2_ERROR_NONE;
        s_stream_result_read = false;
        s_stream_result_pending = false;
        s_stage = 6u;
        ready_set(1u);

        if (terminal) {
            /* The terminal result is carried by this CRC-protected cell.
             * Wait until the master has read it and acknowledged the READ
             * phase; no separate per-cell status transaction is required. */
            while (((s_stream_flags & SPI_SLV_RD_BUF_DONE) == 0u) ||
                   ack_level() != 0u)
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (error == KSPI_V2_ERROR_QUIESCE)
                enter_quiesced();
            ++s_route_faults;
            s_stage = 0x80u | (uint32_t)error;
            ESP_LOGE(TAG,
                     "TRANSACTION_FATAL error=%u desc=%08lx result=%08lx flags=%02lx unexpected=%02lx ack=%lu",
                     (unsigned)error, (unsigned long)descriptor,
                     (unsigned long)result, (unsigned long)stream_flags,
                     (unsigned long)unexpected, (unsigned long)ack_level());
            vTaskDelete(NULL);
        }
    }
}

void kstream_master_start(kstream_master_buffers_t *buffers)
{
    s_buffers = buffers;
    s_quiesced_event = xSemaphoreCreateBinary();
    if (!s_quiesced_event ||
        xTaskCreate(transport_task, "klink_slave", 4096u, NULL, 9u,
                    NULL) != pdPASS)
        abort();
}

void kstream_master_quiesce(void)
{
    s_quiesce_requested = true;
    __sync_synchronize();
    if (!s_transport_task) {
        s_quiesced = true;
        return;
    }
    xTaskNotifyGive(s_transport_task);
    if (!s_quiesced)
        (void)xSemaphoreTake(s_quiesced_event, portMAX_DELAY);
}

size_t kstream_master_diag(char *buffer, size_t size)
{
    int length = snprintf(
        buffer, size,
        "KLINK role=slave active=%u stage=%lu ready=%d ack=%lu events=%lu faults=%lu/%lu descriptor=%lu phase=%lu token=%u desc=%08lx result=%08lx tx=%lu rx=%lu bad=%lu gaps=%lu down=%lu up=%lu console=%lu/%lu update=%lu/%lu\r\n",
        s_link_active ? 1u : 0u, (unsigned long)s_stage,
        gpio_get_level(KLINK_READY_GPIO), (unsigned long)ack_level(),
        (unsigned long)s_spi_events,
        (unsigned long)s_link.stats.faults, (unsigned long)s_route_faults,
        (unsigned long)s_descriptor_faults, (unsigned long)s_phase_faults,
        (unsigned)s_last_token, (unsigned long)s_last_descriptor,
        (unsigned long)s_last_result,
        (unsigned long)s_link.stats.tx_cells,
        (unsigned long)s_link.stats.rx_cells,
        (unsigned long)s_link.stats.bad_cells,
        (unsigned long)s_link.stats.rx_realtime_gaps,
        (unsigned long)s_downlink_bytes, (unsigned long)s_uplink_bytes,
        (unsigned long)s_console_rx_bytes, (unsigned long)s_console_tx_bytes,
        (unsigned long)s_update_rx_bytes, (unsigned long)s_update_tx_bytes);
    if (length <= 0)
        return 0u;
    return (size_t)length < size ? (size_t)length : size;
}
