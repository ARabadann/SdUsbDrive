# SdUsbDrive

Share a microSD card with a computer as a USB drive, and take it back
from your sketch.

Plug the board in and the card shows up as a removable drive. When the
sketch needs the card (to log data, save a recording, store a photo) it
takes it back, writes its files and lends the card again. The computer
then shows the new files without unplugging the cable.

For the Raspberry Pi RP2040 / RP2350 Arduino core
([arduino-pico](https://github.com/earlephilhower/arduino-pico)).

## Features

- **One owner at a time.** Either the sketch or the computer uses the
  card, so the file system is never corrupted by both writing at once.
- **Safe hand-off.** `takeBack()` refuses while the computer is in the
  middle of writing, unless you force it.
- **Media change.** The computer is told the card changed, so it reads
  the file system again. Tested on Windows 11; macOS, Linux and
  Android are not tested yet (reports welcome).
- **SDIO and SPI cards.** 4-bit SDIO on boards that have it (UNIT Pulsar
  RP2350A), SPI on everything else.
- **FAT16, FAT32 and exFAT**, through the bundled SdFat library.
- **Full-speed transfers.** Read-ahead cache and buffered writes, so
  the card is not read again for every 64-byte USB packet.
- **Read-only mode.** The computer can copy files but not change them.
- Works together with USB Serial on the same cable.

## Quick start

```cpp
#include <SdUsbDrive.h>

SdUsbDrive drive;

void setup() {
    drive.begin(SdUsbDrive::sdio(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_DAT0));
    // or: drive.begin(SdUsbDrive::spi(17));
    drive.shareWithHost();
}

void loop() {
    if (timeToLog() && drive.takeBack()) {
        FsFile f = drive.fs().open("LOG.CSV", O_WRONLY | O_CREAT | O_APPEND);
        f.println(readSensor());
        f.close();                 // close files before sharing again
        drive.shareWithHost();
    }
}
```

## API

| Function | Description |
|---|---|
| `begin(config)` | Starts the card and adds the USB drive. The sketch owns the card afterwards. Call it early in `setup()`. Call again to retry or after swapping cards. |
| `SdUsbDrive::sdio(clk, cmd, dat0)` | SDIO card configuration (DAT1..DAT3 on the next pins). |
| `SdUsbDrive::spi(cs, port = SPI, maxSck = SD_SCK_MHZ(25))` | SPI card configuration. |
| `shareWithHost(readOnly = false)` | Lends the card to the computer. Close every file first. |
| `takeBack(force = false)` | Takes the card back and mounts the file system. Refuses if the computer read or wrote during the quiet time (2 s), so copies are never cut. Returns true when `fs()` is ready. |
| `fs()` | The `SdFs` file system (`FsFile`, `open`, `exists`, `remove`, `mkdir`…). Use it only while the sketch owns the card. |
| `owner()` | `Owner::Sketch`, `Owner::Host` or `Owner::None`. |
| `cardPresent()` | A card started and had no I/O error since. |
| `hostMounted()` | The computer has the drive open. |
| `hostActive()` | The computer read or wrote in the last 250 ms (activity LED). |
| `hostEjected()` | The computer used Eject / Safely Remove. |
| `onEject(callback)` | Called from the USB interrupt on eject. Keep it short. |
| `msSinceHostWrite()` / `msSinceHostAccess()` | Time since the computer last wrote / read or wrote. |
| `bytesRead()` / `bytesWritten()` | Transfer counters. |
| `setIdentity(vendor, product, rev)` | Name shown by the computer (8 / 16 / 4 chars). |
| `setQuietTime(ms)` | Quiet time used by `takeBack()`. |
| `end()` | Removes the drive and releases the card. |

## Good to know

- **Use `fs()`, not `SD.h`.** The library owns the card through SdFat.
  Do not start `SD`, `SDFS` or `FatFSUSB` in the same sketch.
- **Close files before `shareWithHost()`.** Open files would point to
  data the computer may change.
- **Eject first for long writes.** Windows writes removable drives right
  away. macOS and Linux may keep writes in memory for several seconds:
  eject the drive, or raise `setQuietTime()`, before the sketch takes
  the card back.
- **Take the card only when needed.** While the sketch owns the card
  the computer sees no media: open Explorer windows on the drive close
  and files played from it stop. Keep data in RAM and write it on a
  user action (a button, a command) or in rare batches, not every few
  seconds.
- **Eject as "go ahead".** When the user ejects the drive the card is
  free and `takeBack()` succeeds at once. The next `shareWithHost()`
  reconnects USB (about 1 s) so the drive shows up again; USB Serial
  reconnects with it.
- **Speed.** RP2040/RP2350 USB is Full Speed (12 Mbit/s), about
  0.8-1 MB/s in practice.
- **SPI bus.** The computer reaches the card from the USB interrupt, so
  keep the card's SPI bus for the card while the computer owns it.
- **RAM.** 16 KB for the caches. Define `SDUSBDRIVE_CACHE_SECTORS`
  before including the library to change it (sectors of 512 bytes).

## Examples

- **CardReader** - the board becomes a USB card reader with an activity
  LED and speed report.
- **DataLogger** - the drive stays visible; readings are kept in RAM
  and saved to LOG.CSV when you eject the drive or type `s` in the
  Serial Monitor.
- **CardReaderSPI** - card reader with a microSD module on SPI.

## License

MIT. Copyright (c) 2026 Adrián Rabadán Ortiz.
