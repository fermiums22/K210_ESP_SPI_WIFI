#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "spi_flash.h"
#include "tcpip_adapter.h"

#include "kstream_master.h"
#include "kstream_v2.h"

#define KESP_BUILD_ID       "kesp-master-stream-v2"
#define KESP_STA_SSID       "ELECTRONICS"
#define KESP_STA_PASSWORD   "bdc123print"
#define DOWNLINK_RING_BYTES 8192u
#define UPLINK_RING_BYTES   8192u
#define CONSOLE_RX_BYTES    2048u
#define CONSOLE_TX_BYTES    4096u
#define UPDATE_RX_BYTES     1024u
#define UPDATE_TX_BYTES     128u
#define ESP_OTA_PORT        21001u
#define ESP_OTA_MAGIC       0x41544f45u
#define ESP_OTA_FLAG_DUAL   0x00000001u
#define ESP_OTA_READY       UINT32_MAX
#define ESP_OTA_PROGRESS    (UINT32_MAX - 1u)

typedef struct __attribute__((packed)) esp_ota_request {
    uint32_t magic;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t flags;
} esp_ota_request_t;

typedef struct __attribute__((packed)) esp_ota_status {
    uint32_t magic;
    uint32_t result;
    uint32_t received;
    uint32_t detail;
} esp_ota_status_t;

static const char *TAG = "kesp";
static kstream_master_buffers_t s_streams;
static ip4_addr_t s_sta_ip;
static bool s_kstream_started;
static int s_uplink_udp_fd = -1;
static volatile uint32_t s_uplink_peer_seq;
static volatile uint32_t s_uplink_peer_addr;
static volatile uint16_t s_uplink_peer_port;
static volatile int s_console_send_errno;
static volatile uint32_t s_console_send_count;
static volatile int s_console_recv_errno;
static volatile int s_update_close_reason;
static volatile int s_update_close_errno;
static volatile uint32_t s_update_recv_bytes;
static volatile uint32_t s_update_session_counter;
static uint8_t s_update_input[KSTREAM_V2_UPDATE_DATA_BYTES];
static uint8_t s_update_output[128];
static volatile bool s_ota_active;
static volatile int s_uplink_send_errno;
static volatile uint32_t s_uplink_send_drops;

static int listener_open(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    while (length != 0u) {
        int sent = send(fd, bytes, length, 0);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(fd, &writable);
            if (select(fd + 1, NULL, &writable, NULL, NULL) > 0)
                continue;
        }
        if (sent <= 0)
            return -1;
        bytes += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static int recv_all(int fd, void *data, size_t length)
{
    uint8_t *bytes = (uint8_t *)data;
    while (length != 0u) {
        int received = recv(fd, bytes, length, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(fd, &readable);
            if (select(fd + 1, &readable, NULL, NULL, NULL) > 0)
                continue;
        }
        if (received <= 0)
            return -1;
        bytes += received;
        length -= (size_t)received;
    }
    return 0;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)data;
    while (length-- != 0u) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8u; ++bit)
            crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static int ota_status_send(int fd, uint32_t result, uint32_t received,
                           uint32_t detail)
{
    esp_ota_status_t status = {ESP_OTA_MAGIC, result, received, detail};
    return send_all(fd, &status, sizeof(status));
}

