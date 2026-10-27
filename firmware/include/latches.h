/*
 * latches.h -- toggle inputs that remember their state across a power cycle.
 *
 * A toggle's on/off state normally lives in RAM, so every power-up starts with it off.
 * Flag a toggle channel CH_F_REMEMBER and its state is written to the EEPROM when it
 * changes and restored at boot -- so auto headlights left on come back on with the car.
 *
 * This is deliberately NOT "adopt the switch position at boot". The channel layer still
 * refuses that, so a stuck, shorted or wet button cannot bring its load up with the
 * board. What is restored is what the driver last chose, read from storage.
 */
#ifndef RCM_LATCHES_H
#define RCM_LATCHES_H

#include <stdint.h>

/* After cfg_load() and ch_begin(): adopt the remembered states. */
void latches_restore(void);

/* Every tick: write the remembered states once they have been stable briefly. */
void latches_tick(uint32_t now_ms);

/* Channels this applies to: inputs, IN_TOGGLE, CH_F_REMEMBER. */
uint32_t latches_mask(void);

#endif /* RCM_LATCHES_H */
