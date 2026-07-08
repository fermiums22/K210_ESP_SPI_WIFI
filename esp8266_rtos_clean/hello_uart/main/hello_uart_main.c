#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "spi_flash.h"
#include "tcpip_adapter.h"
#include "driver/spi.h"

#define KESP_VERSION           "0.3.0-wifi-spi-sd"
#define KESP_CONFIG_FLASH_ADDR 0x000E0000u
#define KESP_CONFIG_MAGIC      "KESPJSON"
#define KESP_TCP_PORT_DEFAULT  18080
#define KESP_STA_SSID_DEFAULT  "ELECTRONICS"
#define KESP_STA_PASS_DEFAULT  "bdc123print"
#define KESP_AP_SSID           "KESP-SD"
#define KESP_AP_PASS           ""

#define KESP_FRAME_WORDS       16u
#define KESP_FRAME_BYTES       (KESP_FRAME_WORDS * 4u)
#define KESP_FRAME_BITS        (KESP_FRAME_WORDS * 32u)
#define KESP_PAYLOAD_BYTES     40u
#define KESP_QUEUE_DEPTH       24u
#define KESP_MAGIC_LE          0x5053454bUL

enum {
    KESP_F_IDLE = 1,
    KESP_F_BEGIN = 2,
    KESP_F_DATA = 3,
    KESP_F_END = 4,
    KESP_F_ERROR = 5,
};

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t len;
    uint32_t seq;
    uint32_t total;
    uint32_t offset;
    uint32_t reserved;
    uint8_t payload[KESP_PAYLOAD_BYTES];
} kesp_frame_t;

typedef struct {
    char mode[16];
    char ssid[33];
    char pass[65];
    int tcp_port;
} kesp_config_t;

static const char *TAG = "kesp";
static EventGroupHandle_t s_wifi_events;
static QueueHandle_t s_spi_q;
static uint32_t s_spi_miso[KESP_FRAME_WORDS] __attribute__((aligned(4)));
static uint32_t s_spi_mosi[KESP_FRAME_WORDS] __attribute__((aligned(4)));
static volatile uint32_t s_spi_irq_events;
static volatile uint32_t s_tcp_files;
static volatile uint32_t s_tcp_bytes;
static kesp_config_t s_cfg = {
    .mode = "sta_ap",
    .ssid = KESP_STA_SSID_DEFAULT,
    .pass = KESP_STA_PASS_DEFAULT,
    .tcp_port = KESP_TCP_PORT_DEFAULT,
};

#define WIFI_CONNECTED_BIT BIT0

static char *json_find(char *json, const char *key)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static void json_get_string(char *json, const char *key, char *out, size_t out_size)
{
    char *p = json_find(json, key);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_size) out[n++] = *p++;
    out[n] = 0;
}

static void copy_cstr(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) return;
    memset(dst, 0, dst_size);
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
}

static void json_get_int(char *json, const char *key, int *out)
{
    char *p = json_find(json, key);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    *out = atoi(p + 1);
}

static void load_flash_config(void)
{
    uint8_t raw[1024];
    memset(raw, 0, sizeof(raw));
    if (spi_flash_read(KESP_CONFIG_FLASH_ADDR, raw, sizeof(raw)) != ESP_OK) {
        ESP_LOGW(TAG, "kesp: config flash read failed, using defaults");
        return;
    }

    char *json = (char *)raw;
    if (memcmp(json, KESP_CONFIG_MAGIC, strlen(KESP_CONFIG_MAGIC)) != 0) {
        ESP_LOGW(TAG, "kesp: config json missing at 0x%06x, using defaults", KESP_CONFIG_FLASH_ADDR);
        return;
    }
    json += strlen(KESP_CONFIG_MAGIC);
    while (*json == '\r' || *json == '\n' || *json == ' ') json++;
    json_get_string(json, "mode", s_cfg.mode, sizeof(s_cfg.mode));
    json_get_string(json, "ssid", s_cfg.ssid, sizeof(s_cfg.ssid));
    json_get_string(json, "pass", s_cfg.pass, sizeof(s_cfg.pass));
    json_get_int(json, "tcp_port", &s_cfg.tcp_port);
    if (s_cfg.tcp_port <= 0 || s_cfg.tcp_port > 65535) {
        s_cfg.tcp_port = KESP_TCP_PORT_DEFAULT;
    }
    ESP_LOGI(TAG, "kesp: config loaded mode=%s ssid=%s tcp_port=%d",
             s_cfg.mode, s_cfg.ssid[0] ? s_cfg.ssid : "<empty>", s_cfg.tcp_port);
}

