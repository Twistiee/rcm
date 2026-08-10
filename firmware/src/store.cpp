/*
 * store.cpp -- M95640 driver.
 *
 * SPI2 is entirely ours: nothing else is on PB13/14/15 and PB12 is a dedicated CS.
 * Mode 0, MSB first. The M95640 supports up to 20MHz; 8MHz is plenty for ~90 bytes
 * of config and keeps the edges civilised on a board with 12V switching on it.
 */
#include <Arduino.h>
#include <SPI.h>
#include "board.h"
#include "store.h"

#define CMD_WREN  0x06
#define CMD_WRDI  0x04
#define CMD_RDSR  0x05
#define CMD_WRSR  0x01
#define CMD_READ  0x03
#define CMD_WRITE 0x02

#define SR_WIP    0x01   /* write in progress */
#define SR_WEL    0x02   /* write enable latch */

#define EEP_SPI_HZ    8000000UL
#define WRITE_TIMEOUT 20   /* ms; datasheet says a page write completes within 5ms */

static SPIClass eep_spi(PIN_EEP_MOSI, PIN_EEP_MISO, PIN_EEP_SCK);
static SPISettings eep_settings(EEP_SPI_HZ, MSBFIRST, SPI_MODE0);
static bool present;

static inline void cs_lo(void) { digitalWrite(PIN_EEP_CS, LOW); }
static inline void cs_hi(void) { digitalWrite(PIN_EEP_CS, HIGH); }

static uint8_t read_status(void)
{
    eep_spi.beginTransaction(eep_settings);
    cs_lo();
    eep_spi.transfer(CMD_RDSR);
    uint8_t s = eep_spi.transfer(0xFF);
    cs_hi();
    eep_spi.endTransaction();
    return s;
}

/* Returns false on timeout. A stuck WIP means the part is absent or held in reset --
 * either way the caller must not treat the write as done. */
static bool wait_ready(void)
{
    uint32_t t0 = millis();
    while ((read_status() & SR_WIP) != 0) {
        if (millis() - t0 > WRITE_TIMEOUT) return false;
    }
    return true;
}

void store_begin(void)
{
    pinMode(PIN_EEP_CS, OUTPUT);
    cs_hi();
    eep_spi.begin();

    /* Probe by toggling the write-enable latch. Reading the status register alone
     * proves nothing: with no chip fitted, MISO floats and can read as a plausible
     * 0x00. WEL going up and back down again is a real round trip. */
    eep_spi.beginTransaction(eep_settings);
    cs_lo(); eep_spi.transfer(CMD_WREN); cs_hi();
    eep_spi.endTransaction();
    bool set = (read_status() & SR_WEL) != 0;

    eep_spi.beginTransaction(eep_settings);
    cs_lo(); eep_spi.transfer(CMD_WRDI); cs_hi();
    eep_spi.endTransaction();
    bool cleared = (read_status() & SR_WEL) == 0;

    present = set && cleared;
}

bool store_present(void) { return present; }

bool store_read(uint16_t addr, void *dst, uint16_t len)
{
    if (!present || (uint32_t)addr + len > EEP_SIZE) return false;
    uint8_t *p = (uint8_t *)dst;

    eep_spi.beginTransaction(eep_settings);
    cs_lo();
    eep_spi.transfer(CMD_READ);
    eep_spi.transfer((uint8_t)(addr >> 8));
    eep_spi.transfer((uint8_t)addr);
    for (uint16_t i = 0; i < len; i++) p[i] = eep_spi.transfer(0xFF);
    cs_hi();
    eep_spi.endTransaction();
    return true;
}

static bool write_page(uint16_t addr, const uint8_t *src, uint16_t len)
{
    eep_spi.beginTransaction(eep_settings);
    cs_lo(); eep_spi.transfer(CMD_WREN); cs_hi();
    eep_spi.endTransaction();

    eep_spi.beginTransaction(eep_settings);
    cs_lo();
    eep_spi.transfer(CMD_WRITE);
    eep_spi.transfer((uint8_t)(addr >> 8));
    eep_spi.transfer((uint8_t)addr);
    for (uint16_t i = 0; i < len; i++) eep_spi.transfer(src[i]);
    cs_hi();                       /* the write only STARTS on this rising edge */
    eep_spi.endTransaction();

    return wait_ready();
}

bool store_write(uint16_t addr, const void *src, uint16_t len)
{
    if (!present || (uint32_t)addr + len > EEP_SIZE) return false;
    const uint8_t *p = (const uint8_t *)src;

    while (len) {
        /* Never let a burst run past the end of its page -- the part would wrap it
         * back to the page start and overwrite what we just sent. */
        uint16_t room = EEP_PAGE - (addr % EEP_PAGE);
        uint16_t n    = len < room ? len : room;
        if (!write_page(addr, p, n)) return false;
        addr += n;
        p    += n;
        len  -= n;
    }
    return true;
}
