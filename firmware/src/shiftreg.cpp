/*
 * shiftreg.cpp -- 3x 74HC595 out, 3x 74HC165 in, one shared SPI1 transfer.
 *
 * ===========================================================================
 * BIT ORDER. Read this before changing anything; it is the one part of this
 * firmware you cannot verify by inspection once the board is in a car.
 * ===========================================================================
 *
 * The two chains run in OPPOSITE physical directions. That is deliberate on the
 * board (it saved a ~74mm haul of MOSI across the whole PCB) but it means the
 * transmit and receive byte orders are mirror images of each other. Getting this
 * backwards gives you a board where channel 1 reads channel 15 -- which looks like
 * a wiring fault and is not.
 *
 * OUT (gen_spec.py: MOSI -> U_SO3.14, SO3.QH' -> SO2.SER, SO2.QH' -> SO1.SER):
 *   data enters at tile 3 and ripples AWAY from the MCU, so the FIRST byte sent
 *   travels furthest and lands in U_SO1.
 *       tx[0] -> U_SO1 -> channels  1..7
 *       tx[1] -> U_SO2 -> channels  8..14
 *       tx[2] -> U_SO3 -> channels 15..21
 *
 * IN  (gen_spec.py: SI1.DS=GND, SI1.Q7 -> SI2.DS, SI2.Q7 -> SI3.DS, SI3.Q7 -> MISO):
 *   the chain drains from tile 3 first, because U_SI3 is the one holding MISO.
 *       rx[0] <- U_SI3 -> channels 15..21
 *       rx[1] <- U_SI2 -> channels  8..14
 *       rx[2] <- U_SI1 -> channels  1..7
 *
 *   >>> tx byte index is (ch/7); rx byte index is (2 - ch/7). NOT the same. <<<
 *
 * WITHIN a byte, both directions happen to come out the same way round, which is
 * a genuine coincidence worth double-checking rather than trusting:
 *   595: SPI is MSB-first, and the first bit clocked into a 595 ends up at QH after
 *        8 clocks. So bit7->QH, bit6->QG ... bit0->QA. Channels use QA..QG (pins
 *        15,1,2,3,4,5,6) for k=1..7, so channel k sits at bit (k-1). QH is unused.
 *   165: after the load pulse Q7 presents D7, and each clock walks down to D0. MSB
 *        first again, so bit7=D7, bit0=D0. Channels use D0..D6 for k=1..7, so
 *        channel k sits at bit (k-1). D7 is that tile's J_AUX input.
 *
 * SPI MODE. Mode 0, and it must be, because the 595 samples SER on the RISING edge
 * of SRCLK -- mode 1 would move MOSI on that same edge and blow the setup time.
 * Mode 0 is slightly less comfortable for the 165, which also shifts on the rising
 * edge, so the master samples MISO at the very instant the 165 starts driving the
 * next bit. It reads the old (correct) value because of the 165's output propagation
 * delay, which is the standard way these parts are read over SPI -- but it is why the
 * clock is held at 4MHz rather than pushed. 24 bits at 4MHz is 6us; there is nothing
 * to gain by going faster and real margin to lose.
 */
#include <Arduino.h>
#include <SPI.h>
#include "board.h"
#include "shiftreg.h"

#define SR_SPI_HZ 4000000UL

static SPIClass sr_spi(PIN_SR_MOSI, PIN_SR_MISO, PIN_SR_SCK);
static SPISettings sr_settings(SR_SPI_HZ, MSBFIRST, SPI_MODE0);

static uint8_t tx[RCM_SR_BYTES];
static uint8_t rx[RCM_SR_BYTES];
static bool    oe_on;

/* tx and rx byte indices for a 0-based channel. The asymmetry is the chain direction;
 * see the header comment. */
static constexpr uint8_t tx_byte(uint8_t ch) { return ch / RCM_CH_PER_TILE; }
static constexpr uint8_t rx_byte(uint8_t ch) { return (RCM_TILES - 1) - ch / RCM_CH_PER_TILE; }
static constexpr uint8_t ch_bit(uint8_t ch)  { return ch % RCM_CH_PER_TILE; }

/* ---------------------------------------------------------------------------
 * The bit map, asserted at COMPILE TIME against the netlist.
 *
 * This is the one thing in the firmware that cannot be checked by reading the
 * code back -- a mirrored byte order looks completely reasonable and only shows
 * up as channel 1 reporting channel 15's fuse, which reads like a wiring fault.
 * So the expected answers are written out here longhand, taken from gen_spec.py's
 * daisy-chain nets rather than derived from the functions above. If someone
 * "simplifies" tx_byte and rx_byte into the same expression, the build breaks.
 *
 * From gen_spec.py:
 *   MOSI -> U_SO3.SER, SO3.QH' -> SO2.SER, SO2.QH' -> SO1.SER   (out: 1st byte reaches SO1)
 *   SI1.Q7 -> SI2.DS,  SI2.Q7 -> SI3.DS,   SI3.Q7 -> MISO       (in:  1st byte comes from SI3)
 * --------------------------------------------------------------------------- */
