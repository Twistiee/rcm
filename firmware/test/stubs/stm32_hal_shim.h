/*
 * stm32_hal_shim.h -- just enough bxCAN for canbus.cpp to compile and run on a PC.
 *
 * canbus.cpp was the one module with no host coverage, which is awkward given it
 * contains the newest code (the transmit queue) and the classic bug magnet (filter
 * register packing, where an 11-bit id has to be shifted into bits 31..21 of a bank).
 *
 * The model is deliberately small but faithful on the things being tested: three
 * transmit mailboxes with controllable availability, a receive FIFO, and an error
 * status register. It is NOT a simulation of the CAN protocol.
 */
#ifndef RCM_TEST_HAL_SHIM_H
#define RCM_TEST_HAL_SHIM_H

#include <stdint.h>
#include <string.h>

#define HAL_OK      0
#define HAL_ERROR   1
#define ENABLE      1
#define DISABLE     0

typedef int HAL_StatusTypeDef;
typedef int FunctionalState;

/* --- GPIO (accepted and recorded, nothing more) ----------------------------- */
#define GPIO_PIN_8                (1u << 8)
#define GPIO_PIN_9                (1u << 9)
#define GPIO_MODE_AF_PP           2
#define GPIO_NOPULL               0
#define GPIO_SPEED_FREQ_VERY_HIGH 3
#define GPIO_AF9_CAN1             9

typedef struct { uint32_t Pin, Mode, Pull, Speed, Alternate; } GPIO_InitTypeDef;
typedef struct { int dummy; } GPIO_TypeDef;
extern GPIO_TypeDef *const GPIOB;

#define __HAL_RCC_GPIOB_CLK_ENABLE()  do { hal_sim.gpiob_clocked = true; } while (0)
#define __HAL_RCC_CAN1_CLK_ENABLE()   do { hal_sim.can1_clocked  = true; } while (0)

/* --- CAN -------------------------------------------------------------------- */
#define CAN_MODE_NORMAL       0
#define CAN_MODE_LOOPBACK     1
#define CAN_SJW_1TQ           0
#define CAN_BTR_TS1_Pos       16
#define CAN_BTR_TS2_Pos       20
#define CAN_ID_STD            0
#define CAN_ID_EXT            1
#define CAN_RTR_DATA          0
#define CAN_RX_FIFO0          0
#define CAN_FILTERMODE_IDMASK 0
#define CAN_FILTERSCALE_32BIT 1

#define CAN_ESR_BOFF     (1u << 2)
#define CAN_ESR_REC_Pos  24
#define CAN_ESR_TEC_Pos  16
#define CAN_ESR_REC      (0xFFu << CAN_ESR_REC_Pos)
#define CAN_ESR_TEC      (0xFFu << CAN_ESR_TEC_Pos)

typedef struct { volatile uint32_t ESR; } CAN_TypeDef;
extern CAN_TypeDef *const CAN1;

typedef struct {
    uint32_t Prescaler, Mode, SyncJumpWidth, TimeSeg1, TimeSeg2;
    FunctionalState TimeTriggeredMode, AutoBusOff, AutoWakeUp, AutoRetransmission;
    FunctionalState ReceiveFifoLocked, TransmitFifoPriority;
} CAN_InitTypeDef;

typedef struct { CAN_TypeDef *Instance; CAN_InitTypeDef Init; } CAN_HandleTypeDef;

typedef struct {
    uint32_t StdId, ExtId, IDE, RTR, DLC;
    FunctionalState TransmitGlobalTime;
} CAN_TxHeaderTypeDef;

typedef struct {
    uint32_t StdId, ExtId, IDE, RTR, DLC, Timestamp, FilterMatchIndex;
} CAN_RxHeaderTypeDef;

typedef struct {
    uint32_t FilterIdHigh, FilterIdLow, FilterMaskIdHigh, FilterMaskIdLow;
    uint32_t FilterFIFOAssignment, FilterBank, FilterMode, FilterScale;
    FunctionalState FilterActivation;
    uint32_t SlaveStartFilterBank;
} CAN_FilterTypeDef;

/* --- the model -------------------------------------------------------------- */
#define HAL_SIM_SENT_MAX 64
#define HAL_SIM_RX_MAX   16
#define HAL_SIM_BANKS    8

struct hal_sim_frame { uint32_t id; uint32_t ide; uint8_t len; uint8_t data[8]; };

struct hal_sim_t {
    uint32_t pclk1;
    bool     gpiob_clocked, can1_clocked, started;
    int      init_result;

    /* Transmit. free_mailboxes is what the code sees; the test moves it to say
     * "the wire is busy" and then "a mailbox freed up". */
    int                  free_mailboxes;
    struct hal_sim_frame sent[HAL_SIM_SENT_MAX];
    int                  sent_count;

    struct hal_sim_frame rx[HAL_SIM_RX_MAX];
    int                  rx_count;

    CAN_FilterTypeDef banks[HAL_SIM_BANKS];
    bool              bank_used[HAL_SIM_BANKS];
    int               bank_count;

    uint32_t esr;
};

extern struct hal_sim_t hal_sim;

void     hal_sim_reset(uint32_t pclk1);
bool     hal_sim_accepts(uint16_t std_id);   /* would the installed banks let it in? */
void     hal_sim_rx_push(uint32_t id, uint32_t ide, const uint8_t *d, uint8_t len);
uint32_t HAL_RCC_GetPCLK1Freq(void);
void     HAL_GPIO_Init(GPIO_TypeDef *, GPIO_InitTypeDef *);
HAL_StatusTypeDef HAL_CAN_Init(CAN_HandleTypeDef *);
HAL_StatusTypeDef HAL_CAN_Start(CAN_HandleTypeDef *);
HAL_StatusTypeDef HAL_CAN_ConfigFilter(CAN_HandleTypeDef *, CAN_FilterTypeDef *);
uint32_t HAL_CAN_GetTxMailboxesFreeLevel(CAN_HandleTypeDef *);
HAL_StatusTypeDef HAL_CAN_AddTxMessage(CAN_HandleTypeDef *, CAN_TxHeaderTypeDef *,
                                       uint8_t *, uint32_t *);
uint32_t HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef *, uint32_t fifo);
HAL_StatusTypeDef HAL_CAN_GetRxMessage(CAN_HandleTypeDef *, uint32_t fifo,
                                       CAN_RxHeaderTypeDef *, uint8_t *);

#endif /* RCM_TEST_HAL_SHIM_H */
