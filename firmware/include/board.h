/*
 * board.h -- RCM pin map and board constants.
 *
 * GENERATED FROM THE BOARD, NOT TYPED. Every pin below is lifted from gen_spec.py's
 * PINMAP, which is the same table that produced the schematic and hence the copper. If
 * the hardware changes, regenerate this rather than editing it -- a firmware pin map
 * that has drifted from the netlist is a whole afternoon of confusion.
 */
#ifndef RCM_BOARD_H
#define RCM_BOARD_H

/* ---- channels ------------------------------------------------------------
 * 21 universal channels, 7 per TPL7407L driver, behind a shift-register chain.
 * Each channel is an output AND an input at the same time: the sense divider is
 * permanently connected, so a channel can be read while it is being driven.
 * That is what makes fuse detection free -- see README.md.
 */
#define RCM_CHANNELS          21
#define RCM_TILES              3
#define RCM_CH_PER_TILE        7

/* ---- shift register chains ----------------------------------------------
 * OUT: 3 x 74HC595, 8 bits each, but only QA..QG (7) drive a channel. QH unused.
 * IN : 3 x 74HC165, 8 bits each: 7 channel senses + 1 AUX input on the spare bit.
 * Both chains share SCK. Shift out 24 bits, latch, shift in 24 bits.
 */
#define RCM_SR_OUT_BITS       24
#define RCM_SR_IN_BITS        24
#define RCM_AUX_INPUTS         3

/* ---- SPI-ish bit-bang / hardware SPI pins -------------------------------- */
#define PIN_SR_SCK            PA5      /* shared clock, both chains */
#define PIN_SR_MOSI           PA7      /* -> 595 data in  */
#define PIN_SR_MISO           PA6      /* <- 165 data out */

/* ---- config DIP (positions 2-6; 1 is passive CAN termination) -------------
 * Internal pull-ups, switch shorts to ground: CLOSED reads 0.
 * These carry what cannot be fixed over the bus once wrong -- get the bitrate
 * or the address wrong and the node is simply silent.
 */
#define PIN_CFG_ROLE          PC0      /* 0 = keypad, 1 = relay module */
#define PIN_CFG_ADDR0         PC1     
#define PIN_CFG_ADDR1         PC2     
#define PIN_CFG_BAUD          PC3      /* 0 = 1 Mbps, 1 = 500 kbps (rusEFI default) */
#define PIN_CFG_IMU_EN        PC4      /* publish IMU frames; only ONE board per car */

/* ---- ignition latch -------------------------------------------------------
 * The board holds its own power up after ignition off so it can shut down
 * cleanly. LATCH_HOLD must be asserted early in boot or the board drops dead.
 */
#define PIN_LATCH_HOLD        PB10    
#define PIN_IGN_SENSE         PA0      /* divider off the ignition feed */

/* ---- CAN, IMU, EEPROM ---------------------------------------------------- */
#define PIN_CAN_RX            PB8     
#define PIN_CAN_TX            PB9     
#define PIN_IMU_INT1          PA1     
#define PIN_IMU_SCL           PB6     
#define PIN_IMU_SDA           PB7     
#define IMU_I2C_ADDR          0x68   /* SDO strapped low by R_ADDR (0R, fitted) */

/* ---- status ------------------------------------------------------------- */
#define PIN_LED1              PA8      /* green -- runs dim, see DESIGN.md */
#define PIN_LED2              PB5      /* red */

/* ---- CAN bitrates -------------------------------------------------------- */
#define RCM_BAUD_500K         500000u
#define RCM_BAUD_1M          1000000u

#endif /* RCM_BOARD_H */
