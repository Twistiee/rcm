/*
 * simboard.h -- a model of the RCM hardware, for the native tests.
 *
 * The interesting part is the shift register chain, modelled as the real thing:
 * three 74HC595s daisy-chained one way and three 74HC165s daisy-chained the OTHER
 * way, exactly as gen_spec.py wires them. The firmware's own shiftreg.cpp drives it
 * through the SPI stub. If the byte order in the driver were mirrored, this model
 * would report channel 15 where channel 1 was expected -- which is the whole reason
 * it exists, because that is a bug you cannot see by reading either side alone.
 *
 * Each channel then gets a little electrical model, so the channel layer's fuse
 * diagnosis can be tested against a blown fuse rather than against a mock that
 * simply asserts the answer.
 */
#ifndef RCM_TEST_SIMBOARD_H
#define RCM_TEST_SIMBOARD_H

#include <stdint.h>
#include <string.h>
#include "board.h"

/* What is wired to a channel terminal. */
enum sim_wiring {
    SIM_UNCONNECTED = 0,  /* nothing on the terminal: the 10k pull-down wins, reads LOW */
    SIM_COIL_OK,          /* relay coil fed from an intact fused +12V                   */
    SIM_COIL_OPEN,        /* blown fuse, missing relay, open coil, broken wire          */
    SIM_BUTTON_OPEN,      /* button to +12V, not pressed                                */
    SIM_BUTTON_PRESSED,   /* button to +12V, pressed                                    */
    SIM_SHORT_12V,        /* hard short to +12V, or a driver that will not pull down    */
};

struct simboard {
    /* --- what the test sets --- */
    uint8_t  wiring[RCM_CHANNELS];
    uint8_t  aux[RCM_AUX_INPUTS];
    uint8_t  dip_closed[8];        /* index by DIP position 0..7 (pin, not switch no.) */
    uint16_t ign_mv_at_pin;

    /* --- what the firmware has done --- */
    uint32_t latch595;             /* 24 published output bits                          */
    uint32_t chain595;             /* 24 bits still in the shift register               */
    uint32_t chain165;             /* 24 bits being clocked out toward MISO             */
    bool     oe_low;               /* SR_OE_N driven low = outputs live                 */
    bool     latch_hold;
    bool     led1, led2;
    int      pl_level, rclk_level;

    uint32_t now_ms;
    uint32_t exchanges;            /* how many sr_exchange() cycles have run            */

    /* --- M95640 on the other SPI bus ---
     * A real one, page wrap and all: a write burst that runs off the end of a 32-byte
     * page comes back round to the START of that page and overwrites what it just
     * wrote. It does not error. store_write() splitting at page boundaries is the only
     * thing standing between the config record and quiet corruption, so the model has
     * to reproduce the misbehaviour or the test proves nothing. */
    uint8_t  eep[8192];
    bool     eep_fitted;           /* set false to test the no-EEPROM path */
    bool     eep_wel;              /* write enable latch */
    int      eep_wip_polls;        /* status reads still to report "busy" */
    int      eep_cs;
    uint8_t  eep_cmd, eep_phase;
    uint16_t eep_addr;
    bool     eep_wrapped;          /* this burst has come back round within its page */
    uint32_t eep_page_wraps;       /* bytes clobbered by a wrap -- must stay 0 */
};

extern struct simboard SIM;

/* Where a 0-based channel physically sits in each 24-bit chain.
 *
 * THE TWO FORMULAS ARE DIFFERENT, and the first draft of this file got it wrong by
 * copying one into the other. Both chains are indexed here with bit 23 as the end
 * furthest from the MCU in transmit terms and first out in receive terms:
 *
 *   595: bit 0 = U_SO3.QA ... bit 23 = U_SO1.QH.
 *        MOSI enters at SO3 and ripples away, so the first bit sent travels furthest
 *        and tile 1 ends up at the TOP.            -> base = (2 - tile) * 8
 *
 *   165: bit 23 = U_SI3.D7 ... bit 0 = U_SI1.D0.
 *        SI3 is the one holding MISO, so tile 3 comes out FIRST and therefore sits
 *        at the top.                               -> base = tile * 8
 *
 * That is the mirror the driver has to undo, and writing it out from the netlist here
 * -- rather than reusing the driver's own expression -- is the only reason a mistake
 * on either side shows up as a failing test rather than as a mystery in a car.
 */
static inline uint8_t sim_pos595(uint8_t ch)
{
    const uint8_t t = ch / RCM_CH_PER_TILE, k = ch % RCM_CH_PER_TILE;
    return (uint8_t)((RCM_TILES - 1 - t) * 8 + k);
}
static inline uint8_t sim_pos165(uint8_t ch)
{
    const uint8_t t = ch / RCM_CH_PER_TILE, k = ch % RCM_CH_PER_TILE;
    return (uint8_t)(t * 8 + k);
}
/* J_AUX pin a+1 lands on tile (a+1)'s spare 165 bit, D7. */
static inline uint8_t sim_pos_aux(uint8_t a)
{
    return (uint8_t)(a * 8 + 7);
}

void sim_reset(void);
bool sim_driver_on(uint8_t ch);   /* is the TPL7407L actually sinking this channel */
bool sim_sense_level(uint8_t ch); /* what the 165 input pin sees right now */

#endif /* RCM_TEST_SIMBOARD_H */
