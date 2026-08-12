#include "stm32_hal_shim.h"

struct hal_sim_t hal_sim;

static CAN_TypeDef  can1_instance;
static GPIO_TypeDef gpiob_instance;
CAN_TypeDef  *const CAN1  = &can1_instance;
GPIO_TypeDef *const GPIOB = &gpiob_instance;

void hal_sim_reset(uint32_t pclk1)
{
    memset(&hal_sim, 0, sizeof(hal_sim));
    hal_sim.pclk1 = pclk1;
    hal_sim.free_mailboxes = 3;      /* bxCAN has three */
    can1_instance.ESR = 0;
}

void hal_sim_rx_push(uint32_t id, uint32_t ide, const uint8_t *d, uint8_t len)
{
    if (hal_sim.rx_count >= HAL_SIM_RX_MAX) return;
    struct hal_sim_frame *f = &hal_sim.rx[hal_sim.rx_count++];
    f->id = id; f->ide = ide; f->len = len;
    memset(f->data, 0, 8);
    memcpy(f->data, d, len > 8 ? 8 : len);
}

uint32_t HAL_RCC_GetPCLK1Freq(void) { return hal_sim.pclk1; }
void HAL_GPIO_Init(GPIO_TypeDef *, GPIO_InitTypeDef *) {}

HAL_StatusTypeDef HAL_CAN_Init(CAN_HandleTypeDef *h)
{
    h->Instance->ESR = 0;
    return hal_sim.init_result;
}

HAL_StatusTypeDef HAL_CAN_Start(CAN_HandleTypeDef *)
{
    hal_sim.started = true;
    return hal_sim.init_result;
}

/* Indexed by FilterBank, as the hardware is -- so reconfiguring a bank OVERWRITES it
 * rather than adding another, which is the behaviour the accept-all replacement relies
 * on. An append-only model hid that entirely. */
HAL_StatusTypeDef HAL_CAN_ConfigFilter(CAN_HandleTypeDef *, CAN_FilterTypeDef *f)
{
    if (f->FilterBank >= HAL_SIM_BANKS) return HAL_ERROR;
    hal_sim.banks[f->FilterBank] = *f;
    hal_sim.bank_used[f->FilterBank] = (f->FilterActivation == ENABLE);
    if ((int)f->FilterBank >= hal_sim.bank_count) hal_sim.bank_count = (int)f->FilterBank + 1;
    return HAL_OK;
}

/* Would this filter set accept a given standard id? The whole point of the banks. */
bool hal_sim_accepts(uint16_t id)
{
    for (int i = 0; i < HAL_SIM_BANKS; i++) {
        if (!hal_sim.bank_used[i]) continue;
        const uint16_t fid  = (uint16_t)(hal_sim.banks[i].FilterIdHigh >> 5);
        const uint16_t mask = (uint16_t)(hal_sim.banks[i].FilterMaskIdHigh >> 5);
        if (((id ^ fid) & mask) == 0) return true;
    }
    return false;
}

uint32_t HAL_CAN_GetTxMailboxesFreeLevel(CAN_HandleTypeDef *)
{
    return (uint32_t)(hal_sim.free_mailboxes < 0 ? 0 : hal_sim.free_mailboxes);
}

HAL_StatusTypeDef HAL_CAN_AddTxMessage(CAN_HandleTypeDef *, CAN_TxHeaderTypeDef *h,
                                       uint8_t *data, uint32_t *mailbox)
{
    if (hal_sim.free_mailboxes <= 0) return HAL_ERROR;
    hal_sim.free_mailboxes--;
    *mailbox = 0;
    if (hal_sim.sent_count < HAL_SIM_SENT_MAX) {
        struct hal_sim_frame *f = &hal_sim.sent[hal_sim.sent_count++];
        f->id = h->StdId; f->ide = h->IDE; f->len = (uint8_t)h->DLC;
        memcpy(f->data, data, 8);
    }
    return HAL_OK;
}

uint32_t HAL_CAN_GetRxFifoFillLevel(CAN_HandleTypeDef *, uint32_t)
{
    return (uint32_t)hal_sim.rx_count;
}

HAL_StatusTypeDef HAL_CAN_GetRxMessage(CAN_HandleTypeDef *, uint32_t,
                                       CAN_RxHeaderTypeDef *h, uint8_t *data)
{
    if (hal_sim.rx_count == 0) return HAL_ERROR;
    struct hal_sim_frame f = hal_sim.rx[0];
    for (int i = 1; i < hal_sim.rx_count; i++) hal_sim.rx[i - 1] = hal_sim.rx[i];
    hal_sim.rx_count--;

    memset(h, 0, sizeof(*h));
    h->StdId = f.id;
    h->IDE   = f.ide;
    h->DLC   = f.len;
    memcpy(data, f.data, 8);
    return HAL_OK;
}
