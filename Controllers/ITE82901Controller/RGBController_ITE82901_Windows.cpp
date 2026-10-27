/*---------------------------------------------------------*\
| RGBController_ITE82901_Windows.cpp                        |
|                                                           |
|   RGBController for ITE 82901 LED controller              |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <cstring>
#include "RGBController_ITE82901_Windows.h"

/**------------------------------------------------------------------*\
    @name ITE 82901
    @category Motherboard
    @type I2C
    @save :x:
    @direct :x:
    @effects :white_check_mark:
    @detectors DetectITE82901Controllers
    @comment Uses the ITE SPB peripheral kernel driver (ITE8853_<UID>)
        to reach the PCH I2C controller. LED effects use the IT8297 /
        IT8237 LED parameter protocol, raw I2C framing (0xE0 0x00, then
        command 0x20+N, apply 0x28). Settings are not saved to flash. Configure under "ITE82901Devices" in OpenRGB.json.
\*-------------------------------------------------------------------*/

/*---------------------------------------------------------*\
| OpenRGB mode list                                         |
\*---------------------------------------------------------*/
enum
{
    ITE82901_UI_OFF             = 0,
    ITE82901_UI_STATIC          = 1,
    ITE82901_UI_BREATHING       = 2,
    ITE82901_UI_FLASHING        = 3,
    ITE82901_UI_DOUBLE_FLASH    = 4,
    ITE82901_UI_SPECTRUM_CYCLE  = 5,
    ITE82901_UI_METEOR          = 6,
    ITE82901_UI_RAINBOW_WAVE    = 7,
    ITE82901_UI_RIPPLE          = 8,
    ITE82901_UI_WAVE            = 9,
};

/*---------------------------------------------------------*\
| bCtrl_value0 = 7: use all 7 colors of the pattern table   |
\*---------------------------------------------------------*/
#define ITE82901_ALL_PATTERN_COLORS     7

/*---------------------------------------------------------*\
| Convert OpenRGB color (0x00BBGGRR) to dwColor (0x00RRGGBB)|
\*---------------------------------------------------------*/
static unsigned int ToDeviceColor(RGBColor color)
{
    return((RGBGetRValue(color) << 16) | (RGBGetGValue(color) << 8) | RGBGetBValue(color));
}

/*---------------------------------------------------------*\
| Map speed slider (0 = slow .. 10 = fast) to milliseconds  |
\*---------------------------------------------------------*/
static unsigned short SpeedToMs(unsigned int speed, int slow_ms, int fast_ms)
{
    if(speed > ITE82901_SPEED_MAX)
    {
        speed = ITE82901_SPEED_MAX;
    }

    int ms = slow_ms + ((fast_ms - slow_ms) * (int)speed) / ITE82901_SPEED_MAX;

    return((unsigned short)ms);
}

static void AddMode
    (
    std::vector<mode>&  modes,
    const char*         mode_name,
    int                 value,
    unsigned int        flags,
    unsigned int        color_mode,
    unsigned int        color_count
    )
{
    mode new_mode;

    new_mode.name       = mode_name;
    new_mode.value      = value;
    new_mode.flags      = flags;
    new_mode.color_mode = color_mode;

    if(flags & MODE_FLAG_HAS_SPEED)
    {
        new_mode.speed_min  = ITE82901_SPEED_MIN;
        new_mode.speed_max  = ITE82901_SPEED_MAX;
        new_mode.speed      = ITE82901_SPEED_DEFAULT;
    }

    if(flags & MODE_FLAG_HAS_BRIGHTNESS)
    {
        new_mode.brightness_min = ITE82901_BRIGHTNESS_MIN;
        new_mode.brightness_max = ITE82901_BRIGHTNESS_MAX;
        new_mode.brightness     = ITE82901_BRIGHTNESS_MAX;
    }

    if(color_count > 0)
    {
        new_mode.colors_min = color_count;
        new_mode.colors_max = color_count;
        new_mode.colors.resize(color_count);

        /*-------------------------------------------------*\
        | Default colors: red, then blue                    |
        \*-------------------------------------------------*/
        new_mode.colors[0] = ToRGBColor(255, 0, 0);

        if(color_count > 1)
        {
            new_mode.colors[1] = ToRGBColor(0, 0, 255);
        }
    }

    modes.push_back(new_mode);
}

