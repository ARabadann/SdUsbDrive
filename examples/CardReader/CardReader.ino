/*
    CardReader - the board becomes a USB microSD card reader

    Plug the board into a computer: the microSD card shows up as a
    removable drive. Copy, delete and rename files as usual.

    Boards with SDIO pins (PIN_SD_CLK / PIN_SD_CMD / PIN_SD_DAT0, like
    the UNIT Pulsar RP2350A) use fast 4-bit SDIO. Other boards use SPI:
    set SD_CS_PIN below.

    LED_BUILTIN:
      fast blink   no card (it retries every 2 s, so you can insert one)
      short blink  drive ready, idle
      on           the computer is reading or writing

    Serial monitor (115200) prints the transfer speed.
*/

#include <SdUsbDrive.h>

const uint8_t SD_CS_PIN = 17;   // only used on boards without SDIO pins

SdUsbDrive drive;

SdUsbDrive::Config cardConfig() {
#if defined(PIN_SD_CLK) && defined(PIN_SD_CMD) && defined(PIN_SD_DAT0)
    return SdUsbDrive::sdio(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_DAT0);
#else
    return SdUsbDrive::spi(SD_CS_PIN);
#endif
}

bool startDrive() {
    return drive.begin(cardConfig()) && drive.shareWithHost();
}

void updateLed() {

    const uint32_t t = millis();
    bool on;

    if (!drive.cardPresent()) {
        on = (t % 200) < 100;
    } else if (drive.hostActive()) {
        on = true;
    } else {
        on = (t % 1000) < 50;
    }

    digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
}

void report() {

    static uint32_t lastMs = 0;
    static uint64_t lastRead = 0;
    static uint64_t lastWritten = 0;

    const uint32_t now = millis();
    if (now - lastMs < 2000) {
        return;
    }

    const float seconds = (now - lastMs) / 1000.0f;
    const uint64_t r = drive.bytesRead();
    const uint64_t w = drive.bytesWritten();

    Serial.printf("card %s | host %s | read %.0f KB/s | write %.0f KB/s\n",
                  drive.cardPresent() ? "ok" : "missing",
                  drive.hostEjected() ? "ejected" :
                  drive.hostMounted() ? "mounted" : "-",
                  (r - lastRead) / 1024.0f / seconds,
                  (w - lastWritten) / 1024.0f / seconds);

    lastMs = now;
    lastRead = r;
    lastWritten = w;
}

void setup() {

    pinMode(LED_BUILTIN, OUTPUT);

    // Start the drive before anything else: adding it reconnects USB
    startDrive();

    Serial.begin(115200);
}

void loop() {

    static uint32_t lastRetry = 0;

    if (!drive.cardPresent() && millis() - lastRetry > 2000) {
        lastRetry = millis();
        if (startDrive()) {
            Serial.println("Card found");
        }
    }

    updateLed();
    report();
}
