#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
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

static const char *TAG = "kesp";
static kstream_master_buffers_t s_streams;
static ip4_addr_t s_sta_ip;
static bool s_kstream_started;
static int s_uplink_udp_fd = -1;
static volatile uint32_t s_uplink_peer_seq;
static volatile uint32_t s_uplink_peer_addr;
static volatile uint16_t s_uplink_peer_port;

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
        if (sent <= 0)
            return -1;
        bytes += sent;
        length -= (size_t)sent;
    }
    return 0;
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
        if (address == 0u || count == 0u)
            continue;
        const uint32_t magic = 0x3250554bu;
        memcpy(packet, &magic, sizeof(magic));
        memcpy(packet + 4u, &offset, sizeof(offset));
        struct sockaddr_in peer;
        memset(&peer, 0, sizeof(peer));
        peer.sin_family = AF_INET;
        peer.sin_addr.s_addr = address;
        peer.sin_port = port;
        (void)sendto(s_uplink_udp_fd, packet, count + 8u, 0,
                     (struct sockaddr *)&peer, sizeof(peer));
        offset += count;
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
    if (xTaskCreate(uplink_udp_sender_task, "uplink_udp", 2048u, NULL, 9u,
                    NULL) != pdPASS)
        abort();
    ESP_LOGI(TAG, "UPLINK_UDP port=%u", KSTREAM_V2_PORT_UPLINK);
    uint8_t registration[16];
    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int count = recvfrom(s_uplink_udp_fd, registration,
                             sizeof(registration), 0,
                             (struct sockaddr *)&peer, &peer_length);
        if (count <= 0)
            continue;
        ++s_uplink_peer_seq;
        __sync_synchronize();
        s_uplink_peer_addr = peer.sin_addr.s_addr;
        s_uplink_peer_port = peer.sin_port;
        __sync_synchronize();
        ++s_uplink_peer_seq;
        ESP_LOGI(TAG, "UPLINK_UDP_PEER");
    }
}

typedef struct console_writer_context {
    int fd;
} console_writer_context_t;

static void console_writer_task(void *arg)
{
    console_writer_context_t *context = (console_writer_context_t *)arg;
    uint8_t output[512];
    for (;;) {
        size_t count = xStreamBufferReceive(s_streams.console_tx, output,
                                            sizeof(output), portMAX_DELAY);
        if (count != 0u && send_all(context->fd, output, count) != 0) {
            shutdown(context->fd, SHUT_RDWR);
            vTaskSuspend(NULL);
        }
    }
}

static void console_client(int fd)
{
    static const char banner[] =
        "K210 console over WiFi/SPI. Type 'help'.\r\n";
    (void)send_all(fd, banner, sizeof(banner) - 1u);
    char diagnostic[384];
    size_t diagnostic_length =
        kstream_master_diag(diagnostic, sizeof(diagnostic));
    if (diagnostic_length != 0u)
        (void)send_all(fd, diagnostic, diagnostic_length);
    console_writer_context_t writer_context = {fd};
    TaskHandle_t writer = NULL;
    if (xTaskCreate(console_writer_task, "console_tx", 2048u,
                    &writer_context, 9u, &writer) != pdPASS)
        return;
    uint8_t input[256];
    for (;;) {
        int count = recv(fd, input, sizeof(input), 0);
        if (count <= 0)
            break;
        if (xStreamBufferSend(s_streams.console_rx, input, count,
                              portMAX_DELAY) != (size_t)count)
            abort();
    }
    shutdown(fd, SHUT_RDWR);
    vTaskDelete(writer);
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
    if (!s_streams.downlink || !s_streams.uplink ||
        !s_streams.console_rx || !s_streams.console_tx)
        abort();

    wifi_start_sta();
    xTaskCreate(audio_server_task, "audio_tcp", 3072, NULL, 9, NULL);
    xTaskCreate(mic_server_task, "mic_tcp", 3072, NULL, 9, NULL);
    xTaskCreate(console_server_task, "console_tcp", 3072, NULL, 9, NULL);
}
