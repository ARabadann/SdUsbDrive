/*
    DataLogger - log to the card while the computer can see it

    Every 10 seconds the sketch takes the card back, appends a line to
    LOG.CSV and lends the card to the computer again. The computer sees
    the file grow without unplugging the board.

    takeBack() refuses while the computer is writing (for example in
    the middle of a copy), so the sketch simply tries again a second
    later. Nothing is cut in half.

    The computer briefly sees "no media" while the sketch writes. Open
    LOG.CSV again after it changes to see the new lines.

    Boards with SDIO pins (like the UNIT Pulsar RP2350A) use SDIO; other
    boards use SPI: set SD_CS_PIN below.
*/

#include <SdUsbDrive.h>

const uint8_t SD_CS_PIN = 17;          // only used without SDIO pins
const uint32_t LOG_EVERY_MS = 10000;
const uint32_t RETRY_MS = 1000;

SdUsbDrive drive;

SdUsbDrive::Config cardConfig() {
#if defined(PIN_SD_CLK) && defined(PIN_SD_CMD) && defined(PIN_SD_DAT0)
    return SdUsbDrive::sdio(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_DAT0);
#else
    return SdUsbDrive::spi(SD_CS_PIN);
#endif
}

uint32_t nextLogMs = 0;
uint32_t lineNumber = 0;

bool appendLine() {

    // The sketch owns the card here: use fs() like any SdFat volume
    FsFile f = drive.fs().open("LOG.CSV", O_WRONLY | O_CREAT | O_APPEND);
    if (!f) {
        return false;
    }

    if (f.size() == 0) {
        f.println("line,uptime_s");
    }

    f.printf("%lu,%lu\n", (unsigned long) ++lineNumber,
             (unsigned long) (millis() / 1000));
    f.close();   // always close before shareWithHost()
    return true;
}

void setup() {

    drive.setIdentity("SdUsb", "Data Logger");
    const bool cardOk = drive.begin(cardConfig());

    Serial.begin(115200);

    if (!cardOk) {
        Serial.println("No card. Insert one and reset the board.");
        return;
    }

    appendLine();
    drive.shareWithHost();
    nextLogMs = millis() + LOG_EVERY_MS;
}

void loop() {

    if (!drive.cardPresent() || (int32_t) (millis() - nextLogMs) < 0) {
        return;
    }

    if (!drive.takeBack()) {
        Serial.println("Computer is writing, trying again in 1 s");
        nextLogMs = millis() + RETRY_MS;
        return;
    }

    Serial.println(appendLine() ? "Logged a line" : "Could not open LOG.CSV");

    drive.shareWithHost();
    nextLogMs = millis() + LOG_EVERY_MS;
}
