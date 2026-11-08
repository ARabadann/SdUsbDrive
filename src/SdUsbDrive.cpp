/*
    SdUsbDrive - Share a microSD card with a computer as a USB drive
    Copyright (c) 2026 ARabadann

    Released under the MIT License. See LICENSE for details.

    How it works
    ------------
    TinyUSB runs the mass storage class from the USB task interrupt
    (every 1 ms, under USB.mutex) and hands the data over in 64-byte
    pieces. The library therefore:

      - reads several sectors at once and serves the pieces from a
        read-ahead cache,
      - collects consecutive written sectors and writes them to the
        card together when the host command finishes,
      - changes the card owner while holding USB.mutex, so the USB
        interrupt never sees a half-made change,
      - tells the host "no media" while the sketch owns the card and
        "media changed" when it gets it back, so the host reads the
        file system again instead of trusting its old copy.
*/

#include "SdUsbDrive.h"

#include <USB.h>
#include <tusb-msc.h>
#include <class/msc/msc.h>
#include <device/usbd.h>

#ifdef __FREERTOS
#include <_freertos.h>
#endif

// SCSI sense keys and codes
static constexpr uint8_t SENSE_NOT_READY = 0x02;
static constexpr uint8_t SENSE_UNIT_ATTENTION = 0x06;
static constexpr uint8_t SENSE_ILLEGAL_REQUEST = 0x05;
static constexpr uint8_t ASC_MEDIUM_NOT_PRESENT = 0x3A;
static constexpr uint8_t ASC_MEDIUM_CHANGED = 0x28;
static constexpr uint8_t ASC_INVALID_COMMAND = 0x20;
static constexpr uint8_t SCSI_SYNCHRONIZE_CACHE_10 = 0x35;

// TinyUSB callbacks are plain C functions, so they reach the drive
// through this pointer. Only one SdUsbDrive can be active.
static SdUsbDrive* activeDrive = nullptr;

static void copyPadded(char* dst, size_t size, const char* src) {
    memset(dst, ' ', size);
    if (src) {
        const size_t n = strnlen(src, size);
        memcpy(dst, src, n);
    }
}

SdUsbDrive::SdUsbDrive() {
    setIdentity("SdUsb", "microSD Drive", "1.0");
}

SdUsbDrive::Config SdUsbDrive::sdio(uint8_t clkPin, uint8_t cmdPin,
                                    uint8_t dat0Pin) {
    Config c = {};
    c.useSdio = true;
    c.clkPin = clkPin;
    c.cmdPin = cmdPin;
    c.dat0Pin = dat0Pin;
    return c;
}

SdUsbDrive::Config SdUsbDrive::spi(uint8_t csPin, SpiPort_t& port,
                                   uint32_t maxSck) {
    Config c = {};
    c.useSdio = false;
    c.csPin = csPin;
    c.spiPort = &port;
    c.maxSck = maxSck;
    return c;
}

void SdUsbDrive::lock() {
#ifdef __FREERTOS
    xSemaphoreTake(__get_freertos_mutex_for_ptr(&USB.mutex), portMAX_DELAY);
#else
    mutex_enter_blocking(&USB.mutex);
#endif
}

void SdUsbDrive::unlock() {
#ifdef __FREERTOS
    xSemaphoreGive(__get_freertos_mutex_for_ptr(&USB.mutex));
#else
    mutex_exit(&USB.mutex);
#endif
}

// ------------------------------------------------------------------
// Sketch side
// ------------------------------------------------------------------

bool SdUsbDrive::startCard(const Config& config) {

    // Some cards need a second try right after power-up
    for (int attempt = 0; attempt < 2; attempt++) {

        bool ok;

        if (config.useSdio) {
            ok = _sd.cardBegin(SdioConfig(config.clkPin, config.cmdPin,
                                          config.dat0Pin));
        } else {
            ok = _sd.cardBegin(SdSpiConfig(config.csPin, SHARED_SPI,
                                           config.maxSck, config.spiPort));
        }

        if (ok) {
            _sectors = _sd.card()->sectorCount();
            if (_sectors > 0) {
                return true;
            }
        }

        _sd.end();
        delay(20);
    }

    return false;
}

void SdUsbDrive::addUsbInterface() {

    if (_usbAdded) {
        return;
    }

    USB.disconnect();
    _epIn = USB.registerEndpointIn();
    _epOut = USB.registerEndpointOut();

    static uint8_t mscDesc[] = {
        TUD_MSC_DESCRIPTOR(1 /* placeholder */, 0, _epOut, _epIn,
                           CFG_TUD_MSC_EP_BUFSIZE)
    };

    _usbId = USB.registerInterface(1, USBClass::simpleInterface, mscDesc,
                                   sizeof(mscDesc), 2, 0);
    USB.connect();
    _usbAdded = true;
}

