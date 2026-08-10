/*
 * channels.h -- the 21 universal channels, as the rest of the firmware sees them.
 *
 * Every channel is simultaneously an output and an input in hardware: the sense
 * divider never disconnects, so a channel can be read while it is being driven.
 * What "output" and "input" mean here is purely a matter of the configured mode --
 * see DESIGN.md, "Channel mode became a software table".
 */
#ifndef RCM_CHANNELS_H
#define RCM_CHANNELS_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

void ch_begin(void);

/* The I/O tick. Exchanges the shift registers, debounces, and ages the fault
 * timers. Everything else in this header reports what the last tick found. */
void ch_tick(uint32_t now_ms);

/* Commands are IGNORED on channels not configured CH_OUTPUT. That is a safety
 * property, not tidiness: driving a channel wired to a button would short that
 * button's +12V feed to ground through the TPL7407L. */
void ch_command(uint8_t ch, bool on);
void ch_command_mask(uint32_t mask, uint32_t values);
void ch_all_off(void);
void ch_apply_failsafe(void);

uint32_t ch_commanded(void);     /* what we are asking the drivers to do */
uint32_t ch_inputs(void);        /* debounced, invert applied, input channels only */
uint32_t ch_sense_raw(void);     /* what the sense pins actually read, all channels */
uint8_t  ch_aux(void);           /* debounced J_AUX bits */

/* Confirmed faults. Both are only ever set on CH_OUTPUT channels without CH_F_NO_DIAG,
 * and only after the condition has held for fault_confirm_ms. */
uint32_t ch_fault_open(void);    /* driver off but the node is not at +12V:
                                  * blown fuse, missing relay, open coil, broken wire */
uint32_t ch_fault_short(void);   /* driver on but the node stays at +12V:
                                  * the low side is not pulling down, or a 12V short */
void     ch_clear_faults(void);

#endif /* RCM_CHANNELS_H */
