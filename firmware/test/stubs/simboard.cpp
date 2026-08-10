/*
 * simboard.cpp -- the hardware model behind the Arduino/SPI stubs.
 */
#include "simboard.h"
#include "Arduino.h"

struct simboard SIM;

void sim_reset(void)
{
    memset(&SIM, 0, sizeof(SIM));
    SIM.pl_level = HIGH;
    SIM.rclk_level = LOW;
    SIM.ign_mv_at_pin = 2700;      /* ~12.7V at J_IGN through the 1M/270k divider */
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) SIM.wiring[i] = SIM_UNCONNECTED;
    SIM.eep_fitted = true;
    SIM.eep_cs = HIGH;
    memset(SIM.eep, 0xFF, sizeof(SIM.eep));   /* erased EEPROM reads as 0xFF */
}

bool sim_driver_on(uint8_t ch)
{
    if (!SIM.oe_low) return false;                 /* 595 outputs high-impedance */
    return (SIM.latch595 >> sim_pos595(ch)) & 1u;
}

bool sim_sense_level(uint8_t ch)
{
    const bool driven = sim_driver_on(ch);
    switch (SIM.wiring[ch]) {
    case SIM_COIL_OK:        return !driven;   /* coil pulls the node up unless we sink it */
    case SIM_COIL_OPEN:      return false;     /* nothing to pull it up; the 10k wins      */
    case SIM_BUTTON_PRESSED: return !driven;
    case SIM_BUTTON_OPEN:    return false;
    case SIM_SHORT_12V:      return true;      /* stays up even when we try to sink it     */
    case SIM_UNCONNECTED:
    default:                 return false;
    }
}

/* Sample every channel into the 165s. Real 165s are transparent while PL is low and
 * hold once it rises; sampling on the rising edge is the same thing observationally. */
static void sim_load_165(void)
{
    uint32_t v = 0;
    for (uint8_t ch = 0; ch < RCM_CHANNELS; ch++)
        if (sim_sense_level(ch)) v |= 1ul << sim_pos165(ch);
    for (uint8_t a = 0; a < RCM_AUX_INPUTS; a++)
        if (SIM.aux[a]) v |= 1ul << sim_pos_aux(a);
    SIM.chain165 = v;
    SIM.exchanges++;
}

