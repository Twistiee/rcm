/*
 * SPI.h -- host stub. Every transfer goes to sim_spi_transfer(), which the test
 * implements as a model of whatever is on the other end of that bus.
 */
#ifndef RCM_TEST_SPI_H
#define RCM_TEST_SPI_H

#include "Arduino.h"

#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3

class SPISettings {
public:
    SPISettings() {}
    SPISettings(uint32_t hz, uint8_t order, uint8_t mode)
        : clock(hz), bitOrder(order), dataMode(mode) {}
    uint32_t clock = 0;
    uint8_t  bitOrder = MSBFIRST;
    uint8_t  dataMode = SPI_MODE0;
};

class SPIClass {
public:
    SPIClass(uint32_t mosi, uint32_t miso, uint32_t sck)
        : _mosi(mosi), _miso(miso), _sck(sck) {}
    void begin(void) {}
    void beginTransaction(SPISettings) {}
    void endTransaction(void) {}
    uint8_t transfer(uint8_t out) { return sim_spi_transfer(out); }
    uint32_t _mosi, _miso, _sck;
};

#endif /* RCM_TEST_SPI_H */
