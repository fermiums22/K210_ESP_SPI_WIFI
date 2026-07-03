#include <Arduino.h>

#ifndef KESP_VERSION
#define KESP_VERSION "gpio-link-test"
#endif

static const uint32_t UART_BAUD = 115200;
static const uint8_t PIN_CS_IN = 15;
static const uint8_t PIN_CLK_IN = 14;
static const uint8_t PIN_MOSI_IN = 13;
static const uint8_t PIN_MISO_OUT = 12;

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

static uint32_t lastLogMs;
static uint32_t lastDriveMs;
static uint32_t stepNo;
static int lastCs = -1;
static int lastClk = -1;
static int lastMosi = -1;
static uint32_t csChanges;
static uint32_t clkChanges;
static uint32_t mosiChanges;
static bool resultLogged;
static bool ledState;

static void noteInput(int v, int &last, uint32_t &changes)
{
    if (last < 0) {
        last = v;
    } else if (last != v) {
        changes++;
        last = v;
    }
}

static void logVerdict()
{
    if (resultLogged || millis() < 8000)
        return;
    resultLogged = true;
    if (csChanges && clkChanges && mosiChanges) {
        Serial.printf("kesp-gpio-test: RESULT ESP_SEES_K210_OK cs_chg=%lu clk_chg=%lu mosi_chg=%lu\n",
                      (unsigned long)csChanges, (unsigned long)clkChanges, (unsigned long)mosiChanges);
    } else {
        Serial.printf("kesp-gpio-test: RESULT ESP_INPUT_FAIL cs_chg=%lu clk_chg=%lu mosi_chg=%lu\n",
                      (unsigned long)csChanges, (unsigned long)clkChanges, (unsigned long)mosiChanges);
    }
}

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    Serial.begin(UART_BAUD);
    Serial.setDebugOutput(false);
    delay(200);
    Serial.println();
    Serial.printf("kesp-gpio-test: boot version=%s baud=%lu\n", KESP_VERSION, (unsigned long)UART_BAUD);
    Serial.println(F("kesp-gpio-test: no WiFi, no SPI peripheral, GPIO link test only"));
    Serial.println(F("kesp-gpio-test: ESP drives GPIO12/MISO, reads GPIO15/CS GPIO14/CLK GPIO13/MOSI"));

    pinMode(PIN_CS_IN, INPUT);
    pinMode(PIN_CLK_IN, INPUT);
    pinMode(PIN_MOSI_IN, INPUT);
    pinMode(PIN_MISO_OUT, OUTPUT);
    digitalWrite(PIN_MISO_OUT, LOW);

    Serial.println(F("kesp: spi slave ready"));
    Serial.println(F("kesp-gpio-test: gpio ready"));
    lastLogMs = millis();
    lastDriveMs = millis();
}

void loop()
{
    uint32_t now = millis();
    if (now - lastDriveMs >= 250) {
        lastDriveMs = now;
        stepNo++;
        digitalWrite(PIN_MISO_OUT, (stepNo & 1) ? HIGH : LOW);
        ledState = !ledState;
        digitalWrite(LED_BUILTIN, ledState ? LOW : HIGH);
    }

    int cs = digitalRead(PIN_CS_IN);
    int clk = digitalRead(PIN_CLK_IN);
    int mosi = digitalRead(PIN_MOSI_IN);
    noteInput(cs, lastCs, csChanges);
    noteInput(clk, lastClk, clkChanges);
    noteInput(mosi, lastMosi, mosiChanges);

    if (now - lastLogMs >= 1000) {
        lastLogMs = now;
        Serial.printf("kesp-gpio-test: drive_miso=%u read cs=%d clk=%d mosi=%d chg=%lu/%lu/%lu\n",
                      (unsigned)(stepNo & 1), cs, clk, mosi,
                      (unsigned long)csChanges, (unsigned long)clkChanges, (unsigned long)mosiChanges);
        logVerdict();
    }
    yield();
}
