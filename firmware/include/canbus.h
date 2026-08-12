/*
 * canbus.h -- CAN1 on PB8/PB9, any bitrate.
 *
 * Written straight onto the STM32 HAL's bxCAN driver rather than pulling in an
 * Arduino CAN library, for one reason: the bit timing has to be SOLVED for an
 * arbitrary bitrate rather than looked up in a table of the usual four. The board
 * has to sit on a 500k rusEFI bus, but the whole point of making the rate settable
 * is that somebody else's loom runs at 250k or 1M.
 */
#ifndef RCM_CANBUS_H
#define RCM_CANBUS_H

#include <stdint.h>
#include <stdbool.h>

struct can_frame_t {
    uint16_t id;
    uint8_t  len;
    uint8_t  data[8];
};

/* Returns false if the requested bitrate cannot be produced from the current APB1
 * clock, or if the peripheral refuses to leave initialisation mode. Check it: a
 * silently dead CAN node is the single most annoying thing to diagnose in a car. */
bool can_begin(uint32_t bitrate);

/* Loopback puts the peripheral in CAN_MODE_LOOPBACK: it still drives the bus, but it
 * also receives its own frames. That is how the self-test proves the controller, the
 * bit timing and the filter banks with nothing else connected -- normally a lone CAN
 * node cannot transmit at all, because no other node is there to acknowledge it. */
bool can_begin_ex(uint32_t bitrate, bool loopback);

/* Queues if all three hardware mailboxes are busy. Returns false only if the software
 * queue is also full, which at our frame rates means the bus is not running. */
bool can_send(uint16_t id, const uint8_t *data, uint8_t len);
/* Moves queued frames into mailboxes as they free up. Call it every time round the
 * main loop; without it a queued frame never leaves. */
void can_tx_pump(void);
uint8_t can_tx_pending(void);

bool can_recv(struct can_frame_t *f);          /* false when the FIFO is empty */

/* Accept only these. bxCAN has 28 filter banks and using them is cheaper than
 * waking the CPU for every frame on a busy engine bus. */
void can_filter_block(uint16_t base, uint16_t mask);
void can_filter_id(uint16_t id);
void can_filter_accept_all(void);

bool     can_bus_off(void);
uint8_t  can_rx_errors(void);
uint8_t  can_tx_errors(void);
uint32_t can_actual_bitrate(void);             /* what the timing really produces */
uint16_t can_sample_point_permille(void);

#endif /* RCM_CANBUS_H */