static void spi_event_cb(int event, void *arg)
{
    (void)arg;
    if (event == SPI_TRANS_DONE_EVENT) {
        s_spi_irq_events++;
    }
}

static void make_idle_frame(kesp_frame_t *f)
{
    memset(f, 0, sizeof(*f));
    f->magic = KESP_MAGIC_LE;
    f->version = 1;
    f->type = KESP_F_IDLE;
    f->seq = s_spi_irq_events;
    f->total = s_tcp_bytes;
    f->offset = s_tcp_files;
}

static void enqueue_frame(const kesp_frame_t *f)
{
    while (xQueueSend(s_spi_q, f, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "kesp: spi queue full");
    }
}

static void enqueue_named(uint8_t type, const char *name, uint32_t size, uint32_t seq)
{
    kesp_frame_t f;
    memset(&f, 0, sizeof(f));
    f.magic = KESP_MAGIC_LE;
    f.version = 1;
    f.type = type;
    f.seq = seq;
    f.total = size;
    snprintf((char *)f.payload, sizeof(f.payload), "%s", name);
    f.len = strlen((const char *)f.payload);
    enqueue_frame(&f);
}

static void enqueue_error(const char *msg, uint32_t seq)
{
    enqueue_named(KESP_F_ERROR, msg, 0, seq);
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
    cfg.clk_div = SPI_2MHz_DIV;
    return spi_init(HSPI_HOST, &cfg);
}

static void spi_slave_task(void *arg)
{
    (void)arg;
    uint16_t cmd = 0;
    uint32_t addr = 0;
    kesp_frame_t f;

    ESP_LOGI(TAG, "kesp: spi slave ready hspi gpio12=miso gpio13=mosi gpio14=clk gpio15=cs mode=0");
    for (;;) {
        if (xQueueReceive(s_spi_q, &f, 0) != pdTRUE) {
            make_idle_frame(&f);
        }
        memcpy(s_spi_miso, &f, sizeof(f));
        memset(s_spi_mosi, 0, sizeof(s_spi_mosi));

        spi_trans_t trans;
        memset(&trans, 0, sizeof(trans));
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
    }
}

static int recv_all(int fd, uint8_t *buf, int len)
{
    int got = 0;
    while (got < len) {
        int r = recv(fd, buf + got, len - got, 0);
        if (r <= 0) return -1;
        got += r;
    }
    return got;
}

static int recv_line(int fd, char *out, int out_len)
{
    int n = 0;
    while (n < out_len - 1) {
        char c;
        int r = recv(fd, &c, 1, 0);
        if (r <= 0) return 0;
        if (c == '\n') {
            out[n] = 0;
            return 1;
        }
        if (c != '\r') out[n++] = c;
    }
    out[n] = 0;
    return 1;
}

