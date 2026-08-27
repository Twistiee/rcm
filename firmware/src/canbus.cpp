/*
 * canbus.cpp -- bxCAN via the STM32 HAL.
 *
 * ===========================================================================
 * BIT TIMING
 * ===========================================================================
 * A CAN bit is divided into (1 + TS1 + TS2) time quanta, and one time quantum is
 * (BRP+1) / PCLK1 seconds. So:
 *
 *      bitrate = PCLK1 / (BRP * (1 + TS1 + TS2))
 *
 * There are usually several (BRP, TS1, TS2) triples that hit a given bitrate
 * exactly, and they are NOT equivalent -- they differ in where the bit gets
 * sampled. Everything on the bus has to agree on that within a few percent or long
 * cables and slow transceivers start producing errors that look like noise.
 * CiA's recommendation for automotive rates is 87.5%, which is what we solve for.
 *
 * PCLK1 is read from the HAL rather than assumed. The STM32duino core's clock setup
 * is not ours to depend on, and a firmware that quietly runs CAN at 437.5k because
 * somebody changed the PLL is exactly the sort of bug this avoids.
 *
 * Exact rates only. If no (BRP, TS1, TS2) divides out exactly, can_begin() fails
 * rather than settling for "close" -- a 1% bitrate error works on the bench and
 * fails in the rain, and a hard failure at boot is a far better outcome.
 */
#include <Arduino.h>
#include <math.h>
#include "canbus.h"

static CAN_HandleTypeDef hcan;
static bool      up;
static uint32_t  actual_rate;
static uint16_t  sample_permille;
static uint8_t   next_bank;
static bool      default_filter;   /* bank 0 still holds can_begin's accept-all */

/* Software transmit queue -- see the note above can_send(). Declared here so
 * can_begin() can reset it. */
#define TXQ_LEN 8
/* The queue carries a WIDER id than can_frame_t, because one thing this board sends is
 * a 29-bit extended frame (rusEFI's bench-test command block at 0x770000). can_frame_t
 * stays 11-bit: everything RECEIVED here is standard, the filters are standard-only,
 * and widening the public struct would touch every caller for one transmit path. */
struct txq_entry_t {
    uint32_t id;
    bool     ext;
    uint8_t  len;
    uint8_t  data[8];
};
static struct txq_entry_t txq[TXQ_LEN];
static uint8_t txq_head, txq_tail, txq_count;

struct timing_t {
    bool     found;
    uint32_t brp, ts1, ts2;
    uint16_t sp_permille;
};

/* bxCAN limits: BRP 1..1024, TS1 1..16, TS2 1..8, so 3..25 quanta per bit.
 * Below 8 quanta the sample point cannot be placed anywhere sensible.
 *
 * constexpr so the results can be asserted at compile time -- see below. That is
 * worth a little awkwardness (no fabsf, no output parameters) because there is no
 * other way to check this without an oscilloscope and a second CAN node. */
static constexpr timing_t solve_timing(uint32_t pclk, uint32_t bitrate)
{
    timing_t best = { false, 0, 0, 0, 0 };
    float best_err = 1e9f;

    for (uint32_t ntq = 25; ntq >= 8; ntq--) {
        const uint32_t div = bitrate * ntq;
        if (div == 0 || pclk % div) continue;          /* exact rates only */
        const uint32_t p = pclk / div;
        if (p < 1 || p > 1024) continue;

        int t1 = (int)((0.875f * (float)ntq) + 0.5f) - 1;
        if (t1 > 16) t1 = 16;
        int t2 = (int)ntq - 1 - t1;
        while (t2 > 8) { t2--; t1++; }                 /* push the slack into TS1 */
        while (t2 < 1) { t2++; t1--; }
        if (t1 < 1 || t1 > 16 || t2 < 1 || t2 > 8) continue;

        const float sp  = (float)(1 + t1) / (float)ntq;
        const float d   = sp - 0.875f;
        const float err = d < 0.0f ? -d : d;
        /* Strictly less-than, and we count DOWN from 25, so on a tie the larger
         * quantum count wins -- more quanta means finer resynchronisation. */
        if (err < best_err) {
            best_err = err;
            best.found = true;
            best.brp = p; best.ts1 = (uint32_t)t1; best.ts2 = (uint32_t)t2;
            best.sp_permille = (uint16_t)(sp * 1000.0f + 0.5f);
        }
    }
    return best;
}

