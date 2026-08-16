/*
 * selftest_main.cpp -- a bring-up console on the USB-C port.
 *
 *     pio run -e selftest -t upload
 *
 * Replaces main.cpp in its own environment, so it can never ship by accident.
 *
 * WHY THIS EXISTS. The normal bring-up path needs a CAN adapter on the PC, and the
 * first hour with a new board is exactly when you least want a second unknown in the
 * loop. This build needs nothing but the USB-C cable that is already on the board: it
 * enumerates as a serial port and gives you a menu.
 *
 * It can prove, with no other hardware at all:
 *   - the board holds its own power on
 *   - the config DIP reads
 *   - the EEPROM answers, and survives a write
 *   - the CAN controller, its bit timing and its filters, via internal LOOPBACK
 *   - the IMU initialises and produces sane gravity
 *   - every one of the 21 channels drives, and what its sense line reads back
 *
 * The one thing it cannot prove is that the CAN transceiver talks to another node.
 * That needs a second node.
 */
#include <Arduino.h>
#include <IWatchdog.h>
#include "app.h"
#include "board.h"
#include "canbus.h"
#include "channels.h"
#include "chnames.h"
#include "config.h"
#include "imu.h"
#include "protocol.h"
#include "shiftreg.h"
#include "store.h"

/* app.h, satisfied locally -- the scheduler in main.cpp is not in this build. */
static bool outputs_live;
bool     app_outputs_live(void) { return outputs_live; }
void     app_set_outputs_live(bool v) { outputs_live = v; sr_outputs_enable(v); }
bool     app_eeprom_ok(void)    { return store_present(); }
uint16_t app_ignition_mv(void)
{
    const uint32_t mv = ((uint32_t)analogRead(PIN_IGN_SENSE) * 3300UL) / 4095UL;
    return (uint16_t)((mv * 1270UL) / 270UL);
}
bool app_ignition_on(void) { return app_ignition_mv() >= 6000; }

static bool can_ok;
static bool     was_wdg_reset;
static uint32_t live_at_ms;      /* millis() when the outputs came back up */

#define WDG_US 200000

static void tick_a_while(uint32_t ms)
{
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) { ch_tick(millis()); delay(5); }
}

/* --- individual checks ------------------------------------------------------ */

static void show_straps(void)
{
    Serial.println(F("\n-- straps (DIP, closed = on) --"));
    Serial.printf("  role        : %s\n", straps.keypad ? "KEYPAD" : "relay module");
    Serial.printf("  address     : %u\n", straps.address);
    Serial.printf("  node        : %u  (role is the top bit)\n", straps.node);
    Serial.printf("  force 500k  : %s\n", straps.force_500k ? "YES" : "no");
    Serial.printf("  publish IMU : %s\n", straps.publish_imu ? "YES" : "no");
    Serial.println(F("  Flip a switch and press 'd' again -- if nothing changes, the"));
    Serial.println(F("  DIP is not reaching PC0-PC4."));
}

static void test_eeprom(void)
{
    Serial.println(F("\n-- EEPROM (M95640) --"));
    if (!store_present()) {
        Serial.println(F("  NOT RESPONDING. Check U_EEP and its CS on PB12."));
        return;
    }
    Serial.println(F("  present"));

    /* Scratch area well clear of both config copies. */
    const uint16_t addr = 0x0800;
    uint8_t w[40], r[40];
    for (int i = 0; i < 40; i++) w[i] = (uint8_t)(0xA5 ^ i);
    bool ok = store_write(addr, w, sizeof(w)) && store_read(addr, r, sizeof(r));
    ok = ok && memcmp(w, r, sizeof(w)) == 0;
    /* 40 bytes from 0x800 spans two 32-byte pages, so this also proves the page
     * splitting -- an unsplit burst would wrap and come back scrambled. */
    Serial.printf("  40-byte write across a page boundary: %s\n", ok ? "ok" : "FAILED");

    Serial.printf("  stored config: %s\n", cfg_valid(&cfg) ? "valid" : "defaults in use");
    Serial.printf("  bitrate %lu, base id 0x%03X\n",
                  (unsigned long)cfg.can_bitrate, cfg.can_base_id);
}

