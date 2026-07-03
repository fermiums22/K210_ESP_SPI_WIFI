#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SPISlave.h>

#ifndef KESP_VERSION
#define KESP_VERSION "dev"
#endif

static const char *WIFI_SSID = "Fermiums_2.4";
static const char *WIFI_PASS = "876543212";
static const uint16_t TCP_PORT = 7777;
static const uint32_t FRAME_MAGIC = 0x5053454bUL;
static const uint32_t UART_BAUD = 115200;
static const size_t DATA_BYTES = 20;
static const size_t QUEUE_SIZE = 64;
static const uint32_t AP_FALLBACK_MS = 30000;

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

enum { FT_IDLE = 0, FT_BEGIN = 1, FT_DATA = 2, FT_END = 3 };

struct __attribute__((packed)) Frame {
    uint32_t magic;
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    uint32_t value;
    uint8_t data[DATA_BYTES];
};

static WiFiServer server(TCP_PORT);
static Frame queueBuf[QUEUE_SIZE];
static Frame spiTxFrame;
static volatile uint8_t qHead;
static volatile uint8_t qTail;
static bool spiReady;
static uint8_t seqNo;
static uint32_t rxBytes;
static uint32_t lastBytes;
static uint32_t lastMs;
static uint32_t bootMs;
static uint32_t lastWifiLogMs;
static bool ledState;
static bool apStarted;
static wl_status_t lastWifiStatus = WL_IDLE_STATUS;
static char apSsid[24];

static void logLine(const __FlashStringHelper *msg)
{
    Serial.print(F("kesp: "));
    Serial.println(msg);
}

static const char *wifiStatusName(wl_status_t st)
{
    switch (st) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
    }
}

static void makeIdle(Frame &f)
{
    memset(&f, 0, sizeof(f));
    f.magic = FRAME_MAGIC;
    f.type = FT_IDLE;
}

static bool enqueue(const Frame &f)
{
    uint8_t next = (qHead + 1) % QUEUE_SIZE;
    while (next == qTail) {
        delay(1);
        yield();
    }
    noInterrupts();
    queueBuf[qHead] = f;
    qHead = next;
    interrupts();
    return true;
}

static bool dequeue(Frame &f)
{
    if (qTail == qHead)
        return false;
    noInterrupts();
    f = queueBuf[qTail];
    qTail = (qTail + 1) % QUEUE_SIZE;
    interrupts();
    return true;
}

static void spiLoadNext()
{
    if (!dequeue(spiTxFrame))
        makeIdle(spiTxFrame);
    SPISlave.setData((uint8_t *)&spiTxFrame, sizeof(spiTxFrame));
}

static void spiStartOnce()
{
    if (spiReady)
        return;

    SPISlave.onData([](uint8_t *data, size_t len) {
        (void)data;
        (void)len;
        spiLoadNext();
    });
    SPISlave.onDataSent([]() {
        spiLoadNext();
    });
    SPISlave.begin();
    spiLoadNext();
    spiReady = true;
    Serial.println(F("kesp: spi slave ready"));
}

static void enqueueBegin(const char *name, uint32_t size)
{
    Frame f;
    memset(&f, 0, sizeof(f));
    f.magic = FRAME_MAGIC;
    f.type = FT_BEGIN;
    f.seq = seqNo++;
    f.value = size;
    f.len = strlen(name);
    if (f.len > DATA_BYTES)
        f.len = DATA_BYTES;
    memcpy(f.data, name, f.len);
    enqueue(f);
}

static void enqueueData(const uint8_t *data, uint8_t len)
{
    Frame f;
    memset(&f, 0, sizeof(f));
    f.magic = FRAME_MAGIC;
    f.type = FT_DATA;
    f.seq = seqNo++;
    f.len = len;
    memcpy(f.data, data, len);
    enqueue(f);
}

static void enqueueEnd(uint32_t size)
{
    Frame f;
    memset(&f, 0, sizeof(f));
    f.magic = FRAME_MAGIC;
    f.type = FT_END;
    f.seq = seqNo++;
    f.value = size;
    enqueue(f);
}

static bool readLine(WiFiClient &c, String &line)
{
    line = "";
    uint32_t start = millis();
    while (millis() - start < 5000) {
        while (c.available()) {
            char ch = (char)c.read();
            if (ch == '\n')
                return true;
            if (ch != '\r')
                line += ch;
            if (line.length() > 96)
                return false;
        }
        delay(1);
        yield();
    }
    return false;
}