/* ---------------------------------------------------------------------------
 * Bit timing, checked at compile time.
 *
 * PCLK1 on this board is 45MHz (180MHz SYSCLK / 4), which is what the STM32duino
 * core sets up for an 8MHz HSE. 42MHz is checked too because that is what you get
 * from the other common F4 clock tree, and someone will eventually build this for
 * a different board.
 *
 * The properties that matter are (a) the rate comes out EXACT and (b) the sample
 * point lands within a couple of percent of 87.5%, because that is what every
 * other node on an automotive bus will be using.
 * --------------------------------------------------------------------------- */
static constexpr bool rate_exact(uint32_t pclk, uint32_t want)
{
    return solve_timing(pclk, want).found
        && pclk / (solve_timing(pclk, want).brp * (1 + solve_timing(pclk, want).ts1
                                                     + solve_timing(pclk, want).ts2)) == want;
}
static constexpr bool sp_sane(uint32_t pclk, uint32_t want)
{
    return solve_timing(pclk, want).sp_permille >= 820
        && solve_timing(pclk, want).sp_permille <= 900;
}

static_assert(rate_exact(45000000, 500000) && sp_sane(45000000, 500000), "45MHz 500k");
static_assert(rate_exact(45000000, 250000) && sp_sane(45000000, 250000), "45MHz 250k");
static_assert(rate_exact(45000000, 125000) && sp_sane(45000000, 125000), "45MHz 125k");
static_assert(rate_exact(45000000, 1000000) && sp_sane(45000000, 1000000), "45MHz 1M");
static_assert(rate_exact(42000000, 500000) && sp_sane(42000000, 500000), "42MHz 500k");
static_assert(rate_exact(42000000, 250000) && sp_sane(42000000, 250000), "42MHz 250k");
static_assert(rate_exact(42000000, 1000000) && sp_sane(42000000, 1000000), "42MHz 1M");
/* 45MHz cannot produce 800k exactly (45e6/800e3 = 56.25), and can_begin() must say
 * so rather than quietly running the bus 1% out. */
static_assert(!solve_timing(45000000, 800000).found, "800k must FAIL cleanly on 45MHz");

bool can_begin(uint32_t bitrate) { return can_begin_ex(bitrate, false); }

bool can_begin_ex(uint32_t bitrate, bool loopback)
{
    up = false;
    next_bank = 0;
    txq_head = txq_tail = txq_count = 0;

    const uint32_t pclk = HAL_RCC_GetPCLK1Freq();
    const timing_t t = solve_timing(pclk, bitrate);
    if (!t.found) return false;
    const uint32_t brp = t.brp, ts1 = t.ts1, ts2 = t.ts2;
    sample_permille = t.sp_permille;
    actual_rate = pclk / (brp * (1 + ts1 + ts2));

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_CAN1_CLK_ENABLE();

    /* PB8 = CAN1_RX, PB9 = CAN1_TX, both alternate function 9. Configured directly
     * rather than through the Arduino pin layer, because we are driving the
     * peripheral directly too and mixing the two is how pins end up half-owned. */
    GPIO_InitTypeDef g = {};
    g.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF9_CAN1;
    HAL_GPIO_Init(GPIOB, &g);

    hcan.Instance = CAN1;
    hcan.Init.Prescaler     = brp;
    hcan.Init.Mode          = loopback ? CAN_MODE_LOOPBACK : CAN_MODE_NORMAL;
    hcan.Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan.Init.TimeSeg1      = (ts1 - 1) << CAN_BTR_TS1_Pos;
    hcan.Init.TimeSeg2      = (ts2 - 1) << CAN_BTR_TS2_Pos;
    hcan.Init.TimeTriggeredMode  = DISABLE;
    /* Recover from bus-off automatically. In a car the alternative is a node that
     * goes permanently silent because something brushed the bus once. */
    hcan.Init.AutoBusOff         = ENABLE;
    hcan.Init.AutoWakeUp         = DISABLE;
    hcan.Init.AutoRetransmission = ENABLE;
    /* Overwrite the oldest frame rather than dropping the newest. Everything we
     * receive is a command or a state update: the latest one is the one that
     * matters, and a stale command is worse than no command. */
    hcan.Init.ReceiveFifoLocked  = DISABLE;
    hcan.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan) != HAL_OK) return false;

    can_filter_accept_all();      /* replaced by the caller's real filters */
    if (HAL_CAN_Start(&hcan) != HAL_OK) return false;

    up = true;
    return true;
}