bool SdUsbDrive::begin(const Config& config) {

    if (activeDrive && activeDrive != this) {
        return false;
    }

    // Release the card first if begin() is called again (card swap)
    lock();
    _owner = Owner::None;
    _writeCount = 0;
    _readCount = 0;
    unlock();

    _sd.end();
    _fsMounted = false;
    _config = config;
    _ioError = false;
    _ejected = false;

    const bool ok = startCard(config);

    if (ok) {
        _fsMounted = _sd.volumeBegin();
        lock();
        _owner = Owner::Sketch;
        unlock();
    }

    activeDrive = this;
    addUsbInterface();
    return ok;
}

void SdUsbDrive::end() {

    if (_usbAdded) {
        lock();
        flushWrites();
        _owner = Owner::None;
        unlock();

        USB.disconnect();
        USB.unregisterInterface(_usbId);
        USB.unregisterEndpointOut(_epOut);
        USB.unregisterEndpointIn(_epIn);
        USB.connect();
        _usbAdded = false;
    }

    _sd.end();
    _fsMounted = false;
    _owner = Owner::None;

    if (activeDrive == this) {
        activeDrive = nullptr;
    }
}

void SdUsbDrive::setIdentity(const char* vendor, const char* product,
                             const char* revision) {
    copyPadded(_vendor, sizeof(_vendor), vendor);
    copyPadded(_product, sizeof(_product), product);
    copyPadded(_revision, sizeof(_revision), revision);
}

void SdUsbDrive::setQuietTime(uint32_t ms) {
    _quietMs = ms;
}

bool SdUsbDrive::shareWithHost(bool readOnly) {

    if (_owner == Owner::None || _ioError) {
        return false;
    }

    if (_owner == Owner::Host && _readOnly == readOnly && !_ejected) {
        return true;
    }

    if (_owner == Owner::Sketch) {
        // Files must already be closed; this only makes sure the card
        // finished its own internal writes.
        _sd.card()->syncDevice();
        static_cast<FsVolume&>(_sd).end();
        _fsMounted = false;
    }

    lock();
    _readOnly = readOnly;
    _readCount = 0;
    _writeCount = 0;
    _ejected = false;
    _hostWrote = false;
    _mediaChanged = true;
    _owner = Owner::Host;
    unlock();

    return true;
}

bool SdUsbDrive::takeBack(bool force) {

    if (_owner == Owner::Sketch) {
        return _fsMounted;
    }

    if (_owner == Owner::None) {
        return false;
    }

    if (!force && !_ejected && msSinceHostWrite() < _quietMs) {
        return false;
    }

    lock();
    flushWrites();
    _readCount = 0;
    _owner = Owner::Sketch;
    unlock();

    if (_ioError) {
        return false;
    }

    // The host may have changed anything: mount from scratch
    _sd.card()->syncDevice();
    _fsMounted = _sd.volumeBegin();
    return _fsMounted;
}

bool SdUsbDrive::hostMounted() const {
    return _owner == Owner::Host && !_ejected && !_ioError &&
           tud_mounted() && millis() - _lastCmdMs < 3000;
}

bool SdUsbDrive::hostActive() const {
    return _owner == Owner::Host && millis() - _lastIoMs < 250;
}

uint32_t SdUsbDrive::msSinceHostWrite() const {
    return _hostWrote ? millis() - _lastWriteMs : UINT32_MAX;
}

// ------------------------------------------------------------------
// Host side (USB interrupt, USB.mutex held)
// ------------------------------------------------------------------

bool SdUsbDrive::hostAllowed() const {
    return _owner == Owner::Host && !_ejected && !_ioError;
}

bool SdUsbDrive::flushWrites() {

    if (_writeCount == 0) {
        return true;
    }

    const bool ok = _sd.card()->writeSectors(_writeStart, _writeBuf,
                                             _writeCount);
    _writeCount = 0;

    if (!ok) {
        _ioError = true;
    }

    return ok;
}

void SdUsbDrive::_inquiry(uint8_t vendor[8], uint8_t product[16],
                          uint8_t rev[4]) {
    memcpy(vendor, _vendor, sizeof(_vendor));
    memcpy(product, _product, sizeof(_product));
    memcpy(rev, _revision, sizeof(_revision));
}

