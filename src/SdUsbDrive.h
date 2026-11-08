/*
    SdUsbDrive - Share a microSD card with a computer as a USB drive
    Copyright (c) 2026 ARabadann

    Released under the MIT License. See LICENSE for details.
*/

#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>

// Sectors held by the read-ahead cache and by the write buffer.
// Each sector is 512 bytes, so the default uses 2 x 8 KB of RAM.
#ifndef SDUSBDRIVE_CACHE_SECTORS
#define SDUSBDRIVE_CACHE_SECTORS 16
#endif

// The microSD card has one owner at a time: either the sketch or the
// computer (host). The library lends the card to the host and takes it
// back safely, like a camera does.
class SdUsbDrive {

public:

    enum class Owner : uint8_t {
        None,     // no card, or begin() failed
        Sketch,   // the sketch uses fs(); the host sees "no media"
        Host      // the host sees the drive; the sketch must not use fs()
    };

    struct Config {
        bool useSdio;
        uint8_t clkPin;
        uint8_t cmdPin;
        uint8_t dat0Pin;
        uint8_t csPin;
        SpiPort_t* spiPort;
        uint32_t maxSck;
    };

    SdUsbDrive();

    // 4-bit SDIO card (DAT1..DAT3 must follow DAT0 on consecutive pins)
    static Config sdio(uint8_t clkPin, uint8_t cmdPin, uint8_t dat0Pin);

    // SPI card. The SPI bus should only be used by the card while the
    // host owns it, because the host accesses it from the USB interrupt.
    static Config spi(uint8_t csPin, SpiPort_t& port = SPI,
                      uint32_t maxSck = SD_SCK_MHZ(25));

    // Starts the card and adds the USB drive. The sketch owns the card
    // afterwards. Call it early in setup(): adding the drive makes the
    // USB device reconnect once. Returns false if the card failed; the
    // host then sees an empty card reader. Call begin() again to retry.
    bool begin(const Config& config);

    // Removes the USB drive and releases the card.
    void end();

    // Strings shown by the host (8, 16 and 4 characters max).
    void setIdentity(const char* vendor, const char* product,
                     const char* revision = "1.0");

    // How long the host must stop writing before takeBack() succeeds.
    void setQuietTime(uint32_t ms);

    // Lends the card to the host. Close every file first. With
    // readOnly the host can copy files but not change them. If the
    // host had the card before, it first has to notice the card was
    // gone, so the drive reappears 1-2 s later and the host reads the
    // file system again instead of trusting its old copy. If the host
    // ejected the drive, USB reconnects (about 1 s) so the drive shows
    // up again; USB Serial reconnects with it.
    bool shareWithHost(bool readOnly = false);

    // Takes the card back for the sketch and mounts the file system.
    // Refuses while the host read or wrote during the last quiet time
    // (2 s by default), so a copy in either direction is not cut in
    // half, unless force is true. Status polls do not count as use.
    // Returns true when fs() is ready to use.
    bool takeBack(bool force = false);

    // SdFat file system (FAT16/FAT32/exFAT). Use it only while
    // owner() == Owner::Sketch and fsMounted() is true.
    SdFs& fs() { return _sd; }
    bool fsMounted() const { return _fsMounted; }

    Owner owner() const { return _owner; }
    bool cardPresent() const { return _owner != Owner::None && !_ioError; }
    bool readOnly() const { return _readOnly; }
    uint32_t sectorCount() const { return _sectors; }

    // The host has the drive open (connected, polling and not ejected).
    bool hostMounted() const;

    // The host read or wrote during the last 250 ms.
    bool hostActive() const;

    // The host used "Eject" / "Safely remove": the card is free and
    // takeBack() succeeds at once. Cleared by shareWithHost().
    bool hostEjected() const { return _ejected; }

    // Milliseconds since the host last wrote (UINT32_MAX if never).
    uint32_t msSinceHostWrite() const;

    // Milliseconds since the host last read or wrote (UINT32_MAX if
    // never since the card was shared).
    uint32_t msSinceHostAccess() const;

    uint64_t bytesRead() const { return uint64_t(_sectorsRead) * 512; }
    uint64_t bytesWritten() const { return uint64_t(_sectorsWritten) * 512; }

    // Called from the USB interrupt when the host ejects the drive.
    // Keep it short: set a flag and act on it in loop().
    void onEject(void (*callback)()) { _onEject = callback; }

    // ---- Internal: called by the TinyUSB mass storage callbacks ----
    void _inquiry(uint8_t vendor[8], uint8_t product[16], uint8_t rev[4]);
    bool _testUnitReady(uint8_t lun);
    void _capacity(uint32_t* count, uint16_t* size);
    int32_t _read10(uint32_t lba, uint32_t offset, void* buf, uint32_t len);
    int32_t _write10(uint32_t lba, uint32_t offset, const uint8_t* buf,
                     uint32_t len);
    void _writeComplete();
    bool _writable() const { return !_readOnly; }
    void _startStop(bool start, bool loadEject);
    void _preventRemoval(bool prevent) { _hostLocked = prevent; }
    bool _syncCache();

private:

    static constexpr uint32_t SECTOR = 512;
    static constexpr uint32_t CACHE = SDUSBDRIVE_CACHE_SECTORS;

    bool hostAllowed() const;
    bool flushWrites();
    bool startCard(const Config& config);
    void addUsbInterface();
    static void lock();
    static void unlock();

    SdFs _sd;
    Config _config = {};

    volatile Owner _owner = Owner::None;
    volatile bool _readOnly = false;
    volatile bool _ejected = false;
    volatile bool _mediaChanged = false;
    volatile bool _ioError = false;
    volatile bool _hostLocked = false;

    // After the sketch used the card, the host must notice it was gone
    // (answer "no media" at least once, for at least MIN_REMOVAL_MS)
    // before the card comes back. Otherwise a host that polled too late
    // keeps its old copy of the file system and can overwrite the
    // sketch's changes.
    static constexpr uint32_t MIN_REMOVAL_MS = 1000;
    volatile bool _hostHadMedia = false;
    volatile bool _presentPending = false;
    volatile bool _hostSawRemoval = false;
    volatile uint32_t _removedAtMs = 0;
    bool _fsMounted = false;
    bool _usbAdded = false;

    uint32_t _sectors = 0;
    uint32_t _quietMs = 2000;

    volatile uint32_t _lastCmdMs = 0;
    volatile uint32_t _lastIoMs = 0;
    volatile uint32_t _lastWriteMs = 0;
    volatile bool _hostWrote = false;
    volatile bool _hostAccessed = false;
    volatile uint32_t _sectorsRead = 0;
    volatile uint32_t _sectorsWritten = 0;

    // Read-ahead cache
    alignas(4) uint8_t _readBuf[CACHE * SECTOR];
    uint32_t _readStart = 0;
    uint32_t _readCount = 0;

    // Write buffer: consecutive sectors written by one host command
    alignas(4) uint8_t _writeBuf[CACHE * SECTOR];
    uint32_t _writeStart = 0;
    uint32_t _writeCount = 0;   // complete sectors buffered

    char _vendor[8];
    char _product[16];
    char _revision[4];

    void (*_onEject)() = nullptr;

    uint8_t _usbId = 0;
    uint8_t _epIn = 0;
    uint8_t _epOut = 0;
};
