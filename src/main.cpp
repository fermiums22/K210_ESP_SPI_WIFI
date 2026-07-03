#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <SPISlave.h>

#ifndef KESP_VERSION
#define KESP_VERSION "spi-pattern-test"
#endif

static const uint32_t UART_BAUD = 115200;
static const size_t SPI_FRAME_BYTES = 32;

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

static uint8_t spiTx[SPI_FRAME_BYTES];
static volatile uint32_t spiRxCount;
static volatile uint32_t spiSentCount;
static volatile uint32_t spiSeq;
static volatile size_t lastRxLen;
static volatile bool rxPreviewReady;
static uint8_t rxPreview[8];
static uint32_t lastLogMs;
static bool ledState;

static void makePattern(uint32_t seq)
{
    memset(spiTx, 0, sizeof(spiTx));

    /* Little-endian 0x5053454b == bytes "KESP".
     * Keep the magic at offset 0 for the clean case; the K210 scanner will also
     * search all offsets to catch dummy/command/alignment shifts. */
    spiTx[0] = 'K';
    spiTx[1] = 'E';
    spiTx[2] = 'S';
    spiTx[3] = 'P';
    spiTx[4] = (uint8_t)(seq >> 0);
    spiTx[5] = (uint8_t)(seq >> 8);
    spiTx[6] = (uint8_t)(seq >> 16);
    spiTx[7] = (uint8_t)(seq >> 24);
    spiTx[8] = 0xA5;
    spiTx[9] = 0x5A;
    spiTx[10] = 0xC3;
    spiTx[11] = 0x3C;

    for (size_t i = 12; i < sizeof(spiTx); i++) {
        spiTx[i] = (uint8_t)(0x30u + ((seq + i) & 0x3fu));
    }
}

static void loadNextPattern()
{
    uint32_t seq = spiSeq++;
    makePattern(seq);
    SPISlave.setData(spiTx, sizeof(spiTx));
}

static void onSpiData(uint8_t *data, size_t len)
{
    spiRxCount++;
    lastRxLen = len;

    if (!rxPreviewReady) {
        size_t n = len < sizeof(rxPreview) ? len : sizeof(rxPreview);
        for (size_t i = 0; i < n; i++) {
            rxPreview[i] = data[i];
        }
        for (size_t i = n; i < sizeof(rxPreview); i++) {
            rxPreview[i] = 0;
        }
        rxPreviewReady = true;
    }
}

static void onSpiDataSent()
{
    spiSentCount++;
    loadNextPattern();
}

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    Serial.begin(UART_BAUD);
    Serial.setDebugOutput(false);
    delay(200);
    Serial.println();
    Serial.printf("kesp-spi-test: boot version=%s baud=%lu\n", KESP_VERSION, (unsigned long)UART_BAUD);
    Serial.println(F("kesp-spi-test: no WiFi, no TCP, no SD; ESP8266 Arduino SPISlave only"));
    Serial.println(F("kesp-spi-test: pins GPIO15=CS GPIO14=CLK GPIO12=MISO GPIO13=MOSI"));
    Serial.println(F("kesp-spi-test: TX frame is exactly 32 bytes and starts with bytes 4b 45 53 50"));

    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
    delay(1);

    SPISlave.onData(onSpiData);
    SPISlave.onDataSent(onSpiDataSent);

    loadNextPattern();
    SPISlave.begin();

    Serial.println(F("kesp: spi slave ready"));
    Serial.println(F("kesp-spi-test: ready"));
    lastLogMs = millis();
}

void loop()
{
    uint32_t now = millis();
    if (now - lastLogMs >= 1000) {
        lastLogMs = now;
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState ? LOW : HIGH);

        Serial.printf(
            "kesp-spi-test: alive=%lu rx=%lu sent=%lu next_seq=%lu last_rx_len=%u",
            (unsigned long)(now / 1000u),
            (unsigned long)spiRxCount,
            (unsigned long)spiSentCount,
            (unsigned long)spiSeq,
            (unsigned)lastRxLen);

        if (rxPreviewReady) {
            Serial.printf(" rx0=%02x %02x %02x %02x %02x %02x %02x %02x",
                          rxPreview[0], rxPreview[1], rxPreview[2], rxPreview[3],
                          rxPreview[4], rxPreview[5], rxPreview[6], rxPreview[7]);
            rxPreviewReady = false;
        }
        Serial.println();
    }
    yield();
}
