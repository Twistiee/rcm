/*
 * sysclock.h -- see sysclock.cpp. The core runs from the 8 MHz crystal, falling back to the
 * internal RC oscillator only if the crystal will not start.
 */
#ifndef RCM_SYSCLOCK_H
#define RCM_SYSCLOCK_H

#include <stdbool.h>

/* True if the crystal started. False means the board is on HSI, whose drift is enough
 * to make CAN unreliable -- STATUS byte 3 bit 2 reports it. */
bool clock_on_crystal(void);

#endif /* RCM_SYSCLOCK_H */