static void handle_client(int fd)
{
    char line[96];
    char name[40];
    unsigned long size_ul;
    uint8_t chunk[KESP_PAYLOAD_BYTES];
    uint32_t seq = 1;
    uint32_t offset = 0;

    if (!recv_line(fd, line, sizeof(line)) ||
        sscanf(line, "PUT %39s %lu", name, &size_ul) != 2 ||
        size_ul == 0 || size_ul > 16u * 1024u * 1024u) {
        const char *err = "ERR bad-command\n";
        send(fd, err, strlen(err), 0);
        return;
    }

    const uint32_t size = (uint32_t)size_ul;
    ESP_LOGI(TAG, "kesp: PUT %s %lu", name, (unsigned long)size);
    enqueue_named(KESP_F_BEGIN, name, size, seq++);

    while (offset < size) {
        uint32_t n = size - offset;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (recv_all(fd, chunk, (int)n) != (int)n) {
            ESP_LOGE(TAG, "kesp: PUT short %s %lu/%lu", name, (unsigned long)offset, (unsigned long)size);
            enqueue_error("short-read", seq++);
            return;
        }

        kesp_frame_t f;
        memset(&f, 0, sizeof(f));
        f.magic = KESP_MAGIC_LE;
        f.version = 1;
        f.type = KESP_F_DATA;
        f.len = n;
        f.seq = seq++;
        f.total = size;
        f.offset = offset;
        memcpy(f.payload, chunk, n);
        enqueue_frame(&f);
        offset += n;
    }

    enqueue_named(KESP_F_END, name, size, seq++);
    s_tcp_files++;
    s_tcp_bytes += size;
    ESP_LOGI(TAG, "kesp: DONE %s %lu", name, (unsigned long)size);
    {
        const char *ok = "OK queued\n";
        send(fd, ok, strlen(ok), 0);
    }
}

static void tcp_server_task(void *arg)
{
    (void)arg;
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "kesp: socket failed errno=%d", errno);
        vTaskDelete(NULL);
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)s_cfg.tcp_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_sock, 2) != 0) {
        ESP_LOGE(TAG, "kesp: bind/listen failed errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "kesp: tcp server ready port=%d", s_cfg.tcp_port);
    for (;;) {
        struct sockaddr_in remote;
        socklen_t len = sizeof(remote);
        int fd = accept(listen_sock, (struct sockaddr *)&remote, &len);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        handle_client(fd);
        shutdown(fd, 0);
        close(fd);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "kesp: wifi disconnected, reconnect");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "kesp: wifi connected ip=%s", ip4addr_ntoa(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_mode_t mode = WIFI_MODE_APSTA;
    if (strcmp(s_cfg.mode, "sta") == 0) mode = WIFI_MODE_STA;
    if (strcmp(s_cfg.mode, "ap") == 0) mode = s_cfg.ssid[0] ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        wifi_config_t ap = {0};
        copy_cstr((char *)ap.ap.ssid, sizeof(ap.ap.ssid), KESP_AP_SSID);
        copy_cstr((char *)ap.ap.password, sizeof(ap.ap.password), KESP_AP_PASS);
        ap.ap.ssid_len = strlen(KESP_AP_SSID);
        ap.ap.max_connection = 2;
        ap.ap.authmode = strlen(KESP_AP_PASS) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap));
        ESP_LOGI(TAG, "kesp: fallback AP ssid=%s ip=192.168.4.1", KESP_AP_SSID);
    }

    if ((mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) && s_cfg.ssid[0]) {
        wifi_config_t sta = {0};
        copy_cstr((char *)sta.sta.ssid, sizeof(sta.sta.ssid), s_cfg.ssid);
        copy_cstr((char *)sta.sta.password, sizeof(sta.sta.password), s_cfg.pass);
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta));
        ESP_LOGI(TAG, "kesp: wifi begin ssid=%s", s_cfg.ssid);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "kesp: boot version=%s", KESP_VERSION);
    ESP_ERROR_CHECK(nvs_flash_init());
    load_flash_config();

    s_spi_q = xQueueCreate(KESP_QUEUE_DEPTH, sizeof(kesp_frame_t));
    ESP_ERROR_CHECK(spi_slave_start());
    xTaskCreate(spi_slave_task, "kesp_spi", 3072, NULL, 6, NULL);

    wifi_start();
    xTaskCreate(tcp_server_task, "kesp_tcp", 4096, NULL, 5, NULL);
}
