/*
 * chnames.cpp -- human names for channel functions and behaviours.
 *
 * The function byte is mostly a label, and a label nobody can read is not worth
 * storing. This is what turns "CH07" into "Fuel pump" in the self-test console.
 *
 * Kept as a lookup rather than an array indexed by the enum, because the enum is
 * deliberately grouped in blocks of 32 with gaps for extension -- a dense table would
 * be mostly holes, and every future insertion would renumber somebody's saved config.
 */
#include "chnames.h"
#include "config.h"

struct name_entry { uint8_t id; const char *name; };

static const struct name_entry FUNCS[] = {
    { FN_NONE,              "unassigned" },
    /* engine and drivetrain */
    { FN_IGNITION,          "Ignition (RUN)" },
    { FN_STARTER,           "Starter" },
    { FN_MAIN_RELAY,        "Main relay" },
    { FN_FUEL_PUMP,         "Fuel pump" },
    { FN_FUEL_PUMP_2,       "Fuel pump 2" },
    { FN_FAN_1,             "Fan 1" },
    { FN_FAN_2,             "Fan 2" },
    { FN_WATER_PUMP,        "Water pump" },
    { FN_INTERCOOLER_PUMP,  "Intercooler pump" },
    { FN_BOOST_SOLENOID,    "Boost solenoid" },
    { FN_NITROUS,           "Nitrous" },
    /* lighting */
    { FN_HEADLIGHT_LOW,     "Headlight low" },
    { FN_HEADLIGHT_HIGH,    "Headlight high" },
    { FN_TAIL,              "Tail" },
    { FN_BRAKE_LIGHT,       "Brake light" },
    { FN_REVERSE_LIGHT,     "Reverse light" },
    { FN_INDICATOR_L,       "Indicator L" },
    { FN_INDICATOR_R,       "Indicator R" },
    { FN_FOG_FRONT,         "Fog front" },
    { FN_FOG_REAR,          "Fog rear" },
    { FN_RAIN_LIGHT,        "Rain light" },
    { FN_INTERIOR_LIGHT,    "Interior light" },
    { FN_WORK_LIGHT,        "Work light" },
    /* body */
    { FN_HORN,              "Horn" },
    { FN_WIPER,             "Wiper" },
    { FN_WIPER_FAST,        "Wiper fast" },
    { FN_WASHER,            "Washer" },
    { FN_HEATED_SCREEN,     "Heated screen" },
    { FN_HEATED_SEAT,       "Heated seat" },
    { FN_AC_CLUTCH,         "A/C clutch" },
    { FN_LINE_LOCK,         "Line lock" },
    /* inputs */
    { FN_IN_BRAKE,          "Brake pedal" },
    { FN_IN_ENGINE_RUN,     "Engine running" },
    { FN_IN_CLUTCH,         "Clutch" },
    { FN_IN_HANDBRAKE,      "Handbrake" },
    { FN_IN_REVERSE,        "Reverse switch" },
    { FN_IN_DOOR,           "Door" },
    { FN_IN_BONNET,         "Bonnet" },
    { FN_IN_TRACTION_CTL,   "Traction control" },
    { FN_IN_LAUNCH_ARM,     "Launch arm" },
    { FN_IN_PIT_LIMITER,    "Pit limiter" },
    { FN_IN_MAP_SELECT,     "Map select" },
    { FN_IN_HORN,           "Horn button" },
    { FN_IN_HEADLIGHT,      "Headlight switch" },
    { FN_IN_INDICATOR_L,    "Indicator L switch" },
    { FN_IN_INDICATOR_R,    "Indicator R switch" },
    { FN_IN_HAZARD,         "Hazard switch" },
    { FN_IN_WIPER,          "Wiper switch" },
    { FN_IN_WASHER,         "Washer switch" },
    { FN_IN_USER,           "Button" },
};

const char *ch_func_name(uint8_t func)
{
    for (unsigned i = 0; i < sizeof(FUNCS) / sizeof(FUNCS[0]); i++)
        if (FUNCS[i].id == func) return FUNCS[i].name;
    return "?";
}

const char *ch_behaviour_name(uint8_t b)
{
    switch (b) {
    case OUT_STEADY:    return "steady";
    case OUT_FLASH:     return "flash";
    case OUT_PULSE:     return "pulse";
    case OUT_DELAY_OFF: return "delay-off";
    default:            return "?";
    }
}

/* Inputs live at FN_IN_BRAKE and above, which is what makes a mismatch checkable. */
bool ch_func_is_input(uint8_t func)
{
    return func >= FN_IN_BRAKE;
}

bool ch_func_matches_mode(uint8_t func, uint8_t mode)
{
    if (func == FN_NONE) return true;                 /* unlabelled is always fine */
    return ch_func_is_input(func) ? (mode == CH_INPUT) : (mode == CH_OUTPUT);
}
