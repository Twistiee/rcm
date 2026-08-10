/*
 * shiftreg.h -- the 24-bit-out / 24-bit-in shift register chain.
 *
 * Outputs and inputs are clocked SIMULTANEOUSLY by one SPI transfer: MOSI feeds the
 * 74HC595 chain, MISO drains the 74HC165 chain, and SR_SCK is shared. That is not a
 * coincidence, it is why the board was drawn this way -- 21 outputs and 24 inputs cost
 * one 3-byte SPI exchange, about 12us.
 *
 * Nothing above this file should ever touch a raw byte. Use the channel accessors.
 */
#ifndef RCM_SHIFTREG_H
#define RCM_SHIFTREG_H

#include <stdint.h>
#include <stdbool.h>

void sr_begin(void);

/* One atomic cycle: latch the 165 inputs, clock 24 bits both ways, publish the 595
 * outputs. Call this at a steady rate -- it IS the I/O tick. */
void sr_exchange(void);

/* --- outputs (staged; take effect at the next sr_exchange) ------------------ */
void sr_set(uint8_t ch, bool on);       /* ch: 0..20 */
bool sr_get(uint8_t ch);
void sr_set_all(uint32_t bits21);
uint32_t sr_get_all(void);

/* --- inputs (as of the last sr_exchange) ----------------------------------- */
bool sr_sense(uint8_t ch);              /* ch: 0..20; true = channel node is at +12V */
uint32_t sr_sense_all(void);
bool sr_aux(uint8_t a);                 /* a: 0..2, the J_AUX inputs */
uint8_t sr_aux_all(void);

/* --- output enable --------------------------------------------------------- */
/* R_OE holds SR_OE_N high (outputs Hi-Z) from the instant power arrives. Only call
 * sr_outputs_enable(true) after a known state has been shifted out and latched, or
 * every relay on the car clacks on during boot. sr_begin() does that sequencing. */
void sr_outputs_enable(bool en);
bool sr_outputs_enabled(void);

#endif /* RCM_SHIFTREG_H */
