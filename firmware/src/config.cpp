/*
 * config.cpp -- straps from the DIP, configuration from EEPROM.
 */
#include <Arduino.h>
#include <string.h>
#include "board.h"
#include "config.h"
#include "store.h"
#include "protocol.h"

struct rcm_config_t cfg;
struct rcm_straps_t straps;

/* DIP positions are active LOW: internal pull-up, the switch shorts to ground. */
static inline bool dip_closed(uint32_t pin) { return digitalRead(pin) == LOW; }

void cfg_read_straps(void)
{
    const uint32_t pins[] = { PIN_CFG_ROLE, PIN_CFG_ADDR0, PIN_CFG_ADDR1,
                              PIN_CFG_BAUD, PIN_CFG_IMU_EN };
    for (uint32_t p : pins) pinMode(p, INPUT_PULLUP);
    delayMicroseconds(50);                /* let the pull-ups settle before sampling */

    straps.keypad      = dip_closed(PIN_CFG_ROLE);
    straps.address     = (dip_closed(PIN_CFG_ADDR0) ? 1 : 0)
                       | (dip_closed(PIN_CFG_ADDR1) ? 2 : 0);
    straps.force_500k  = dip_closed(PIN_CFG_BAUD);
    straps.publish_imu = dip_closed(PIN_CFG_IMU_EN);

    /* Role becomes the top bit of the node number, so a keypad and a relay module
     * can share an address without colliding on the bus. 8 nodes, 0..7. */
    straps.node = (uint8_t)((straps.keypad ? 4 : 0) | straps.address);
}

/* CRC-16/CCITT-FALSE. Chosen over a checksum because the failure this guards against
 * is a half-finished EEPROM write, which leaves a run of plausible-looking bytes. */
uint16_t cfg_crc16(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (uint8_t i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

#define CRC_LEN (sizeof(struct rcm_config_t) - sizeof(uint16_t))

bool cfg_valid(const struct rcm_config_t *c)
{
    return c->magic == RCM_CFG_MAGIC
        && c->version == RCM_CFG_VERSION
        && c->size == sizeof(struct rcm_config_t)
        && c->crc == cfg_crc16(c, CRC_LEN);
}

/* Defaults follow the ROLE strap, which is the whole reason the strap exists: a board
 * with a fresh EEPROM should already do the obviously right thing for what it is.
 * Requires cfg_read_straps() to have run. */
void cfg_defaults(struct rcm_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->magic   = RCM_CFG_MAGIC;
    c->version = RCM_CFG_VERSION;
    c->size    = sizeof(*c);

    c->can_bitrate       = RCM_BAUD_DEFAULT;
    c->can_base_id       = RCM_CAN_BASE_DEFAULT;
    c->broadcast_ms      = 50;
    c->can_timeout_ms    = 1000;
    c->input_debounce_ms = 25;
    c->output_settle_ms  = 100;   /* a relay coil's flyback needs time to collapse
                                   * before the sense node means anything */
    c->fault_confirm_ms  = 500;
    c->imu_rate_ms       = 20;    /* 50Hz -- what a Bosch MM5.10 runs at */

    for (uint8_t i = 0; i < RCM_CHANNELS; i++) {
        c->ch[i].mode  = straps.keypad ? CH_INPUT : CH_OUTPUT;
        c->ch[i].flags = 0;
    }
    c->failsafe_state = 0;        /* bus goes quiet -> everything off */

    c->peer_node        = PEER_NONE;
    c->peer_mask        = 0;
    c->peer_toggle_mask = 0;

    c->imu_map[0] = 0;  /* identity: vehicle X = sensor X, and so on */
    c->imu_map[1] = 1;
    c->imu_map[2] = 2;

    /* Ignition defaults to a plain level, which is what a key or a maintained switch
     * gives. Nothing about cranking is configured out of the box: a board that could
     * turn a starter without anyone having asked it to would be a poor default. */
    c->ign_mode         = IGN_MAINTAINED;
    c->ign_brake_ch     = IGN_CH_NONE;
    c->ign_start_ch     = IGN_CH_NONE;
    c->ign_run_ch       = IGN_CH_NONE;
    c->ign_hold_stop_ms = 1000;
    c->ign_crank_max_ms = 8000;
    c->ign_off_hold_ms  = 2000;

    /* Leave the record self-consistent. Nothing depends on it -- cfg_save() always
     * recomputes -- but it means cfg_valid(&cfg) is a usable invariant on the live
     * config rather than something that only holds after a save. */
    c->crc = cfg_crc16(c, CRC_LEN);
}

void cfg_load(void)
{
    struct rcm_config_t tmp;

    if (store_read(EEP_ADDR_CFG_A, &tmp, sizeof(tmp)) && cfg_valid(&tmp)) {
        cfg = tmp;
        return;
    }
    if (store_read(EEP_ADDR_CFG_B, &tmp, sizeof(tmp)) && cfg_valid(&tmp)) {
        cfg = tmp;
        /* The primary is bad and the backup is good: repair the primary now, while
         * we are sitting still, rather than discovering it again next boot. */
        tmp.crc = cfg_crc16(&tmp, CRC_LEN);
        store_write(EEP_ADDR_CFG_A, &tmp, sizeof(tmp));
        return;
    }
    cfg_defaults(&cfg);
}

bool cfg_save(void)
{
    cfg.magic   = RCM_CFG_MAGIC;
    cfg.version = RCM_CFG_VERSION;
    cfg.size    = sizeof(cfg);
    cfg.crc     = cfg_crc16(&cfg, CRC_LEN);

    /* Backup FIRST. If power drops between the two writes the backup is the older
     * good copy and the primary is untouched -- either way one valid record survives.
     * Writing the primary first would leave a window with two bad copies. */
    bool ok = store_write(EEP_ADDR_CFG_B, &cfg, sizeof(cfg));
    ok = store_write(EEP_ADDR_CFG_A, &cfg, sizeof(cfg)) && ok;
    return ok;
}

uint32_t cfg_effective_bitrate(void)
{
    /* The recovery strap wins outright. That is the point: you cannot fix a wrong
     * CAN bitrate over CAN, so there has to be one setting that no amount of bad
     * configuration can override. */
    if (straps.force_500k) return RCM_BAUD_RECOVERY;
    if (cfg.can_bitrate < 10000UL || cfg.can_bitrate > 1000000UL) return RCM_BAUD_DEFAULT;
    return cfg.can_bitrate;
}
