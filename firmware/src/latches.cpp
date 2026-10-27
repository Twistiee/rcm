/*
 * latches.cpp -- see latches.h.
 *
 * Stored as its own six-byte record, not inside the config: the config is ~300 bytes in
 * two copies, and rewriting it every time someone presses a button would be slow and
 * pointless. One page-aligned record, one short write per change.
 *
 * Wear is not a concern: the M95640 is rated for millions of write cycles per byte, and
 * a write only happens when a remembered toggle actually changes.
 */
#include <string.h>
#include "latches.h"
#include "channels.h"
#include "config.h"
#include "store.h"

#define LATCH_MAGIC     0xA7u
#define LATCH_SETTLE_MS 500u    /* write once a change has been stable this long -- a
                                 * few quick presses become one write, and the board
                                 * holds itself up for seconds at shutdown, so a change
                                 * made just before switching off still lands */

struct latch_rec_t {
    uint8_t  magic;
    uint8_t  bits[3];           /* 21 channels */
    uint16_t crc;
} __attribute__((packed));

static uint32_t written;        /* what the EEPROM holds now */
static uint32_t pending;
static uint32_t changed_at;
static bool     dirty;

uint32_t latches_mask(void)
{
    uint32_t m = 0;
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++)
        if (cfg.ch[ch].mode == CH_INPUT && cfg.ch[ch].behaviour == IN_TOGGLE
            && (cfg.ch[ch].flags & CH_F_REMEMBER))
            m |= 1ul << ch;
    return m;
}

void latches_restore(void)
{
    written = 0; dirty = false;
    struct latch_rec_t r;
    if (!store_read(EEP_ADDR_LATCH, &r, sizeof r)) return;
    if (r.magic != LATCH_MAGIC
        || r.crc != cfg_crc16(&r, sizeof r - sizeof r.crc)) return;   /* blank or torn: all off */

    written = (uint32_t)r.bits[0] | ((uint32_t)r.bits[1] << 8)
            | (((uint32_t)r.bits[2] & 0x1F) << 16);
    /* Only channels that are STILL remembered toggles. A channel since reassigned to
     * something else must not inherit a latch it no longer has. */
    ch_set_latched(written & latches_mask());
}

void latches_tick(uint32_t now_ms)
{
    const uint32_t now_bits = ch_latched() & latches_mask();
    if (now_bits != (dirty ? pending : written)) {
        pending = now_bits; changed_at = now_ms; dirty = true;
    }
    if (!dirty || (uint32_t)(now_ms - changed_at) < LATCH_SETTLE_MS) return;
    dirty = false;
    if (pending == written) return;      /* toggled and back again: nothing to write */

    struct latch_rec_t r;
    r.magic   = LATCH_MAGIC;
    r.bits[0] = (uint8_t)pending;
    r.bits[1] = (uint8_t)(pending >> 8);
    r.bits[2] = (uint8_t)((pending >> 16) & 0x1F);
    r.crc     = cfg_crc16(&r, sizeof r - sizeof r.crc);
    if (store_write(EEP_ADDR_LATCH, &r, sizeof r)) written = pending;
}