static bool ota_receive_client(int fd)
{
    esp_ota_request_t request;
    if (recv_all(fd, &request, sizeof(request)) != 0 ||
        request.magic != ESP_OTA_MAGIC ||
        request.flags != ESP_OTA_FLAG_DUAL) {
        ota_status_send(fd, 1u, 0u, 0u);
        return false;
    }
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    uint32_t slot_size = request.image_size;
    uint32_t slot_index = partition ?
        partition->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0 : 2u;
    if (!partition || slot_index > 1u || slot_size == 0u ||
        slot_size > partition->size) {
        ota_status_send(fd, 2u, 0u, partition ? partition->size : 0u);
        return false;
    }
    s_ota_active = true;
    __sync_synchronize();
    kstream_master_quiesce();
    ESP_LOGI(TAG, "OTA_PREPARE slot=%lu bytes=%lu",
             (unsigned long)slot_index, (unsigned long)slot_size);
    esp_ota_handle_t handle;
    esp_err_t error = esp_ota_begin(partition, slot_size, &handle);
    if (error != ESP_OK) {
        ota_status_send(fd, 3u, 0u, (uint32_t)error);
        return false;
    }
    vTaskDelay(1u);
    if (ota_status_send(fd, ESP_OTA_READY, 0u, partition->subtype) != 0) {
        (void)esp_ota_end(handle);
        return false;
    }
    uint32_t expected_crc;
    if (recv_all(fd, &expected_crc, sizeof(expected_crc)) != 0) {
        (void)esp_ota_end(handle);
        return false;
    }
    uint8_t buffer[1024];
    uint32_t received = 0u;
    uint32_t crc = UINT32_MAX;
    while (received < slot_size) {
        size_t count = slot_size - received;
        if (count > sizeof(buffer))
            count = sizeof(buffer);
        if (recv_all(fd, buffer, count) != 0) {
            (void)esp_ota_end(handle);
            ota_status_send(fd, 4u, received, 0u);
            return false;
        }
        crc = crc32_update(crc, buffer, count);
        error = esp_ota_write(handle, buffer, count);
        if (error != ESP_OK) {
            (void)esp_ota_end(handle);
            ota_status_send(fd, 5u, received, (uint32_t)error);
            return false;
        }
        received += count;
        if ((received & 4095u) == 0u || received == slot_size) {
            if (ota_status_send(fd, ESP_OTA_PROGRESS, received, 0u) != 0) {
                (void)esp_ota_end(handle);
                return false;
            }
        }
    }
    ESP_LOGI(TAG, "OTA_RECEIVED bytes=%lu", (unsigned long)received);
    crc ^= UINT32_MAX;
    if (crc != expected_crc) {
        (void)esp_ota_end(handle);
        ota_status_send(fd, 6u, received, crc);
        return false;
    }
    error = esp_ota_end(handle);
    if (error == ESP_OK)
        error = esp_ota_set_boot_partition(partition);
    if (error != ESP_OK) {
        ota_status_send(fd, 7u, received, (uint32_t)error);
        return false;
    }
    ota_status_send(fd, 0u, received, crc);
    return true;
}

