#include <Arduino.h>
#include <SPISlave.h>

#ifndef KESP_VERSION
#define KESP_VERSION "spi-uart-test"
#endif

static const uint32_t FRAME_MAGIC = 0x5053454bUL;
static const uint32_t UART_BAUD = 115200;
static const size_t DATA_BYTES = 20;

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

enum { FT_INFO = 4 };

struct __attribute__((packed)) Frame {
    uint32_t magic;
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    uint32_t value;
    uint8_t data[DATA_BYTES];
};

static Frame spiTxFrame;
static volatile uint32_t frameCounter;
static uint32_t lastFrameCounter;
static uint32_t lastLogMs;
static bool ledState;

static void fillPattern(Frame &f, uint32_t counter)
{
    memset(&f, 0, sizeof(f));
    f.magic = FRAME_MAGIC;
    f.type = FT_INFO;
    f.seq = (uint8_t)(counter & 0xffu);
    f.len = DATA_BYTES;
    f.value = counter;
    for (uint8_t i = 0; i < DATA_BYTES; i++)
        f.data[i] = (uint8_t)((counter + i) & 0xffu);
}

static void spiLoadNext()
{
    uint32_t counter = frameCounter++;
    fillPattern(spiTxFrame, counter);
    SPISlave.setData((uint8_t *)&spiTxFrame, sizeof(spiTxFrame));
}

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.begin(UART_BAUD);
    Serial.setDebugOutput(false);
    delay(200);
    Serial.println();
    Serial.println(F("kesp-spi-test: boot"));
    Serial.printf("kesp-spi-test: version=%s baud=%lu frame=%u magic=0x%08lx\n",
                  KESP_VERSION, (unsigned long)UART_BAUD,
                  (unsigned)sizeof(Frame), (unsigned long)FRAME_MAGIC);
    Serial.println(F("kesp-spi-test: UART log + SPI slave pattern only"));

    SPISlave.onDataSent([]() {
        spiLoadNext();
    });
    SPISlave.begin();
    spiLoadNext();
    Serial.println(F("kesp: spi slave ready"));
    Serial.println(F("kesp-spi-test: spi slave pattern ready"));
    lastLogMs = millis();
}

void loop()
{
    uint32_t now = millis();
    if (now - lastLogMs >= 1000) {
        uint32_t frames = frameCounter;
        uint32_t d = frames - lastFrameCounter;
        lastFrameCounter = frames;
        lastLogMs = now;
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState ? LOW : HIGH);
        Serial.printf("kesp-spi-test: alive=%lu frames=%lu fps=%lu next_seq=%u\n",
                      (unsigned long)(now / 1000UL),
                      (unsigned long)frames,
                      (unsigned long)d,
                      (unsigned)(frames & 0xffu));
    }
    yield();
}