/* channel 1 (index 0) is the first pin of tile 1 */
static_assert(tx_byte(0)  == 0 && ch_bit(0)  == 0, "ch1 out: U_SO1 QA");
static_assert(rx_byte(0)  == 2 && ch_bit(0)  == 0, "ch1 in : U_SI1 D0, LAST byte in");
/* channel 7 (index 6) is the last pin of tile 1 -- QG/D6, not QH/D7 */
static_assert(tx_byte(6)  == 0 && ch_bit(6)  == 6, "ch7 out: U_SO1 QG");
static_assert(rx_byte(6)  == 2 && ch_bit(6)  == 6, "ch7 in : U_SI1 D6");
/* channel 8 (index 7) is the first pin of tile 2 */
static_assert(tx_byte(7)  == 1 && ch_bit(7)  == 0, "ch8 out: U_SO2 QA");
static_assert(rx_byte(7)  == 1 && ch_bit(7)  == 0, "ch8 in : U_SI2 D0");
/* channel 15 (index 14) is the first pin of tile 3 -- the tile MOSI reaches FIRST
 * but whose byte is sent LAST */
static_assert(tx_byte(14) == 2 && ch_bit(14) == 0, "ch15 out: U_SO3 QA");
static_assert(rx_byte(14) == 0 && ch_bit(14) == 0, "ch15 in : U_SI3 D0, FIRST byte in");
/* channel 21 (index 20), the far end of everything */
static_assert(tx_byte(20) == 2 && ch_bit(20) == 6, "ch21 out: U_SO3 QG");
static_assert(rx_byte(20) == 0 && ch_bit(20) == 6, "ch21 in : U_SI3 D6");
/* and the property that makes the two directions different in the first place */
static_assert(tx_byte(0) != rx_byte(0), "the two chains run opposite ways -- see header");

void sr_begin(void)
{
    pinMode(PIN_SR_OE_N, OUTPUT);
    digitalWrite(PIN_SR_OE_N, HIGH);     /* keep the 595s Hi-Z; R_OE already does */
    oe_on = false;

    pinMode(PIN_SR_RCLK, OUTPUT);
    digitalWrite(PIN_SR_RCLK, LOW);
    pinMode(PIN_SR_PL, OUTPUT);
    digitalWrite(PIN_SR_PL, HIGH);       /* PL is active LOW: high = shift mode */

    sr_spi.begin();

    /* Shift a zeroed frame out and LATCH it before anything can enable the outputs.
     * Until this has happened the 595 storage registers hold power-on garbage, and
     * dropping SR_OE_N would put that garbage on 21 relay coils. */
    for (uint8_t i = 0; i < RCM_SR_BYTES; i++) tx[i] = 0;
    sr_exchange();
}

void sr_exchange(void)
{
    /* 1. Freeze the inputs. PL low makes the 165s transparent; they capture their
     *    parallel inputs while it is low and hold them once it goes high again.
     *    20ns is the datasheet minimum -- one microsecond is free and unambiguous. */
    digitalWrite(PIN_SR_PL, LOW);
    delayMicroseconds(1);
    digitalWrite(PIN_SR_PL, HIGH);

    /* 2. One transfer does both chains. */
    sr_spi.beginTransaction(sr_settings);
    for (uint8_t i = 0; i < RCM_SR_BYTES; i++) rx[i] = sr_spi.transfer(tx[i]);
    sr_spi.endTransaction();

    /* 3. Publish. The 595 outputs do not change until this rising edge, so the whole
     *    24-bit frame appears at once and no channel glitches mid-shift.
     *
     *    Note the consequence of doing this LAST: the inputs read in step 1 were
     *    sampled against the previous output state, so a channel commanded on in this
     *    cycle does not show up in its own sense bit until the cycle after next. That
     *    is fine -- fault diagnosis waits output_settle_ms anyway -- but it is real,
     *    and test_sense_lags_a_commanded_change_by_one_exchange pins it down. */
    digitalWrite(PIN_SR_RCLK, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_SR_RCLK, LOW);
}

void sr_set(uint8_t ch, bool on)
{
    if (ch >= RCM_CHANNELS) return;
    const uint8_t m = 1u << ch_bit(ch);
    if (on) tx[tx_byte(ch)] |= m;
    else    tx[tx_byte(ch)] &= (uint8_t)~m;
}

bool sr_get(uint8_t ch)
{
    if (ch >= RCM_CHANNELS) return false;
    return (tx[tx_byte(ch)] >> ch_bit(ch)) & 1u;
}

void sr_set_all(uint32_t bits21)
{
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) sr_set(ch, (bits21 >> ch) & 1u);
}

uint32_t sr_get_all(void)
{
    uint32_t v = 0;
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) if (sr_get(ch)) v |= 1ul << ch;
    return v;
}

bool sr_sense(uint8_t ch)
{
    if (ch >= RCM_CHANNELS) return false;
    return (rx[rx_byte(ch)] >> ch_bit(ch)) & 1u;
}

uint32_t sr_sense_all(void)
{
    uint32_t v = 0;
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++) if (sr_sense(ch)) v |= 1ul << ch;
    return v;
}

/* J_AUX pin a+1 goes to tile (a+1)'s 165 bit 7 -- so aux 0 is in U_SI1, which is the
 * LAST byte received. Same mirror as the channels. */
bool sr_aux(uint8_t a)
{
    if (a >= RCM_AUX_INPUTS) return false;
    return (rx[(RCM_TILES - 1) - a] >> 7) & 1u;
}

uint8_t sr_aux_all(void)
{
    uint8_t v = 0;
    for (uint8_t a = 0; a < RCM_AUX_INPUTS; a++) if (sr_aux(a)) v |= 1u << a;
    return v;
}

void sr_outputs_enable(bool en)
{
    oe_on = en;
    digitalWrite(PIN_SR_OE_N, en ? LOW : HIGH);   /* active low */
}

bool sr_outputs_enabled(void) { return oe_on; }