bool SdUsbDrive::_testUnitReady(uint8_t lun) {

    _lastCmdMs = millis();

    if (!hostAllowed()) {
        tud_msc_set_sense(lun, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
        return false;
    }

    // Report the change once, so the host reads the card again
    if (_mediaChanged) {
        _mediaChanged = false;
        tud_msc_set_sense(lun, SENSE_UNIT_ATTENTION, ASC_MEDIUM_CHANGED, 0);
        return false;
    }

    return true;
}

void SdUsbDrive::_capacity(uint32_t* count, uint16_t* size) {

    if (hostAllowed()) {
        *count = _sectors;
        *size = SECTOR;
    } else {
        *count = 0;
        *size = 0;
    }
}

int32_t SdUsbDrive::_read10(uint32_t lba, uint32_t offset, void* buf,
                            uint32_t len) {

    if (!hostAllowed() || lba >= _sectors || offset + len > SECTOR) {
        return -1;
    }

    _lastIoMs = millis();
    _lastCmdMs = _lastIoMs;

    if (lba < _readStart || lba >= _readStart + _readCount) {

        // Written data must reach the card before it is read back
        if (!flushWrites()) {
            return -1;
        }

        uint32_t n = _sectors - lba;
        if (n > CACHE) {
            n = CACHE;
        }

        if (!_sd.card()->readSectors(lba, _readBuf, n)) {
            _readCount = 0;
            _ioError = true;
            return -1;
        }

        _readStart = lba;
        _readCount = n;
    }

    memcpy(buf, _readBuf + (lba - _readStart) * SECTOR + offset, len);

    if (offset + len == SECTOR) {
        _sectorsRead = _sectorsRead + 1;
    }

    return len;
}

int32_t SdUsbDrive::_write10(uint32_t lba, uint32_t offset,
                             const uint8_t* buf, uint32_t len) {

    if (!hostAllowed() || _readOnly || lba >= _sectors ||
            offset + len > SECTOR) {
        return -1;
    }

    const uint32_t now = millis();
    _lastIoMs = now;
    _lastCmdMs = now;
    _lastWriteMs = now;
    _hostWrote = true;

    if (offset == 0) {

        // A new sector: keep it only if it continues the buffered run
        if (_writeCount > 0 &&
                (lba != _writeStart + _writeCount || _writeCount == CACHE)) {
            if (!flushWrites()) {
                return -1;
            }
        }

        if (_writeCount == 0) {
            _writeStart = lba;
        }

        // The read cache must not return the old contents
        if (lba >= _readStart && lba < _readStart + _readCount) {
            _readCount = 0;
        }

    } else if (lba != _writeStart + _writeCount) {
        return -1;
    }

    memcpy(_writeBuf + _writeCount * SECTOR + offset, buf, len);

    if (offset + len == SECTOR) {
        _writeCount++;
        _sectorsWritten = _sectorsWritten + 1;
    }

    return len;
}

void SdUsbDrive::_writeComplete() {
    if (_owner == Owner::Host) {
        flushWrites();
    }
}

void SdUsbDrive::_startStop(bool start, bool loadEject) {

    if (!loadEject || _owner != Owner::Host) {
        return;
    }

    if (!start) {
        flushWrites();
        _sd.card()->syncDevice();
        _ejected = true;
        if (_onEject) {
            _onEject();
        }
    } else {
        _ejected = false;
        _mediaChanged = true;
    }
}

bool SdUsbDrive::_syncCache() {

    if (!hostAllowed()) {
        return false;
    }

    return flushWrites() && _sd.card()->syncDevice();
}

// ------------------------------------------------------------------
// TinyUSB mass storage callbacks
// ------------------------------------------------------------------

extern "C" {

// The core runs the USB task once per millisecond, and the mass storage
// class moves one 64-byte packet per run: only 64 KB/s. Running the
// task as soon as TinyUSB queues an event lets transfers go at bus
// speed. The task IRQ still takes USB.mutex, so nothing else changes.
void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void) rhport;
    (void) eventid;
#ifndef __FREERTOS
    if (in_isr && activeDrive) {
        irq_set_pending(USB.usbTaskIRQ);
    }
#else
    (void) in_isr;
#endif
}

uint8_t tud_msc_get_maxlun_cb(void) {
    return 1;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
    (void) lun;
    if (activeDrive) {
        activeDrive->_inquiry(vendor_id, product_id, product_rev);
    }
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!activeDrive) {
        tud_msc_set_sense(lun, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
        return false;
    }
    return activeDrive->_testUnitReady(lun);
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count,
                         uint16_t* block_size) {
    (void) lun;
    if (activeDrive) {
        activeDrive->_capacity(block_count, block_size);
    } else {
        *block_count = 0;
        *block_size = 0;
    }
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void* buffer, uint32_t bufsize) {
    (void) lun;
    return activeDrive ? activeDrive->_read10(lba, offset, buffer, bufsize)
                       : -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t* buffer, uint32_t bufsize) {
    (void) lun;
    return activeDrive ? activeDrive->_write10(lba, offset, buffer, bufsize)
                       : -1;
}

void tud_msc_write10_complete_cb(uint8_t lun) {
    (void) lun;
    if (activeDrive) {
        activeDrive->_writeComplete();
    }
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void) lun;
    return activeDrive && activeDrive->_writable();
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject) {
    (void) lun;
    (void) power_condition;
    if (activeDrive) {
        activeDrive->_startStop(start, load_eject);
    }
    return true;
}

bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun,
                                             uint8_t prohibit_removal,
                                             uint8_t control) {
    (void) lun;
    (void) control;
    if (activeDrive) {
        activeDrive->_preventRemoval(prohibit_removal != 0);
    }
    return true;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void* buffer, uint16_t bufsize) {
    (void) buffer;
    (void) bufsize;

    if (scsi_cmd[0] == SCSI_SYNCHRONIZE_CACHE_10) {
        if (activeDrive && activeDrive->_syncCache()) {
            return 0;
        }
        tud_msc_set_sense(lun, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
        return -1;
    }

    tud_msc_set_sense(lun, SENSE_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
    return -1;
}

} // extern "C"