/* --- filters ---------------------------------------------------------------
 * 32-bit mask mode, one bank each. A standard 11-bit id sits in bits 31..21 of the
 * bank register, which is the detail everyone gets wrong the first time.
 */
static void set_filter(uint8_t bank, uint16_t id, uint16_t mask)
{
    CAN_FilterTypeDef f = {};
    f.FilterBank           = bank;
    f.FilterMode           = CAN_FILTERMODE_IDMASK;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterIdHigh         = (uint16_t)(id << 5);
    f.FilterIdLow          = 0;
    f.FilterMaskIdHigh     = (uint16_t)(mask << 5);
    f.FilterMaskIdLow      = 0x0006;   /* also require IDE=0 and RTR=0: standard
                                        * data frames only, so an extended-id
                                        * bench-test frame cannot alias onto us */
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterActivation     = ENABLE;
    f.SlaveStartFilterBank = 14;
    HAL_CAN_ConfigFilter(&hcan, &f);
}

/* The FIRST explicit filter overwrites bank 0.
 *
 * can_begin() leaves an accept-all filter there so that a caller which installs nothing
 * is merely noisy rather than deaf. But an accept-all bank makes every LATER bank
 * pointless -- the filters are ORed, so bank 0 keeps waving the whole bus through and
 * the 3-deep FIFO can overrun with traffic meant for other nodes, dropping our own
 * commands. Rewinding to bank 0 here replaces it with the first real filter. */
static void claim_bank_zero(void)
{
    if (default_filter) { next_bank = 0; default_filter = false; }
}

/* Rewind the bank allocator so the whole filter set can be rebuilt from scratch.
 * Needed because a filter can now change while the board is running: pointing the
 * ignition at an ECU RPM id installs a filter for it. Without a rewind, reinstalling
 * appends a second copy of every filter and walks next_bank towards the 28-bank
 * ceiling, after which new filters are silently dropped and the board goes deaf to
 * exactly the frame it was just told to listen for. */
void can_filters_reset(void)
{
    next_bank = 0;
    default_filter = false;
}

void can_filter_block(uint16_t base, uint16_t mask)
{
    claim_bank_zero();
    set_filter(next_bank++, base, mask);
}

void can_filter_id(uint16_t id)
{
    claim_bank_zero();
    set_filter(next_bank++, id, 0x7FF);
}

void can_filter_accept_all(void)
{
    CAN_FilterTypeDef f = {};
    f.FilterBank           = 0;
    f.FilterMode           = CAN_FILTERMODE_IDMASK;
    f.FilterScale          = CAN_FILTERSCALE_32BIT;
    f.FilterFIFOAssignment = CAN_RX_FIFO0;
    f.FilterActivation     = ENABLE;
    f.SlaveStartFilterBank = 14;
    HAL_CAN_ConfigFilter(&hcan, &f);
    next_bank = 1;
    default_filter = true;
}