static void test_can(void)
{
    Serial.println(F("\n-- CAN controller, internal loopback --"));
    const uint32_t rate = cfg_effective_bitrate();

    if (!can_begin_ex(rate, true)) {
        Serial.printf("  can_begin FAILED at %lu baud -- no exact bit timing from this"
                      " clock\n", (unsigned long)rate);
        return;
    }
    can_filter_accept_all();
    Serial.printf("  %lu baud, sample point %u.%u%%\n",
                  (unsigned long)can_actual_bitrate(),
                  can_sample_point_permille() / 10, can_sample_point_permille() % 10);

    const uint8_t payload[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4 };
    can_send(0x123, payload, 8);
    can_tx_pump();

    struct can_frame_t f;
    bool got = false;
    const uint32_t t0 = millis();
    while (millis() - t0 < 100 && !got) {
        can_tx_pump();
        got = can_recv(&f);
    }
    if (!got) {
        Serial.println(F("  NO LOOPBACK FRAME. The controller is not transmitting."));
    } else if (f.id != 0x123 || memcmp(f.data, payload, 8) != 0) {
        Serial.printf("  frame came back WRONG: id 0x%03X\n", f.id);
    } else {
        Serial.println(F("  loopback frame returned intact -- controller, timing and"));
        Serial.println(F("  filters are all good. This does NOT test the transceiver."));
    }
    Serial.printf("  errors rx %u tx %u, bus-off %s\n",
                  can_rx_errors(), can_tx_errors(), can_bus_off() ? "YES" : "no");

    /* Leave it in normal mode so the bench tool can be pointed at the board next. */
    can_ok = can_begin(rate);
    if (can_ok) proto_begin();
    Serial.printf("  back to normal mode at %lu baud: %s\n",
                  (unsigned long)rate, can_ok ? "ok" : "FAILED");
}

static void test_imu(void)
{
    Serial.println(F("\n-- IMU (BMI270) --"));
    if (!imu_ok() && !imu_begin()) {
        Serial.println(F("  init FAILED. The 8KB config upload did not take -- check"));
        Serial.println(F("  I2C1 on PB6/PB7 and that R_ADDR is fitted (address 0x68)."));
        return;
    }
    for (int i = 0; i < 5; i++) { imu_tick(); delay(50); }
    const float x = imu_accel(0), y = imu_accel(1), z = imu_accel(2);
    const float mag = sqrtf(x * x + y * y + z * z);
    Serial.printf("  accel  %+.3f %+.3f %+.3f g   |v| = %.3f\n", x, y, z, mag);
    Serial.printf("  gyro   %+.2f %+.2f %+.2f deg/s\n",
                  imu_gyro(0), imu_gyro(1), imu_gyro(2));
    /* Sitting still, the only acceleration is gravity, so the vector length is 1g
     * whatever way up the board is. That single number catches a dead axis, a wrong
     * range setting and a scaling mistake at once. */
    Serial.printf("  sitting still |v| should be ~1.00 g: %s\n",
                  (mag > 0.85f && mag < 1.15f) ? "ok" : "SUSPECT");
    Serial.println(F("  Whichever axis reads about -1 or +1 is pointing at the ground."));
    Serial.println(F("  Use that to set imu_map before trusting the MM5.10 output."));
}

