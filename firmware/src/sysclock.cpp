/*
 * sysclock.cpp -- run the core from the 8 MHz crystal, not the internal RC oscillator.
 *
 * Until this existed the board ran on the STM32duino variant's default
 * SystemClock_Config(), which feeds the PLL from HSI -- the internal 16 MHz RC -- and
 * the crystal fitted as Y1 was never used at all. HSE_VALUE in platformio.ini only
 * tells the HAL how fast the crystal is; it does not select it.
 *
 * HSI is trimmed to +/-1 % at 25 C and drifts with temperature, and CAN's bit timing is
 * derived from this clock. Measured on the keypad against a PC: +1.3 % fast on one day,
 * +3.5 % on another, with the board's CAN transmit error count swinging between 0 and
 * ~240 and whole seconds of nothing getting through. It looked like a wiring fault and
 * was chased as one for an hour. A crystal is good to ~50 ppm.
 *
 * The configuration is the variant's own with exactly two numbers changed. The crystal
 * is half the HSI frequency, so both PLL input dividers halve -- PLLM 8 -> 4 and
 * PLLSAIM 16 -> 8 -- and every clock downstream is identical: 180 MHz core, 45 MHz
 * APB1 for CAN, 48 MHz for USB.
 *
 * If the crystal will not start, fall back to HSI rather than to Error_Handler(). A
 * board with marginal CAN timing is far better than a board that hangs at power-up and
 * takes the car's ignition feed with it. clock_on_crystal() reports which happened, and
 * STATUS carries it so a failed crystal is visible from the bus.
 */
#include <Arduino.h>
#include "sysclock.h"

static bool on_crystal;

bool clock_on_crystal(void) { return on_crystal; }

static bool config(bool hse)
{
    RCC_OscInitTypeDef osc = {};
    RCC_ClkInitTypeDef clk = {};
    RCC_PeriphCLKInitTypeDef per = {};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    if (hse) {
        osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
        osc.HSEState       = RCC_HSE_ON;
        osc.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
        osc.PLL.PLLM       = 4;            /* 8 MHz / 4  = 2 MHz into the PLL */
    } else {
        osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
        osc.HSIState            = RCC_HSI_ON;
        osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
        osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
        osc.PLL.PLLM            = 8;       /* 16 MHz / 8 = 2 MHz into the PLL */
    }
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLN     = 180;                /* 360 MHz VCO */
    osc.PLL.PLLP     = RCC_PLLP_DIV2;
    osc.PLL.PLLQ     = 8;
    osc.PLL.PLLR     = 2;                  /* 180 MHz SYSCLK */
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) return false;

    if (HAL_PWREx_EnableOverDrive() != HAL_OK) return false;

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                       | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLRCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;    /* 45 MHz -- CAN */
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) return false;

    per.PeriphClockSelection = RCC_PERIPHCLK_SDIO | RCC_PERIPHCLK_CLK48;
    per.PLLSAI.PLLSAIM = hse ? 8 : 16;     /* 1 MHz into the SAI PLL either way */
    per.PLLSAI.PLLSAIN = 192;
    per.PLLSAI.PLLSAIQ = 2;
    per.PLLSAI.PLLSAIP = RCC_PLLSAIP_DIV4; /* 48 MHz -- USB */
    per.PLLSAIDivQ     = 1;
    per.Clk48ClockSelection = RCC_CLK48CLKSOURCE_PLLSAIP;
    per.SdioClockSelection  = RCC_SDIOCLKSOURCE_CLK48;
    return HAL_RCCEx_PeriphCLKConfig(&per) == HAL_OK;
}

/* Overrides the variant's WEAK default. C linkage, because that is what the core
 * calls. */
extern "C" void SystemClock_Config(void)
{
    on_crystal = config(true);
    if (on_crystal) return;

    /* The crystal did not start. HAL_RCC_OscConfig() leaves HSI running underneath,
     * so this is a clean second attempt rather than a recovery from a half state. */
    if (!config(false)) Error_Handler();
}
