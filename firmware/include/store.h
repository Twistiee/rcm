/*
 * store.h -- M95640 SPI EEPROM (8 KB) on SPI2, chip select PB12.
 *
 * Raw byte access only. Anything that knows what the bytes MEAN lives in config.cpp.
 *
 * The part is 8192 bytes in 32-byte pages. A write that crosses a page boundary wraps
 * around to the start of the SAME page instead of continuing -- it does not error, it
 * quietly corrupts. store_write() splits at page boundaries so callers never have to
 * think about it.
 */
#ifndef RCM_STORE_H
#define RCM_STORE_H

#include <stdint.h>
#include <stdbool.h>

#define EEP_SIZE       8192u
#define EEP_PAGE       32u

/* Two copies of the config, far enough apart to be in different pages. A write to the
 * primary that loses power halfway leaves the backup intact and CRC-valid. */
#define EEP_ADDR_CFG_A 0x0000u
#define EEP_ADDR_CFG_B 0x0400u

void store_begin(void);
bool store_present(void);                                    /* status register responds */
bool store_read(uint16_t addr, void *dst, uint16_t len);
bool store_write(uint16_t addr, const void *src, uint16_t len);

#endif /* RCM_STORE_H */