static void show_channels(void)
{
    ch_tick(millis());
    const uint32_t cmd = ch_commanded(), raw = ch_sense_raw();
    const uint32_t fo = ch_fault_open(), fs = ch_fault_short();

    Serial.println(F("\n ch  function            mode   how        drive sense  verdict"));
    for (uint8_t i = 0; i < RCM_CHANNELS; i++) {
        const bool on = cmd >> i & 1, hi = raw >> i & 1;
        const char *v = "";
        if (fo >> i & 1)      v = "OPEN  <- blown fuse / no relay / broken wire";
        else if (fs >> i & 1) v = "SHORT <- low side not pulling down";
        else if (!on && hi)   v = "coil circuit intact";
        else if (on && !hi)   v = "driving";
        else if (!on && !hi)  v = "nothing connected, or still settling";

        const uint8_t md = cfg.ch[i].mode, fn = cfg.ch[i].func;
        Serial.printf(" %2u  %-19s %-6s %-10s %-5s %-5s  %s\n", i + 1u,
                      ch_func_name(fn),
                      md == CH_OUTPUT ? "out" : (md == CH_INPUT ? "in" : "unused"),
                      md == CH_OUTPUT ? ch_behaviour_name(cfg.ch[i].behaviour) : "-",
                      on ? "ON" : "off", hi ? "HIGH" : "low", v);

        /* A label that disagrees with the mode is a configuration mistake worth
         * shouting about. "Fuel pump" on an input channel will never do anything, and
         * it is very easy to stare past in a table of 21 rows. */
        if (!ch_func_matches_mode(fn, md))
            Serial.printf("     ^^ MISMATCH: %s is %s but the channel is %s\n",
                          ch_func_name(fn),
                          ch_func_is_input(fn) ? "an input" : "an output",
                          md == CH_OUTPUT ? "an output"
                                          : (md == CH_INPUT ? "an input" : "unused"));
    }
    Serial.printf("  aux %u%u%u   ignition %.1f V\n",
                  ch_aux() & 1, ch_aux() >> 1 & 1, ch_aux() >> 2 & 1,
                  app_ignition_mv() / 1000.0f);
}

static void walk_channels(void)
{
    Serial.println(F("\n-- channel walk --"));
    Serial.println(F("Each channel on for 2s in turn. Meter each terminal as it goes."));
    Serial.println(F("THIS is the check worth doing by hand: the unit tests verify the"));
    Serial.println(F("firmware against the netlist, and this verifies the netlist"));
    Serial.println(F("against the board you are holding.\n"));

    ch_all_off();
    tick_a_while(200);

    for (uint8_t i = 0; i < RCM_CHANNELS; i++) {
        ch_command(i, true);
        tick_a_while(100);
        const bool before = ch_sense_raw() >> i & 1;
        tick_a_while(1900);
        const bool after = ch_sense_raw() >> i & 1;
        Serial.printf("  ch %2u  driving   sense %s -> %s   %s\n", i + 1u,
                      before ? "HIGH" : "low", after ? "HIGH" : "low",
                      after ? "still high: nothing connected, or a 12V short"
                            : "pulled down");
        ch_command(i, false);
        tick_a_while(200);
    }
    ch_all_off();
    tick_a_while(100);
    Serial.println(F("\ndone. If a terminal lit up that was not the one named, the"));
    Serial.println(F("shift-register byte order is wrong -- check for an offset of 14."));
}

static void menu(void)
{
    Serial.println(F("\n=============== RCM self-test ==============="));
    Serial.printf("firmware %d.%d.%d, node %u\n\n",
                  RCM_FW_MAJOR, RCM_FW_MINOR, RCM_FW_PATCH, straps.node);
    Serial.println(F("  d  straps / DIP switch"));
    Serial.println(F("  e  EEPROM"));
    Serial.println(F("  c  CAN controller (internal loopback)"));
    Serial.println(F("  i  IMU"));
    Serial.println(F("  s  channel + sense table"));
    Serial.println(F("  w  walk all 21 channels, 2s each"));
    Serial.println(F("  1  toggle a channel (then type its number and Enter)"));
    Serial.println(F("  0  all off"));
    Serial.println(F("  o  toggle the output enable (595 OE)"));
    Serial.println(F("  A  run d/e/c/i in one go"));
    Serial.println(F("  W  force a watchdog reset, and time the output dropout"));
    Serial.println(F("  ?  this menu"));
}

/* --- setup / loop ----------------------------------------------------------- */

