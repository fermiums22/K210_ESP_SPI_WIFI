#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SPISlave.h>

#ifndef KESP_VERSION
#define KESP_VERSION "dev"
#endif

static const char *WIFI_SSID = "Fermiums";
static const char *WIFI_PASS = "876543212";
static const uint16_t TCP_PORT = 7777;
static const uint32_t FRAME_MAGIC = 0x5053454bUL;
static const uint32_t UART_BAUD = 115200;
static const size_t DATA_BYTES = 20;
static const size_t QUEUE_SIZE = 64;

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
static bool ledState;

static void logLine(const __FlashStringHelper *msg)
{
    Serial.print(F("kesp: "));
    Serial.println(msg);
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
    if (!readLine(c, line) || !line.startsWith("PUT "))
        return;
    int sp = line.lastIndexOf(' ');
    if (sp <= 4)
        return;

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
    Serial.println(F("kesp: wifi begin"));

    bootMs = millis();
    lastMs = millis();
}

void loop()
{
    if (WiFi.status() == WL_CONNECTED)
        spiStartOnce();

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
        Serial.printf("kesp: alive=%lu ip=%s status=%d spi=%d rx=%lu B/s\n",
                      (unsigned long)((now - bootMs) / 1000),
                      WiFi.localIP().toString().c_str(),
                      WiFi.status(),
                      spiReady ? 1 : 0,
                      (unsigned long)d);
    }
    yield();
}
