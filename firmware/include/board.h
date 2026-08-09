/*
 * board.h -- RCM pin map and board constants.
 *
 * Every pin here is taken from gen_spec.py's PINMAP -- the same table that produced the
 * schematic and therefore the copper. Do not "tidy" these by hand: a firmware pin map that
 * has drifted from the netlist is a miserable afternoon with a multimeter.
 */
#ifndef RCM_BOARD_H
#define RCM_BOARD_H

/* ---- channels -------------------------------------------------------------
 * 21 universal channels, 7 per TPL7407L. Every channel is an output AND an input
 * simultaneously -- the sense divider never disconnects, so a channel can be read
 * while it is being driven. That is what makes fuse detection free.
 */
#define RCM_CHANNELS          21
#define RCM_TILES              3
#define RCM_CH_PER_TILE        7
#define RCM_AUX_INPUTS         3    /* J_AUX, on the spare bit of each 165 */

/* ---- shift register chains ------------------------------------------------
 * OUT: 3x 74HC595 daisy-chained, 8 bits each. QA..QG drive channels; QH unused.
 * IN : 3x 74HC165 daisy-chained, 8 bits each: 7 channel senses + 1 AUX.
 * Shared clock. PA5/6/7 are SPI1, so both chains can run on hardware SPI.
 */
#define RCM_SR_BYTES           3    /* 24 bits each way */

#define PIN_SR_SCK            PA5   /* SPI1_SCK  -- shared by both chains        */
#define PIN_SR_MISO           PA6   /* SPI1_MISO <- 165 chain data out           */
#define PIN_SR_MOSI           PA7   /* SPI1_MOSI -> 595 chain data in            */
#define PIN_SR_RCLK           PB0   /* 595 storage latch: pulse HIGH to publish  */
#define PIN_SR_PL             PB1   /* 165 parallel load, ACTIVE LOW: pulse to sample */
#define PIN_SR_OE_N           PB2   /* 595 output enable, ACTIVE LOW             */

/* SR_OE_N has a 10k pull-UP (R_OE), so the 595 outputs are high-impedance from
 * the instant power arrives until firmware deliberately drives this low. That is
 * what stops every relay clacking on during boot. Do NOT enable outputs until the
 * shift registers have been loaded with a known state. */

/* ---- config DIP (positions 2-6; position 1 is passive CAN termination) -----
 * Internal pull-ups; the switch shorts to ground. CLOSED reads 0.
 */
#define PIN_CFG_ROLE          PC0   /* closed = keypad, open = relay module      */
#define PIN_CFG_ADDR0         PC1
#define PIN_CFG_ADDR1         PC2   /* ADDR0+1 -> 4 node addresses per role      */
#define PIN_CFG_BAUD          PC3   /* closed = force 500k (recovery), open = use stored */
#define PIN_CFG_IMU_EN        PC4   /* closed = publish IMU; only ONE board per car */

/* ---- ignition latch -------------------------------------------------------
 * The board holds its own power on so it can shut down cleanly after ignition off.
 * LATCH_HOLD must be driven HIGH within the first few ms of main() or the board
 * switches itself off the moment the ignition signal drops.
 */
#define PIN_LATCH_HOLD        PB10
#define PIN_IGN_SENSE         PA0   /* analog: divider off the ignition feed     */

/* ---- CAN ------------------------------------------------------------------ */
#define PIN_CAN_RX            PB8   /* CAN1 */
#define PIN_CAN_TX            PB9

/* ---- IMU (BMI270, I2C1) --------------------------------------------------- */
#define PIN_IMU_SCL           PB6
#define PIN_IMU_SDA           PB7
#define PIN_IMU_INT1          PA1
#define IMU_I2C_ADDR          0x68  /* SDO pulled low by R_ADDR (0R, fitted)     */

/* ---- EEPROM (M95640, SPI2) ------------------------------------------------ */
#define PIN_EEP_CS            PB12
#define PIN_EEP_SCK           PB13
#define PIN_EEP_MISO          PB14
#define PIN_EEP_MOSI          PB15

/* ---- status LEDs ---------------------------------------------------------- */
#define PIN_LED1              PA8   /* green -- dim by design, see DESIGN.md     */
#define PIN_LED2              PB5   /* red                                       */

/* ---- CAN bitrates --------------------------------------------------------- */
#define RCM_BAUD_DEFAULT      500000UL   /* rusEFI default; uaDASH is limited to this */
#define RCM_BAUD_RECOVERY     500000UL   /* what CFG_BAUD closed forces               */

#endif /* RCM_BOARD_H */