static void ota_server_task(void *arg)
{
    (void)arg;
    int listener = listener_open(ESP_OTA_PORT);
    if (listener < 0) {
        ESP_LOGE(TAG, "OTA listen failed errno=%d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "OTA_LISTEN port=%u", ESP_OTA_PORT);
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0)
            continue;
        bool reboot = ota_receive_client(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
        if (reboot || s_ota_active) {
            vTaskDelay(pdMS_TO_TICKS(100u));
            esp_restart();
        }
    }
}

static int udp_bind(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
        return -1;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void audio_server_task(void *arg)
{
    (void)arg;
    int fd = udp_bind(KSTREAM_V2_PORT_DOWNLINK);
    if (fd < 0) {
        ESP_LOGE(TAG, "audio UDP bind failed errno=%d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "DOWNLINK_UDP port=%u", KSTREAM_V2_PORT_DOWNLINK);
    uint8_t buffer[1200];
    for (;;) {
        int count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0)
            continue;
        if (s_ota_active)
            continue;
        if (xStreamBufferSpacesAvailable(s_streams.downlink) >= (size_t)count &&
            xStreamBufferSend(s_streams.downlink, buffer, count, 0u) !=
                (size_t)count)
            abort();
    }
}

static void uplink_udp_sender_task(void *arg)
{
    (void)arg;
    uint8_t packet[1200];
    uint32_t offset = 0u;
    for (;;) {
        size_t count = xStreamBufferReceive(s_streams.uplink,
                                            packet + 8u,
                                            sizeof(packet) - 8u,
                                            portMAX_DELAY);
        uint32_t before;
        uint32_t after;
        uint32_t address;
        uint16_t port;
        do {
            before = s_uplink_peer_seq;
            __sync_synchronize();
            address = s_uplink_peer_addr;
            port = s_uplink_peer_port;
            __sync_synchronize();
            after = s_uplink_peer_seq;
        } while ((before & 1u) != 0u || before != after);
        if (count == 0u)
            continue;
        uint32_t packet_offset = offset;
        offset += count;
        if (s_ota_active || address == 0u)
            continue;
        const uint32_t magic = 0x3250554bu;
        memcpy(packet, &magic, sizeof(magic));
        memcpy(packet + 4u, &packet_offset, sizeof(packet_offset));
        struct sockaddr_in peer;
        memset(&peer, 0, sizeof(peer));
        peer.sin_family = AF_INET;
        peer.sin_addr.s_addr = address;
        peer.sin_port = port;
        if (sendto(s_uplink_udp_fd, packet, count + 8u, 0,
                   (struct sockaddr *)&peer, sizeof(peer)) < 0) {
            s_uplink_send_errno = errno;
            ++s_uplink_send_drops;
        }
    }
}

static void mic_server_task(void *arg)
{
    (void)arg;
    s_uplink_udp_fd = udp_bind(KSTREAM_V2_PORT_UPLINK);
    if (s_uplink_udp_fd < 0) {
        ESP_LOGE(TAG, "mic UDP bind failed errno=%d", errno);
        vTaskDelete(NULL);
    }
    if (xTaskCreate(uplink_udp_sender_task, "uplink_udp", 2048u, NULL, 8u,
                    NULL) != pdPASS)
        abort();
    ESP_LOGI(TAG, "UPLINK_UDP port=%u", KSTREAM_V2_PORT_UPLINK);
    uint8_t registration[16];
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s_uplink_udp_fd, &readable);
        if (select(s_uplink_udp_fd + 1, &readable, NULL, NULL, NULL) <= 0)
            continue;
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int count = recvfrom(s_uplink_udp_fd, registration,
                             sizeof(registration), 0,
                             (struct sockaddr *)&peer, &peer_length);
        if (count <= 0)
            continue;
        if (s_ota_active)
            continue;
        ++s_uplink_peer_seq;
        __sync_synchronize();
        s_uplink_peer_addr = peer.sin_addr.s_addr;
        s_uplink_peer_port = peer.sin_port;
        __sync_synchronize();
        ++s_uplink_peer_seq;
        static const char acknowledgement[] = "KUP2";
        (void)sendto(s_uplink_udp_fd, acknowledgement,
                     sizeof(acknowledgement) - 1u, 0,
                     (struct sockaddr *)&peer, peer_length);
        ESP_LOGI(TAG, "UPLINK_UDP_PEER");
    }
}

static void console_client(int fd)
{
    static const char banner[] =
        "K210 console over WiFi/SPI. Type 'help'.\r\n";
    (void)send_all(fd, banner, sizeof(banner) - 1u);
    char diagnostic[384];
    int reset_length = snprintf(diagnostic, sizeof(diagnostic),
                                "ESP reset=%d free_heap=%lu console_err=%d/%lu recv_err=%d udp_drop=%lu/%d update_close=%d/%d bytes=%lu\r\n",
                                (int)esp_reset_reason(),
                                (unsigned long)esp_get_free_heap_size(),
                                s_console_send_errno,
                                (unsigned long)s_console_send_count,
                                s_console_recv_errno,
                                (unsigned long)s_uplink_send_drops,
                                s_uplink_send_errno,
                                s_update_close_reason, s_update_close_errno,
                                (unsigned long)s_update_recv_bytes);
    if (reset_length > 0)
        (void)send_all(fd, diagnostic, (size_t)reset_length);
    size_t diagnostic_length =
        kstream_master_diag(diagnostic, sizeof(diagnostic));
    if (diagnostic_length != 0u)
        (void)send_all(fd, diagnostic, diagnostic_length);
    uint8_t input[256];
    uint8_t output[512];
    for (;;) {
        if (s_ota_active)
            break;
        size_t output_count = xStreamBufferReceive(
            s_streams.console_tx, output, sizeof(output), 0u);
        if (output_count != 0u && send_all(fd, output, output_count) != 0) {
            s_console_send_errno = errno;
            s_console_send_count = output_count;
            break;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        struct timeval poll = {0, 10000};
        int selected = select(fd + 1, &readable, NULL, NULL, &poll);
        if (selected == 0)
            continue;
        if (selected < 0)
            break;
        int count = recv(fd, input, sizeof(input), 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(fd, &readable);
            if (select(fd + 1, &readable, NULL, NULL, NULL) > 0)
                continue;
        }
        if (count <= 0) {
            s_console_recv_errno = count < 0 ? errno : 0;
            break;
        }
        if (xStreamBufferSend(s_streams.console_rx, input, count,
                              portMAX_DELAY) != (size_t)count)
            abort();
    }
    shutdown(fd, SHUT_RDWR);
}

static void console_server_task(void *arg)
{
    (void)arg;
    int listener = listener_open(KSTREAM_V2_PORT_CONSOLE);
    if (listener < 0) {
        ESP_LOGE(TAG, "console listen failed errno=%d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "CONSOLE_LISTEN port=%u", KSTREAM_V2_PORT_CONSOLE);
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0)
            continue;
        if (s_ota_active) {
            close(fd);
            continue;
        }
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        ESP_LOGI(TAG, "CONSOLE_CONNECTED");
        console_client(fd);
        close(fd);
        ESP_LOGI(TAG, "CONSOLE_CLOSED");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA_CONNECT ssid=%s", KESP_STA_SSID);
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA_ASSOCIATED");
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event =
            (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGE(TAG,
                 "STA_DISCONNECTED reason=%u bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                 event ? (unsigned)event->reason : 0u,
                 event ? event->bssid[0] : 0u, event ? event->bssid[1] : 0u,
                 event ? event->bssid[2] : 0u, event ? event->bssid[3] : 0u,
                 event ? event->bssid[4] : 0u, event ? event->bssid[5] : 0u);
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_ip = event->ip_info.ip;
        ESP_LOGI(TAG, "STA_READY ssid=%s ip=%s", KESP_STA_SSID,
                 ip4addr_ntoa(&s_sta_ip));
        if (!s_kstream_started) {
            s_kstream_started = true;
            kstream_master_start(&s_streams);
        }
    }
}

static void stream_send_exact(StreamBufferHandle_t stream, const void *data,
                              size_t length)
{
    const uint8_t *source = (const uint8_t *)data;
    while (length != 0u) {
        size_t count = xStreamBufferSend(stream, source, length, portMAX_DELAY);
        source += count;
        length -= count;
    }
}

static void stream_receive_exact(StreamBufferHandle_t stream, void *data,
                                 size_t length)
{
    uint8_t *destination = (uint8_t *)data;
    while (length != 0u) {
        size_t count = xStreamBufferReceive(stream, destination, length,
                                            portMAX_DELAY);
        destination += count;
        length -= count;
    }
}

static void update_send_record(uint32_t session, uint8_t type,
                               const void *data, uint16_t length)
{
    kstream_v2_update_record_t record;
    memset(&record, 0, sizeof(record));
    record.magic = KSTREAM_V2_UPDATE_RECORD_MAGIC;
    record.session = session;
    record.length = length;
    record.type = type;
    record.crc32 = kstream_v2_crc32(
        &record, offsetof(kstream_v2_update_record_t, crc32));
    stream_send_exact(s_streams.update_rx, &record, sizeof(record));
    if (length != 0u)
        stream_send_exact(s_streams.update_rx, data, length);
}

static bool update_forward_status(int fd, uint32_t session)
{
    if (xStreamBufferBytesAvailable(s_streams.update_tx) <
        sizeof(kstream_v2_update_record_t))
        return true;

    kstream_v2_update_record_t record;
    stream_receive_exact(s_streams.update_tx, &record, sizeof(record));
    uint32_t crc = kstream_v2_crc32(
        &record, offsetof(kstream_v2_update_record_t, crc32));
    if (record.magic != KSTREAM_V2_UPDATE_RECORD_MAGIC ||
        record.reserved != 0u || record.type != KSTREAM_V2_UPDATE_STATUS ||
        record.length > sizeof(s_update_output) || record.crc32 != crc) {
        s_update_close_reason = 6;
        s_update_close_errno = 0;
        return false;
    }
    if (record.length != 0u)
        stream_receive_exact(s_streams.update_tx, s_update_output,
                             record.length);
    if (record.session != session)
        return true;
    if (send_all(fd, s_update_output, record.length) != 0) {
        s_update_close_reason = 1;
        s_update_close_errno = errno;
        return false;
    }
    return true;
}

static void update_client(int fd)
{
    uint32_t session = ++s_update_session_counter;
    if (session == 0u)
        session = ++s_update_session_counter;
    update_send_record(session, KSTREAM_V2_UPDATE_BEGIN, NULL, 0u);

    for (;;) {
        if (!update_forward_status(fd, session))
            break;

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        struct timeval poll = {0, 10000};
        int selected = select(fd + 1, &readable, NULL, NULL, &poll);
        if (selected < 0 && errno == EINTR)
            continue;
        if (selected < 0) {
            s_update_close_reason = 2;
            s_update_close_errno = errno;
            break;
        }
        if (selected == 0)
            continue;
        int count = recv(fd, s_update_input, sizeof(s_update_input), 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (count <= 0) {
            s_update_close_reason = count == 0 ? 3 : 4;
            s_update_close_errno = count < 0 ? errno : 0;
            break;
        }
        s_update_recv_bytes += (uint32_t)count;
        update_send_record(session, KSTREAM_V2_UPDATE_DATA, s_update_input,
                           (uint16_t)count);
    }
    update_send_record(session, KSTREAM_V2_UPDATE_ABORT, NULL, 0u);
    shutdown(fd, SHUT_RDWR);
}

static void update_server_task(void *arg)
{
    (void)arg;
    int listener = listener_open(KSTREAM_V2_PORT_UPDATE);
    if (listener < 0) {
        ESP_LOGE(TAG, "K210 update listen failed errno=%d", errno);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "K210_UPDATE_LISTEN port=%u", KSTREAM_V2_PORT_UPDATE);
    for (;;) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0)
            continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0)
            (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        update_client(fd);
        close(fd);
    }
}

static void wifi_start_sta(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    uint8_t sta_mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac));
    ESP_LOGI(TAG, "STA_MAC %02x:%02x:%02x:%02x:%02x:%02x",
             sta_mac[0], sta_mac[1], sta_mac[2],
             sta_mac[3], sta_mac[4], sta_mac[5]);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t station;
    memset(&station, 0, sizeof(station));
    memcpy(station.sta.ssid, KESP_STA_SSID, sizeof(KESP_STA_SSID));
    memcpy(station.sta.password, KESP_STA_PASSWORD, sizeof(KESP_STA_PASSWORD));
    station.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &station));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "BOOT %s sdk=%s xtal=40MHz uart=115200 flash=%lu sta=%s ap=disabled",
             KESP_BUILD_ID, esp_get_idf_version(),
             (unsigned long)spi_flash_get_chip_size(), KESP_STA_SSID);
    ESP_ERROR_CHECK(nvs_flash_init());

    s_streams.downlink = xStreamBufferCreate(DOWNLINK_RING_BYTES, 1u);
    s_streams.uplink = xStreamBufferCreate(UPLINK_RING_BYTES, 1u);
    s_streams.console_rx = xStreamBufferCreate(CONSOLE_RX_BYTES, 1u);
    s_streams.console_tx = xStreamBufferCreate(CONSOLE_TX_BYTES, 1u);
    s_streams.update_rx = xStreamBufferCreate(UPDATE_RX_BYTES, 1u);
    s_streams.update_tx = xStreamBufferCreate(UPDATE_TX_BYTES, 1u);
    if (!s_streams.downlink || !s_streams.uplink ||
        !s_streams.console_rx || !s_streams.console_tx ||
        !s_streams.update_rx || !s_streams.update_tx)
        abort();

    wifi_start_sta();
    xTaskCreate(audio_server_task, "audio_tcp", 3072, NULL, 8, NULL);
    xTaskCreate(mic_server_task, "mic_tcp", 3072, NULL, 8, NULL);
    xTaskCreate(console_server_task, "console_tcp", 3072, NULL, 8, NULL);
    if (xTaskCreate(update_server_task, "k210_ota", 2048, NULL, 9, NULL) !=
        pdPASS)
        abort();
    if (xTaskCreate(ota_server_task, "esp_ota", 3072, NULL, 11, NULL) != pdPASS)
        abort();
}
