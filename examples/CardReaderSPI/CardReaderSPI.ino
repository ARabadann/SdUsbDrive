/*
    CardReaderSPI - USB card reader with a microSD module on SPI

    For boards without SDIO, or a microSD breakout wired to SPI.
    Default wiring (Raspberry Pi Pico, SPI0):

      card    board
      ----    -----
      CS      GP17
      SCK     GP18
      MOSI    GP19  (card DI)
      MISO    GP16  (card DO)
      VCC     3V3
      GND     GND

    Keep this SPI bus for the card only while the computer uses the
    drive: the computer reaches the card from the USB interrupt.
*/

#include <SdUsbDrive.h>

const uint8_t PIN_CS = 17;
const uint8_t PIN_SCK = 18;
const uint8_t PIN_MOSI = 19;
const uint8_t PIN_MISO = 16;

SdUsbDrive drive;

void setup() {

    pinMode(LED_BUILTIN, OUTPUT);

    SPI.setRX(PIN_MISO);
    SPI.setSCK(PIN_SCK);
    SPI.setTX(PIN_MOSI);

    // 25 MHz works for most cards; lower it if the wires are long
    if (drive.begin(SdUsbDrive::spi(PIN_CS, SPI, SD_SCK_MHZ(25)))) {
        drive.shareWithHost();
    }

    Serial.begin(115200);
}

void loop() {

    digitalWrite(LED_BUILTIN, drive.hostActive() ? HIGH : LOW);

    static uint32_t last = 0;
    if (millis() - last > 2000) {
        last = millis();
        Serial.printf("card %s, %lu MB\n",
                      drive.cardPresent() ? "ok" : "missing",
                      (unsigned long) (drive.sectorCount() / 2048));
    }
}