static void handleClient(WiFiClient c)
{
    String line;
    if (!readLine(c, line) || !line.startsWith("PUT ")) {
        c.print("ERR expected: PUT name size\n");
        return;
    }
    int sp = line.lastIndexOf(' ');
    if (sp <= 4) {
        c.print("ERR bad PUT header\n");
        return;
    }

    String name = line.substring(4, sp);
    uint32_t size = (uint32_t)strtoul(line.c_str() + sp + 1, nullptr, 10);
    Serial.printf("kesp: PUT %s %lu\n", name.c_str(), (unsigned long)size);
    enqueueBegin(name.c_str(), size);

    uint8_t buf[DATA_BYTES];
    uint8_t n = 0;
    uint32_t got = 0;
    while (got < size && c.connected()) {
        int b = c.read();
        if (b < 0) {
            delay(0);
            continue;
        }
        buf[n++] = (uint8_t)b;
        got++;
        rxBytes++;
        if (n == DATA_BYTES || got == size) {
            enqueueData(buf, n);
            n = 0;
        }
    }
    enqueueEnd(got);
    c.printf("OK %lu\n", (unsigned long)got);
    Serial.printf("kesp: DONE %lu/%lu\n", (unsigned long)got, (unsigned long)size);
}

static void startFallbackAp()
{
    if (apStarted)
        return;

    snprintf(apSsid, sizeof(apSsid), "KESP-%06X", ESP.getChipId() & 0xFFFFFF);
    WiFi.mode(WIFI_AP_STA);
    bool ok = WiFi.softAP(apSsid, "12345678");
    apStarted = ok;
    Serial.printf("kesp: ap fallback %s ssid=%s pass=12345678 ip=%s port=%u\n",
                  ok ? "started" : "failed",
                  apSsid,
                  WiFi.softAPIP().toString().c_str(),
                  TCP_PORT);
}

static void logWifiStatus(bool force)
{
    wl_status_t st = WiFi.status();
    uint32_t now = millis();
    if (!force && st == lastWifiStatus && now - lastWifiLogMs < 5000)
        return;

    lastWifiStatus = st;
    lastWifiLogMs = now;
    Serial.printf("kesp: wifi status=%d %s sta_ip=%s rssi=%ld ap=%d ap_ip=%s target_ssid=%s\n",
                  (int)st,
                  wifiStatusName(st),
                  WiFi.localIP().toString().c_str(),
                  (long)(st == WL_CONNECTED ? WiFi.RSSI() : 0),
                  apStarted ? 1 : 0,
                  WiFi.softAPIP().toString().c_str(),
                  WIFI_SSID);
}

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    Serial.begin(UART_BAUD);
    Serial.setDebugOutput(false);
    delay(200);
    Serial.println();
    Serial.println(F("kesp: boot"));
    Serial.printf("kesp: version=%s baud=%lu\n", KESP_VERSION, (unsigned long)UART_BAUD);

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.hostname("kesp");
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    server.begin();
    server.setNoDelay(true);
    Serial.printf("kesp: wifi begin ssid=%s port=%u fallback_ms=%lu\n",
                  WIFI_SSID, TCP_PORT, (unsigned long)AP_FALLBACK_MS);

    bootMs = millis();
    lastMs = millis();
    lastWifiLogMs = 0;
    logWifiStatus(true);
}

void loop()
{
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
        if (!spiReady) {
            Serial.printf("kesp: wifi connected ip=%s rssi=%ld port=%u\n",
                          WiFi.localIP().toString().c_str(),
                          (long)WiFi.RSSI(),
                          TCP_PORT);
        }
        spiStartOnce();
    } else if (!apStarted && millis() - bootMs >= AP_FALLBACK_MS) {
        startFallbackAp();
        spiStartOnce();
    }

    logWifiStatus(false);

    WiFiClient c = server.available();
    if (c)
        handleClient(c);

    uint32_t now = millis();
    if (now - lastMs >= 1000) {
        uint32_t d = rxBytes - lastBytes;
        lastBytes = rxBytes;
        lastMs = now;
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState ? LOW : HIGH);
        Serial.printf("kesp: alive=%lu sta_ip=%s ap_ip=%s status=%d spi=%d rx=%lu B/s\n",
                      (unsigned long)((now - bootMs) / 1000),
                      WiFi.localIP().toString().c_str(),
                      apStarted ? WiFi.softAPIP().toString().c_str() : "-",
                      WiFi.status(),
                      spiReady ? 1 : 0,
                      (unsigned long)d);
    }
    yield();
}