/* --- transmit ---------------------------------------------------------------
 * bxCAN has THREE transmit mailboxes, and one broadcast cycle emits four frames.
 * Handing straight to the hardware would therefore drop the fourth every single
 * time -- silently, and always the same one. A small software queue behind the
 * mailboxes fixes that; can_tx_pump() drains it as they free up.
 *
 * Eight deep is two full broadcast cycles. If it ever fills, the bus is not moving
 * and dropping frames is the correct thing to do -- state broadcasts are absolute,
 * not incremental, so the next one supersedes whatever was lost.
 */
static bool to_mailbox(const struct txq_entry_t *f)
{
    if (HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0) return false;

    CAN_TxHeaderTypeDef h = {};
    if (f->ext) { h.ExtId = f->id; h.IDE = CAN_ID_EXT; }
    else        { h.StdId = f->id; h.IDE = CAN_ID_STD; }
    h.RTR   = CAN_RTR_DATA;
    h.DLC   = f->len;
    h.TransmitGlobalTime = DISABLE;

    uint32_t mailbox;
    return HAL_CAN_AddTxMessage(&hcan, &h, (uint8_t *)f->data, &mailbox) == HAL_OK;
}

void can_tx_pump(void)
{
    while (txq_count && to_mailbox(&txq[txq_tail])) {
        txq_tail = (uint8_t)((txq_tail + 1) % TXQ_LEN);
        txq_count--;
    }
}

bool can_send(uint16_t id, const uint8_t *data, uint8_t len)
{
    return can_send_frame(id, false, data, len);
}

/* 29-bit id. Nothing here receives extended frames -- the filters are standard-only --
 * so this is transmit only, and deliberately a separate entry point rather than a flag
 * on can_send(): every other frame this board sends is 11-bit and should stay that way
 * by default. */
bool can_send_ext(uint32_t id, const uint8_t *data, uint8_t len)
{
    if (id > 0x1FFFFFFFu) return false;
    return can_send_frame(id, true, data, len);
}

bool can_send_frame(uint32_t id, bool ext, const uint8_t *data, uint8_t len)
{
    if (!up || len > 8) return false;

    struct txq_entry_t f;
    f.id = id;
    f.ext = ext;
    f.len = len;
    for (uint8_t i = 0; i < 8; i++) f.data[i] = i < len ? data[i] : 0;

    /* Keep ordering: never jump the queue, or a state frame could overtake the
     * command that produced it. */
    if (txq_count == 0 && to_mailbox(&f)) return true;

    if (txq_count >= TXQ_LEN) {
        can_tx_pump();
        if (txq_count >= TXQ_LEN) return false;
    }
    txq[txq_head] = f;
    txq_head = (uint8_t)((txq_head + 1) % TXQ_LEN);
    txq_count++;
    return true;
}

uint8_t can_tx_pending(void) { return txq_count; }

bool can_recv(struct can_frame_t *f)
{
    if (!up || HAL_CAN_GetRxFifoFillLevel(&hcan, CAN_RX_FIFO0) == 0) return false;

    CAN_RxHeaderTypeDef h;
    uint8_t buf[8];
    if (HAL_CAN_GetRxMessage(&hcan, CAN_RX_FIFO0, &h, buf) != HAL_OK) return false;
    if (h.IDE != CAN_ID_STD) return false;

    f->id  = (uint16_t)h.StdId;
    f->len = (uint8_t)h.DLC;
    for (uint8_t i = 0; i < 8; i++) f->data[i] = buf[i];
    return true;
}

bool can_bus_off(void)
{
    if (!up) return true;
    return (hcan.Instance->ESR & CAN_ESR_BOFF) != 0;
}

uint8_t can_rx_errors(void)
{
    if (!up) return 0;
    return (uint8_t)((hcan.Instance->ESR & CAN_ESR_REC) >> CAN_ESR_REC_Pos);
}

uint8_t can_tx_errors(void)
{
    if (!up) return 0;
    return (uint8_t)((hcan.Instance->ESR & CAN_ESR_TEC) >> CAN_ESR_TEC_Pos);
}

uint32_t can_actual_bitrate(void)        { return actual_rate; }
uint16_t can_sample_point_permille(void) { return sample_permille; }