RGBController_ITE82901::RGBController_ITE82901(ITE82901Controller* controller_ptr)
{
    controller  = controller_ptr;

    name        = controller->GetName();
    vendor      = "ITE";
    type        = DEVICE_TYPE_MOTHERBOARD;
    description = "ITE 82901 LED Controller (PCH I2C)";
    location    = controller->GetLocation();

    const unsigned int color_flags  = MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_HAS_BRIGHTNESS;
    const unsigned int effect_flags = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS;

    AddMode(modes, "Off",             ITE82901_UI_OFF,            0,                                                                    MODE_COLORS_NONE,          0);
    AddMode(modes, "Static",          ITE82901_UI_STATIC,         color_flags,                                                          MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(modes, "Breathing",       ITE82901_UI_BREATHING,      color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(modes, "Flashing",        ITE82901_UI_FLASHING,       color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(modes, "Double Flashing", ITE82901_UI_DOUBLE_FLASH,   color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(modes, "Spectrum Cycle",  ITE82901_UI_SPECTRUM_CYCLE, effect_flags,                                                         MODE_COLORS_NONE,          0);
    AddMode(modes, "Meteor",          ITE82901_UI_METEOR,         color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(modes, "Rainbow Wave",    ITE82901_UI_RAINBOW_WAVE,   effect_flags,                                                         MODE_COLORS_NONE,          0);
    AddMode(modes, "Ripple",          ITE82901_UI_RIPPLE,         color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 2);
    AddMode(modes, "Wave",            ITE82901_UI_WAVE,           effect_flags,                                                         MODE_COLORS_NONE,          0);

    /*-----------------------------------------------------*\
    | Device state cannot be read back; start on Static     |
    \*-----------------------------------------------------*/
    active_mode = ITE82901_UI_STATIC;

    SetupZones();
}

RGBController_ITE82901::~RGBController_ITE82901()
{
    Shutdown();

    delete controller;
}

void RGBController_ITE82901::SetupZones()
{
    /*-----------------------------------------------------*\
    | One parameter applies to every LED in lZone_Sel, so   |
    | expose a single zone with a single logical LED        |
    \*-----------------------------------------------------*/
    zone all_zone;
    all_zone.name       = "All LEDs";
    all_zone.type       = ZONE_TYPE_SINGLE;
    all_zone.leds_min   = 1;
    all_zone.leds_max   = 1;
    all_zone.leds_count = 1;
    zones.push_back(all_zone);

    led all_led;
    all_led.name        = "All LEDs";
    all_led.value       = 0;
    leds.push_back(all_led);

    SetupColors();
}

void RGBController_ITE82901::DeviceUpdateLEDs()
{
    /*-----------------------------------------------------*\
    | No per-LED (Direct) mode; colors come from the mode   |
    \*-----------------------------------------------------*/
}

void RGBController_ITE82901::DeviceUpdateZoneLEDs(int /*zone*/)
{
    DeviceUpdateLEDs();
}

void RGBController_ITE82901::DeviceUpdateSingleLED(int /*led*/)
{
    DeviceUpdateLEDs();
}

/*---------------------------------------------------------*\
| Build gLedParameter for the active mode                   |
|   Timing values follow the IT8297 parameter document and  |
|   the ranges used by OpenRGB's Gigabyte IT8297 driver     |
\*---------------------------------------------------------*/
void RGBController_ITE82901::DeviceUpdateMode()
{
    const mode&             m      = modes[active_mode];
    ite82901_led_parameter  p;
    bool                    random = (m.color_mode == MODE_COLORS_RANDOM);

    memset(&p, 0, sizeof(p));

    p.max_brightness = (unsigned char)m.brightness;
    p.min_brightness = 0;

    if(m.colors.size() > 0)
    {
        p.color0 = ToDeviceColor(m.colors[0]);
    }

    if(m.colors.size() > 1)
    {
        p.color1 = ToDeviceColor(m.colors[1]);
    }

    switch(m.value)
    {
        /*-------------------------------------------------*\
        | Off: static black at 0% brightness                |
        \*-------------------------------------------------*/
        case ITE82901_UI_OFF:
            p.mode              = ITE82901_MODE_STATIC;
            p.max_brightness    = 0;
            p.color0            = 0;
            break;

        /*-------------------------------------------------*\
        | Static: only color-0 (bCtrl_value0 = 0)           |
        \*-------------------------------------------------*/
        case ITE82901_UI_STATIC:
            p.mode              = ITE82901_MODE_STATIC;
            break;

        /*-------------------------------------------------*\
        | Pulse: time0 fade on, time1 fade off, time2 hold  |
        \*-------------------------------------------------*/
        case ITE82901_UI_BREATHING:
            p.mode              = ITE82901_MODE_PULSE;
            p.time_base[0]      = SpeedToMs(m.speed, 2000, 400);
            p.time_base[1]      = p.time_base[0];
            p.time_base[2]      = 200;
            p.ctrl_value[0]     = random ? ITE82901_ALL_PATTERN_COLORS : 0;
            break;

        /*-------------------------------------------------*\
        | Flash: time0 on, time1 off, time2 cycle,          |
        | ctrl2 = flashes per cycle                         |
        \*-------------------------------------------------*/
        case ITE82901_UI_FLASHING:
            p.mode              = ITE82901_MODE_FLASH;
            p.time_base[0]      = 100;
            p.time_base[1]      = 100;
            p.time_base[2]      = SpeedToMs(m.speed, 2600, 600);
            p.ctrl_value[0]     = random ? ITE82901_ALL_PATTERN_COLORS : 0;
            p.ctrl_value[2]     = 1;
            break;

        case ITE82901_UI_DOUBLE_FLASH:
            p.mode              = ITE82901_MODE_FLASH;
            p.time_base[0]      = 100;
            p.time_base[1]      = 100;
            p.time_base[2]      = SpeedToMs(m.speed, 2800, 800);
            p.ctrl_value[0]     = random ? ITE82901_ALL_PATTERN_COLORS : 0;
            p.ctrl_value[2]     = 2;
            break;

        /*-------------------------------------------------*\
        | Color cycle: time0 change, time1 transition,      |
        | ctrl0 = number of pattern colors                  |
        \*-------------------------------------------------*/
        case ITE82901_UI_SPECTRUM_CYCLE:
            p.mode              = ITE82901_MODE_COLOR_CYCLE;
            p.time_base[0]      = SpeedToMs(m.speed, 2400, 300);
            p.time_base[1]      = (p.time_base[0] > 300) ? (unsigned short)(p.time_base[0] - 200) : 100;
            p.ctrl_value[0]     = ITE82901_ALL_PATTERN_COLORS;
            break;

        /*-------------------------------------------------*\
        | Meteor: time0 step, time1 tail, time2 color change|
        \*-------------------------------------------------*/
        case ITE82901_UI_METEOR:
            p.mode              = ITE82901_MODE_METEOR;
            p.time_base[0]      = SpeedToMs(m.speed, 200, 20);
            p.time_base[1]      = (unsigned short)(p.time_base[0] * 8);
            p.time_base[2]      = random ? 3000 : 0;
            p.ctrl_value[0]     = random ? ITE82901_ALL_PATTERN_COLORS : 0;
            break;

        /*-------------------------------------------------*\
        | Rainbow: time0 step, ctrl0 colors, ctrl1 pulse off|
        \*-------------------------------------------------*/
        case ITE82901_UI_RAINBOW_WAVE:
            p.mode              = ITE82901_MODE_RAINBOW;
            p.time_base[0]      = SpeedToMs(m.speed, 300, 30);
            p.ctrl_value[0]     = ITE82901_ALL_PATTERN_COLORS;
            p.ctrl_value[1]     = 0;
            break;

        /*-------------------------------------------------*\
        | Ripple: time0 step, time1 tail, time2 cycle,      |
        | ctrl0 = 0 uses color-0 and color-1                |
        \*-------------------------------------------------*/
        case ITE82901_UI_RIPPLE:
            p.mode              = ITE82901_MODE_RIPPLE;
            p.time_base[0]      = SpeedToMs(m.speed, 200, 20);
            p.time_base[1]      = (unsigned short)(p.time_base[0] * 4);
            p.time_base[2]      = 2000;
            p.ctrl_value[0]     = random ? ITE82901_ALL_PATTERN_COLORS : 0;
            break;

        /*-------------------------------------------------*\
        | Wave2: time0 speed, ctrl0 color distance          |
        \*-------------------------------------------------*/
        case ITE82901_UI_WAVE:
            p.mode              = ITE82901_MODE_WAVE2;
            p.time_base[0]      = SpeedToMs(m.speed, 500, 50);
            p.ctrl_value[0]     = 3;
            p.ctrl_value[1]     = 0;
            break;

        default:
            return;
    }

    controller->SetLedParameter(&p);
}
