/*
    DataLogger - the computer always sees the drive; the board writes
    only when you ask

    The board measures its own chip temperature every second and keeps
    the readings in RAM. The microSD card stays visible on the computer
    the whole time. The readings are saved to LOG.CSV when:

      1. you EJECT the drive on the computer ("Eject" / "Safely
         remove"): that tells the board the card is free. It saves at
         once and the drive shows up again a moment later with the new
         lines (USB Serial reconnects too), or
      2. you type  s  in the Serial Monitor (115200): the board waits
         until the computer is not copying anything, then saves. The
         drive disappears for 1-2 s and comes back, or
      3. the RAM buffer gets full (10 minutes), the same way as 2.

    A copy in progress is never cut: the board waits for it to finish.

    Boards with SDIO pins (like the UNIT Pulsar RP2350A) use SDIO; other
    boards use SPI: set SD_CS_PIN below.
*/

#include <SdUsbDrive.h>

const uint8_t SD_CS_PIN = 17;           // only used without SDIO pins
const uint32_t SAMPLE_EVERY_MS = 1000;
const size_t MAX_SAMPLES = 600;         // 10 minutes in RAM

SdUsbDrive drive;

SdUsbDrive::Config cardConfig() {
#if defined(PIN_SD_CLK) && defined(PIN_SD_CMD) && defined(PIN_SD_DAT0)
    return SdUsbDrive::sdio(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_DAT0);
#else
    return SdUsbDrive::spi(SD_CS_PIN);
#endif
}

struct Sample {
    uint32_t seconds;
    float celsius;
};

Sample samples[MAX_SAMPLES];
size_t sampleCount = 0;
uint32_t nextSampleMs = 0;
bool saveRequested = false;
bool waitingMessageShown = false;

void takeSample() {

    if ((int32_t) (millis() - nextSampleMs) < 0) {
        return;
    }

    nextSampleMs += SAMPLE_EVERY_MS;

    if (sampleCount < MAX_SAMPLES) {
        samples[sampleCount++] = { millis() / 1000, analogReadTemp() };
    }

    if (sampleCount >= MAX_SAMPLES - 10 && !saveRequested) {
        Serial.println("RAM almost full, saving");
        saveRequested = true;
    }
}

// The sketch owns the card here: use fs() like any SdFat volume
bool writeSamples() {

    FsFile f = drive.fs().open("LOG.CSV", O_WRONLY | O_CREAT | O_APPEND);
    if (!f) {
        return false;
    }

    if (f.size() == 0) {
        f.println("uptime_s,chip_temp_c");
    }

    for (size_t i = 0; i < sampleCount; i++) {
        f.printf("%lu,%.1f\n", (unsigned long) samples[i].seconds,
                 samples[i].celsius);
    }

    f.close();   // always close before shareWithHost()
    return true;
}

void trySave() {

    // Ejected: the card is free, takeBack() succeeds at once.
    // Otherwise it refuses while the computer is copying.
    if (!drive.takeBack()) {
        if (!waitingMessageShown) {
            Serial.println("Computer is using the drive, waiting...");
            waitingMessageShown = true;
        }
        return;
    }

    const size_t saved = sampleCount;
    const bool ok = writeSamples();

    if (ok) {
        sampleCount = 0;
    }

    drive.shareWithHost();
    saveRequested = false;
    waitingMessageShown = false;

    Serial.printf(ok ? "Saved %u readings to LOG.CSV\n"
                     : "Could not open LOG.CSV (%u readings kept)\n",
                  (unsigned) saved);
}

void readCommands() {

    while (Serial.available()) {
        const char c = Serial.read();
        if (c == 's' || c == 'S') {
            Serial.println("Save requested");
            saveRequested = true;
        }
    }
}

void setup() {

    drive.setIdentity("SdUsb", "Data Logger");
    const bool cardOk = drive.begin(cardConfig());
    drive.shareWithHost();

    Serial.begin(115200);
    nextSampleMs = millis();

    if (!cardOk) {
        Serial.println("No card. Insert one and reset the board.");
    }
}

void loop() {

    takeSample();
    readCommands();

    if (!drive.cardPresent()) {
        return;
    }

    // Ejecting the drive on the computer counts as "save now"
    if (drive.hostEjected() || saveRequested) {
        trySave();
    }

    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 10000) {
        lastStatus = millis();
        Serial.printf("%u readings in RAM. Type s or eject the drive "
                      "to save.\n", (unsigned) sampleCount);
    }
}