extern "C" {

uint32_t sim_millis(void)          { return SIM.now_ms; }
void     sim_advance(uint32_t ms)  { SIM.now_ms += ms; }

void sim_pin_mode(uint32_t pin, uint32_t mode) { (void)pin; (void)mode; }

void sim_digital_write(uint32_t pin, uint32_t val)
{
    switch (pin) {
    case PIN_SR_PL:
        /* Rising edge ends the load window. */
        if (SIM.pl_level == LOW && val == HIGH) sim_load_165();
        SIM.pl_level = (int)val;
        break;
    case PIN_SR_RCLK:
        /* Rising edge publishes the shift register to the output latch. */
        if (SIM.rclk_level == LOW && val == HIGH) SIM.latch595 = SIM.chain595;
        SIM.rclk_level = (int)val;
        break;
    case PIN_EEP_CS:
        /* Chip select framing IS the command boundary on this part -- a write only
         * begins executing on the rising edge. */
        if (SIM.eep_cs == LOW && val == HIGH) {
            if (SIM.eep_cmd == 0x02 && SIM.eep_wel) {
                SIM.eep_wel = false;          /* WEL self-clears after a write */
                SIM.eep_wip_polls = 3;        /* and the part is busy for a while */
            }
        }
        SIM.eep_cs = (int)val;
        if (val == LOW) { SIM.eep_cmd = 0; SIM.eep_phase = 0; SIM.eep_wrapped = false; }
        break;
    case PIN_SR_OE_N:    SIM.oe_low     = (val == LOW);  break;
    case PIN_LATCH_HOLD: SIM.latch_hold = (val == HIGH); break;
    case PIN_LED1:       SIM.led1       = (val == HIGH); break;
    case PIN_LED2:       SIM.led2       = (val == HIGH); break;
    default: break;
    }
}

int sim_digital_read(uint32_t pin)
{
    /* The config DIP shorts to ground, so a closed switch reads LOW. */
    switch (pin) {
    case PIN_CFG_ROLE:   return SIM.dip_closed[0] ? LOW : HIGH;
    case PIN_CFG_ADDR0:  return SIM.dip_closed[1] ? LOW : HIGH;
    case PIN_CFG_ADDR1:  return SIM.dip_closed[2] ? LOW : HIGH;
    case PIN_CFG_BAUD:   return SIM.dip_closed[3] ? LOW : HIGH;
    case PIN_CFG_IMU_EN: return SIM.dip_closed[4] ? LOW : HIGH;
    default:             return HIGH;
    }
}

int sim_analog_read(uint32_t pin)
{
    if (pin != PIN_IGN_SENSE) return 0;
    /* 12-bit ADC against 3.3V. */
    uint32_t counts = ((uint32_t)SIM.ign_mv_at_pin * 4095UL) / 3300UL;
    return (int)(counts > 4095 ? 4095 : counts);
}

/* --- M95640 command state machine ------------------------------------------ */
static uint8_t sim_eeprom_byte(uint8_t out)
{
    if (!SIM.eep_fitted) return 0xFF;      /* MISO floats; commonly reads all-ones */

    if (SIM.eep_phase == 0) {              /* command byte */
        SIM.eep_cmd = out;
        SIM.eep_phase = 1;
        switch (out) {
        case 0x06: SIM.eep_wel = true;  SIM.eep_phase = 0xFF; break;  /* WREN */
        case 0x04: SIM.eep_wel = false; SIM.eep_phase = 0xFF; break;  /* WRDI */
        default: break;
        }
        return 0xFF;
    }

    switch (SIM.eep_cmd) {
    case 0x05: {                           /* RDSR */
        uint8_t s = 0;
        if (SIM.eep_wip_polls > 0) { s |= 0x01; SIM.eep_wip_polls--; }
        if (SIM.eep_wel) s |= 0x02;
        return s;
    }
    case 0x02:                             /* WRITE */
    case 0x03:                             /* READ  */
        if (SIM.eep_phase == 1) { SIM.eep_addr = (uint16_t)(out << 8); SIM.eep_phase = 2; return 0xFF; }
        if (SIM.eep_phase == 2) { SIM.eep_addr |= out; SIM.eep_phase = 3; return 0xFF; }
        if (SIM.eep_cmd == 0x03) {
            /* Reads run on sequentially through the whole array; no page limit. */
            uint8_t v = SIM.eep[SIM.eep_addr % sizeof(SIM.eep)];
            SIM.eep_addr = (uint16_t)((SIM.eep_addr + 1) % sizeof(SIM.eep));
            return v;
        }
        if (SIM.eep_wel) {
            /* Count a wrap only when a byte is actually written after the address has
             * come back round -- that is the moment data gets clobbered. Counting at
             * the point the address wraps would flag every burst that merely FILLS a
             * page to its last byte, which is exactly what a correctly split write
             * does. */
            if (SIM.eep_wrapped) SIM.eep_page_wraps++;
            SIM.eep[SIM.eep_addr % sizeof(SIM.eep)] = out;
            /* THE PAGE WRAP. Only the low 5 bits of the address increment. */
            const uint16_t page = (uint16_t)(SIM.eep_addr & ~0x1Fu);
            const uint16_t next = (uint16_t)(page | ((SIM.eep_addr + 1) & 0x1Fu));
            if (next < SIM.eep_addr) SIM.eep_wrapped = true;
            SIM.eep_addr = next;
        }
        return 0xFF;
    default:
        return 0xFF;
    }
}

/* One byte each way through both chains, MSB first, sharing the clock.
 * The board has two SPI buses; chip select says which one this byte is on. */
uint8_t sim_spi_transfer(uint8_t out)
{
    if (SIM.eep_cs == LOW) return sim_eeprom_byte(out);

    uint8_t in = 0;
    for (int i = 7; i >= 0; i--) {
        /* The 165 presents its top bit before the edge; the master reads it, then
         * the clock shifts everything along. SI1's serial input is tied to ground,
         * so zeros walk in behind. */
        in = (uint8_t)((in << 1) | ((SIM.chain165 >> 23) & 1u));
        SIM.chain165 = (SIM.chain165 << 1) & 0xFFFFFFul;
        /* The 595 chain takes MOSI at the near end and everything ripples away. */
        SIM.chain595 = ((SIM.chain595 << 1) | ((out >> i) & 1u)) & 0xFFFFFFul;
    }
    return in;
}

} /* extern "C" */