void setup(void)
{
    /* Identical opening moves to main.cpp, and for identical reasons. Hold our own
     * power on first; get a zeroed frame latched into the 595s before anything is
     * allowed to enable them. */
    pinMode(PIN_LATCH_HOLD, OUTPUT);
    digitalWrite(PIN_LATCH_HOLD, HIGH);
    sr_begin();

    pinMode(PIN_LED1, OUTPUT);
    pinMode(PIN_LED2, OUTPUT);
    pinMode(PIN_IGN_SENSE, INPUT_ANALOG);
    analogReadResolution(12);

    cfg_read_straps();
    store_begin();
    cfg_load();
    proto_sanitise_base();
    ch_begin();

    /* Mirror the real firmware exactly, so the recovery time measured here is the one
     * the board would actually have in a car. Both paths adopt failsafe_state and go
     * live immediately. */
    was_wdg_reset = IWatchdog.isReset(true);
    ch_apply_failsafe();
    ch_tick(millis());
    app_set_outputs_live(true);
    live_at_ms = millis();

    Serial.begin(115200);
    /* No watchdog in this build: it is an interactive console and half the commands
     * deliberately block for seconds at a time. */
    const uint32_t t0 = millis();
    while (!Serial && millis() - t0 < 5000) delay(10);
    delay(200);
    menu();

    if (was_wdg_reset) {
        Serial.println(F("\n*** recovered from a WATCHDOG RESET ***"));
        Serial.printf("outputs were re-enabled %lu ms after reset, with failsafe_state "
                      "applied.\n", (unsigned long)live_at_ms);
        Serial.println(F("That figure excludes the time before main() -- crystal and PLL"));
        Serial.println(F("startup -- which a scope on any channel would include. Expect"));
        Serial.println(F("the true gap to be a few ms longer."));
    }
    Serial.print(F("\n> "));
}

void loop(void)
{
    static uint32_t last_tick;
    if (millis() - last_tick >= 5) { last_tick = millis(); ch_tick(millis()); }
    if (can_ok) { can_tx_pump(); proto_poll(millis()); }

    digitalWrite(PIN_LED1, (millis() % 1000) < 500);
    digitalWrite(PIN_LED2, (ch_fault_open() || ch_fault_short())
                           && (millis() % 400) < 200);

    if (!Serial.available()) return;
    const int c = Serial.read();
    if (c == '\r' || c == '\n') return;
    Serial.println((char)c);

    switch (c) {
    case 'd': show_straps();   break;
    case 'e': test_eeprom();   break;
    case 'c': test_can();      break;
    case 'i': test_imu();      break;
    case 's': show_channels(); break;
    case 'w': walk_channels(); break;
    case '0': ch_all_off(); Serial.println(F("all off")); break;
    case 'o':
        app_set_outputs_live(!app_outputs_live());
        Serial.printf("outputs %s\n", app_outputs_live() ? "LIVE" : "high-impedance");
        break;
    case '1': {
        Serial.print(F("channel 1-21: "));
        const uint32_t t0 = millis();
        int n = 0;
        while (millis() - t0 < 10000) {
            if (!Serial.available()) continue;
            const int k = Serial.read();
            if (k >= '0' && k <= '9') { n = n * 10 + (k - '0'); Serial.print((char)k); }
            else break;
        }
        Serial.println();
        if (n >= 1 && n <= RCM_CHANNELS) {
            const bool now = !(ch_commanded() >> (n - 1) & 1);
            ch_command((uint8_t)(n - 1), now);
            tick_a_while(150);
            Serial.printf("channel %d %s, sense reads %s\n", n, now ? "ON" : "off",
                          (ch_sense_raw() >> (n - 1) & 1) ? "HIGH" : "low");
        } else {
            Serial.println(F("out of range"));
        }
        break;
    }
    case 'A':
        show_straps(); test_eeprom(); test_can(); test_imu(); show_channels();
        break;
    case 'W':
        /* Deliberately hang so the watchdog fires. The board comes back and reports how
         * long its outputs were down -- which is the number the README claims and the
         * only honest way to check it. Drive a channel first so you can watch it on a
         * scope or hear the relay. */
        Serial.println(F("hanging on purpose -- the watchdog will reset the board."));
        Serial.println(F("Reconnect the serial port; it will report the recovery time."));
        Serial.flush();
        IWatchdog.begin(WDG_US);
        for (;;) { }
        break;
    default:
        menu();
        break;
    }
    Serial.print(F("\n> "));
}
