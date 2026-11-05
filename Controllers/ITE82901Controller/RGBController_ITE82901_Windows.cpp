/*---------------------------------------------------------*\
| RGBController_ITE82901_Windows.cpp                        |
|                                                           |
|   RGBController for ITE 82901 LED controller              |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "RGBController_ITE82901_Windows.h"
#include "LogManager.h"

/**------------------------------------------------------------------*\
    @name ITE 82901
    @category Motherboard
    @type I2C
    @save :white_check_mark:
    @direct :white_check_mark:
    @effects :white_check_mark:
    @detectors DetectITE82901Controllers
    @comment Windows x64 only. Uses the ITE I2C device driver (SPB
        peripheral, device path ITE8853_<UID>) to reach the PCH I2C
        controller; the device is skipped if the driver is not installed. LED effects use the IT8297 /
        IT8237 LED parameter protocol, raw I2C framing (0xE0 0x00, then
        command 0x20+N, apply 0x28, save 0x5E). The stored effect is
        read back with 0x20 at startup. Direct mode streams RGB to
        strip-0 with 0x58 (enable 0x32, length 0x34); the LED count is
        "strip0_led_count" in OpenRGB.json. The single PWM LED is a
        second zone with its own zone modes (Static / Breathing /
        Flashing / Rainbow, no Direct) using gLedParameter[1] (cmd 0x21,
        apply bit 1).
        Configure under "ITE82901Devices" in OpenRGB.json.
\*-------------------------------------------------------------------*/

/*---------------------------------------------------------*\
| OpenRGB mode list                                         |
\*---------------------------------------------------------*/
enum
{
    ITE82901_UI_DIRECT          = 0,
    ITE82901_UI_OFF             = 1,
    ITE82901_UI_STATIC          = 2,
    ITE82901_UI_BREATHING       = 3,
    ITE82901_UI_FLASHING        = 4,
    ITE82901_UI_DOUBLE_FLASH    = 5,
    ITE82901_UI_SPECTRUM_CYCLE  = 6,
    ITE82901_UI_METEOR          = 7,
    ITE82901_UI_RAINBOW_WAVE    = 8,
    ITE82901_UI_RIPPLE          = 9,
    ITE82901_UI_WAVE            = 10,
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

/*---------------------------------------------------------*\
| Inverse of SpeedToMs: milliseconds -> speed slider        |
\*---------------------------------------------------------*/
static unsigned int MsToSpeed(int ms, int slow_ms, int fast_ms)
{
    if(slow_ms == fast_ms)
    {
        return(ITE82901_SPEED_DEFAULT);
    }

    int speed = ((ms - slow_ms) * ITE82901_SPEED_MAX + (fast_ms - slow_ms) / 2) / (fast_ms - slow_ms);

    if(speed < ITE82901_SPEED_MIN)
    {
        speed = ITE82901_SPEED_MIN;
    }

    if(speed > ITE82901_SPEED_MAX)
    {
        speed = ITE82901_SPEED_MAX;
    }

    return((unsigned int)speed);
}

/*---------------------------------------------------------*\
| Convert dwColor (0x00RRGGBB) to OpenRGB color             |
\*---------------------------------------------------------*/
static RGBColor FromDeviceColor(unsigned int color)
{
    return(ToRGBColor((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF));
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
    new_mode.flags      = flags | MODE_FLAG_MANUAL_SAVE;
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

RGBController_ITE82901::RGBController_ITE82901
    (
    std::shared_ptr<ITE82901Controller> controller_ptr,
    int                                 strip_parameter_slot,
    unsigned long long                  strip_zone_mask,
    bool                                pwm_enabled,
    int                                 pwm_parameter_slot,
    unsigned long long                  pwm_zone_mask
    )
{
    controller  = controller_ptr;
    strip_slot  = strip_parameter_slot;
    strip_mask  = strip_zone_mask;
    pwm_present = pwm_enabled;
    pwm_slot    = pwm_parameter_slot;
    pwm_mask    = pwm_zone_mask;

    name        = controller->GetName();
    vendor      = "ITE";
    type        = DEVICE_TYPE_MOTHERBOARD;
    description = "ITE 82901 LED Controller (PCH I2C)";
    location    = controller->GetLocation();

    /*-----------------------------------------------------*\
    | Device modes: Direct + strip hardware effects         |
    \*-----------------------------------------------------*/
    AddStripModes(modes, true);

    active_mode = FindModeIndex(modes, ITE82901_UI_STATIC);

    SetupZones();

    LoadModeFromDevice();
}

RGBController_ITE82901::~RGBController_ITE82901()
{
    Shutdown();
}

/*---------------------------------------------------------*\
| Strip effects (optionally with Direct)                    |
\*---------------------------------------------------------*/
void RGBController_ITE82901::AddStripModes(std::vector<mode>& list, bool with_direct)
{
    const unsigned int color_flags  = MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_HAS_BRIGHTNESS;
    const unsigned int effect_flags = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS;

    if(with_direct)
    {
        /*-------------------------------------------------*\
        | Direct: per-LED colors streamed by OpenRGB /      |
        | Effects plugin (e.g. audio effects). Not saved.   |
        \*-------------------------------------------------*/
        AddMode(list, "Direct",          ITE82901_UI_DIRECT,         MODE_FLAG_HAS_PER_LED_COLOR,                                          MODE_COLORS_PER_LED,       0);
        list.back().flags &= ~MODE_FLAG_MANUAL_SAVE;
    }

    AddMode(list, "Off",             ITE82901_UI_OFF,            0,                                                                    MODE_COLORS_NONE,          0);
    AddMode(list, "Static",          ITE82901_UI_STATIC,         color_flags,                                                          MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(list, "Breathing",       ITE82901_UI_BREATHING,      color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(list, "Flashing",        ITE82901_UI_FLASHING,       color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(list, "Double Flashing", ITE82901_UI_DOUBLE_FLASH,   color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(list, "Spectrum Cycle",  ITE82901_UI_SPECTRUM_CYCLE, effect_flags,                                                         MODE_COLORS_NONE,          0);
    AddMode(list, "Meteor",          ITE82901_UI_METEOR,         color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(list, "Rainbow Wave",    ITE82901_UI_RAINBOW_WAVE,   effect_flags,                                                         MODE_COLORS_NONE,          0);
    AddMode(list, "Ripple",          ITE82901_UI_RIPPLE,         color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 2);
    AddMode(list, "Wave",            ITE82901_UI_WAVE,           effect_flags,                                                         MODE_COLORS_NONE,          0);
}

/*---------------------------------------------------------*\
| PWM LED effects: Static / Breathing / Flashing / Rainbow  |
\*---------------------------------------------------------*/
void RGBController_ITE82901::AddPwmModes(std::vector<mode>& list)
{
    const unsigned int color_flags  = MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_HAS_BRIGHTNESS;
    const unsigned int effect_flags = MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS;

    AddMode(list, "Static",          ITE82901_UI_STATIC,         color_flags,                                                          MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(list, "Breathing",       ITE82901_UI_BREATHING,      color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(list, "Flashing",        ITE82901_UI_FLASHING,       color_flags | MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_RANDOM_COLOR,      MODE_COLORS_MODE_SPECIFIC, 1);
    AddMode(list, "Rainbow",         ITE82901_UI_RAINBOW_WAVE,   effect_flags,                                                         MODE_COLORS_NONE,          0);
}

/*---------------------------------------------------------*\
| Index of the mode with the given UI value in a mode list  |
\*---------------------------------------------------------*/
int RGBController_ITE82901::FindModeIndex(const std::vector<mode>& list, int value)
{
    for(std::size_t mode_idx = 0; mode_idx < list.size(); mode_idx++)
    {
        if(list[mode_idx].value == value)
        {
            return((int)mode_idx);
        }
    }

    return(-1);
}

/*---------------------------------------------------------*\
| Device modes the PWM LED can follow ("Follow Device Mode")|
\*---------------------------------------------------------*/
bool RGBController_ITE82901::PwmSupports(int value)
{
    return((value == ITE82901_UI_OFF)
        || (value == ITE82901_UI_STATIC)
        || (value == ITE82901_UI_BREATHING)
        || (value == ITE82901_UI_FLASHING)
        || (value == ITE82901_UI_RAINBOW_WAVE));
}

void RGBController_ITE82901::SetupZones()
{
    /*-----------------------------------------------------*\
    | Zone 0: LED strip (linear, Direct capable)            |
    \*-----------------------------------------------------*/
    unsigned int led_count = (unsigned int)controller->GetStripLedCount();

    zone strip_zone;
    strip_zone.name       = "LED Strip";
    strip_zone.type       = ZONE_TYPE_LINEAR;
    strip_zone.leds_min   = led_count;
    strip_zone.leds_max   = led_count;
    strip_zone.leds_count = led_count;
    zones.push_back(strip_zone);

    for(unsigned int led_idx = 0; led_idx < led_count; led_idx++)
    {
        led new_led;
        new_led.name  = "LED Strip LED " + std::to_string(led_idx + 1);
        new_led.value = led_idx;
        leds.push_back(new_led);
    }

    /*-----------------------------------------------------*\
    | Zone 1: single PWM LED with its own zone modes        |
    \*-----------------------------------------------------*/
    if(pwm_present)
    {
        zone pwm_zone;
        pwm_zone.name       = "PWM LED";
        pwm_zone.type       = ZONE_TYPE_SINGLE;
        pwm_zone.leds_min   = 1;
        pwm_zone.leds_max   = 1;
        pwm_zone.leds_count = 1;
        zones.push_back(pwm_zone);

        led pwm_led;
        pwm_led.name        = "PWM LED";
        pwm_led.value       = 0;
        leds.push_back(pwm_led);

        /*-------------------------------------------------*\
        | Per-zone modes: the strip zone can follow the     |
        | device mode or use its own effect; the PWM zone   |
        | starts on its own Static mode                     |
        \*-------------------------------------------------*/
        AddStripModes(zones[ITE82901_ZONE_STRIP].modes, false);
        zones[ITE82901_ZONE_STRIP].active_mode = -1;

        AddPwmModes(zones[ITE82901_ZONE_PWM].modes);
        zones[ITE82901_ZONE_PWM].active_mode = FindModeIndex(zones[ITE82901_ZONE_PWM].modes, ITE82901_UI_STATIC);
    }

    SetupColors();
}

/*---------------------------------------------------------*\
| Direct mode: send the strip zone LEDs as R, G, B bytes    |
| (the PWM LED has no Direct control)                       |
\*---------------------------------------------------------*/
void RGBController_ITE82901::DeviceUpdateLEDs()
{
    if(modes[active_mode].value != ITE82901_UI_DIRECT)
    {
        return;
    }

    unsigned int strip_leds = zones[ITE82901_ZONE_STRIP].leds_count;

    std::vector<unsigned char> rgb(strip_leds * 3);

    for(unsigned int i = 0; (i < strip_leds) && (i < colors.size()); i++)
    {
        rgb[(i * 3) + 0] = RGBGetRValue(colors[i]);
        rgb[(i * 3) + 1] = RGBGetGValue(colors[i]);
        rgb[(i * 3) + 2] = RGBGetBValue(colors[i]);
    }

    controller->SetStripColors(rgb.data(), (int)strip_leds);
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
| Device mode changed ("Entire Device")                     |
\*---------------------------------------------------------*/
void RGBController_ITE82901::DeviceUpdateMode()
{
    /*-----------------------------------------------------*\
    | Direct: enable per-LED control of the strip, then     |
    | push the colors. The PWM LED keeps its own effect.    |
    \*-----------------------------------------------------*/
    if(modes[active_mode].value == ITE82901_UI_DIRECT)
    {
        if(controller->SetDirectMode(true))
        {
            DeviceUpdateLEDs();
        }

        return;
    }

    /*-----------------------------------------------------*\
    | Hardware effect: always refresh the strip (it leaves  |
    | Direct if needed); refresh the PWM LED only when it   |
    | follows the device mode                               |
    \*-----------------------------------------------------*/
    ApplyZone(ITE82901_ZONE_STRIP);

    if(pwm_present && (zones[ITE82901_ZONE_PWM].active_mode < 0))
    {
        ApplyZone(ITE82901_ZONE_PWM);
    }
}

/*---------------------------------------------------------*\
| Zone mode changed (zone selected in the Zone menu)        |
\*---------------------------------------------------------*/
void RGBController_ITE82901::DeviceUpdateZoneMode(int zone)
{
    /*-----------------------------------------------------*\
    | While the device is in Direct, Direct owns the strip  |
    | (same behavior as other per-zone drivers)             |
    \*-----------------------------------------------------*/
    if((zone == ITE82901_ZONE_STRIP) && (modes[active_mode].value == ITE82901_UI_DIRECT))
    {
        return;
    }

    ApplyZone(zone);
}

/*---------------------------------------------------------*\
| Mode in effect for a zone: its own zone mode, or the      |
| device mode when set to "Follow Device Mode" (-1)         |
\*---------------------------------------------------------*/
const mode* RGBController_ITE82901::EffectiveZoneMode(int zone)
{
    if((zone >= 0) && (zone < (int)zones.size()))
    {
        int zone_mode = zones[zone].active_mode;

        if((zone_mode >= 0) && (zone_mode < (int)zones[zone].modes.size()))
        {
            return(&zones[zone].modes[zone_mode]);
        }
    }

    if((active_mode >= 0) && (active_mode < (int)modes.size()))
    {
        return(&modes[active_mode]);
    }

    return(NULL);
}

/*---------------------------------------------------------*\
| Send the effective mode of one zone to its parameter      |
|   strip : gLedParameter[strip_slot] (0x20), leaves Direct |
|   PWM   : gLedParameter[pwm_slot]   (0x21), never Direct  |
\*---------------------------------------------------------*/
void RGBController_ITE82901::ApplyZone(int zone)
{
    const mode* m = EffectiveZoneMode(zone);

    if(m == NULL || m->value == ITE82901_UI_DIRECT)
    {
        return;
    }

    if(zone == ITE82901_ZONE_PWM)
    {
        if(!pwm_present)
        {
            return;
        }

        /*-------------------------------------------------*\
        | Following a device mode the PWM LED cannot do:    |
        | keep its current effect                           |
        \*-------------------------------------------------*/
        if((zones[ITE82901_ZONE_PWM].active_mode < 0) && !PwmSupports(m->value))
        {
            LOG_VERBOSE("[ITE82901] PWM LED cannot follow device mode \"%s\", unchanged", m->name.c_str());
            return;
        }
    }

    ite82901_led_parameter p;

    if(!BuildParameter(*m, &p))
    {
        return;
    }

    if(zone == ITE82901_ZONE_PWM)
    {
        controller->SetLedParameter(pwm_slot, pwm_mask, false, &p);
    }
    else
    {
        controller->SetLedParameter(strip_slot, strip_mask, true, &p);
    }
}

/*---------------------------------------------------------*\
| Build gLedParameter for a mode                            |
|   Timing values follow the IT8297 parameter document and  |
|   the ranges used by OpenRGB's Gigabyte IT8297 driver     |
\*---------------------------------------------------------*/
bool RGBController_ITE82901::BuildParameter(const mode& m, ite82901_led_parameter* out)
{
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
            return(false);
    }

    *out = p;
    return(true);
}

void RGBController_ITE82901::DeviceSaveMode()
{
    controller->SaveToFlash();
}

/*---------------------------------------------------------*\
| Read both parameters back (0x20 strip, 0x21 PWM) and set  |
| the UI: strip -> device mode, PWM -> PWM zone mode.       |
| Nothing is written to the LEDs here.                      |
\*---------------------------------------------------------*/
void RGBController_ITE82901::LoadModeFromDevice()
{
    ite82901_led_parameter p;

    if(controller->ReadLedParameter(strip_slot, &p))
    {
        int idx = ApplyParameterToModes(p, modes);

        if(idx >= 0)
        {
            active_mode = idx;
            LOG_INFO("[ITE82901] LED Strip: stored effect bMode_Sel %d -> \"%s\"", p.mode, modes[idx].name.c_str());
        }
        else
        {
            LOG_INFO("[ITE82901] LED Strip: stored bMode_Sel %d has no matching OpenRGB mode, UI starts on Static", p.mode);
        }
    }
    else
    {
        LOG_INFO("[ITE82901] LED Strip: could not read the stored effect, UI starts on Static");
    }

    if(!pwm_present)
    {
        return;
    }

    if(controller->ReadLedParameter(pwm_slot, &p))
    {
        int idx = ApplyParameterToModes(p, zones[ITE82901_ZONE_PWM].modes);

        if(idx >= 0)
        {
            zones[ITE82901_ZONE_PWM].active_mode = idx;
            LOG_INFO("[ITE82901] PWM LED: stored effect bMode_Sel %d -> \"%s\"", p.mode, zones[ITE82901_ZONE_PWM].modes[idx].name.c_str());
        }
        else
        {
            LOG_INFO("[ITE82901] PWM LED: stored bMode_Sel %d has no matching OpenRGB mode, UI starts on Static", p.mode);
        }
    }
    else
    {
        LOG_INFO("[ITE82901] PWM LED: could not read the stored effect, UI starts on Static");
    }
}

/*---------------------------------------------------------*\
| Map a read-back gLedParameter onto a mode list (colors,   |
| speed, brightness, random). Returns the mode index or -1. |
\*---------------------------------------------------------*/
int RGBController_ITE82901::ApplyParameterToModes(const ite82901_led_parameter& p, std::vector<mode>& list)
{
    int          ui_mode = -1;
    unsigned int speed   = ITE82901_SPEED_DEFAULT;
    bool         random  = (p.ctrl_value[0] == ITE82901_ALL_PATTERN_COLORS);

    switch(p.mode)
    {
        case ITE82901_MODE_STATIC:
            ui_mode = (p.max_brightness == 0 || p.color0 == 0) ? ITE82901_UI_OFF : ITE82901_UI_STATIC;
            random  = false;
            break;

        case ITE82901_MODE_PULSE:
            ui_mode = ITE82901_UI_BREATHING;
            speed   = MsToSpeed(p.time_base[0], 2000, 400);
            break;

        case ITE82901_MODE_FLASH:
            if(p.ctrl_value[2] >= 2)
            {
                ui_mode = ITE82901_UI_DOUBLE_FLASH;
                speed   = MsToSpeed(p.time_base[2], 2800, 800);
            }
            else
            {
                ui_mode = ITE82901_UI_FLASHING;
                speed   = MsToSpeed(p.time_base[2], 2600, 600);
            }
            break;

        case ITE82901_MODE_COLOR_CYCLE:
            ui_mode = ITE82901_UI_SPECTRUM_CYCLE;
            speed   = MsToSpeed(p.time_base[0], 2400, 300);
            break;

        case ITE82901_MODE_METEOR:
            ui_mode = ITE82901_UI_METEOR;
            speed   = MsToSpeed(p.time_base[0], 200, 20);
            break;

        case ITE82901_MODE_RAINBOW:
            ui_mode = ITE82901_UI_RAINBOW_WAVE;
            speed   = MsToSpeed(p.time_base[0], 300, 30);
            break;

        case ITE82901_MODE_RIPPLE:
            ui_mode = ITE82901_UI_RIPPLE;
            speed   = MsToSpeed(p.time_base[0], 200, 20);
            break;

        case ITE82901_MODE_WAVE2:
            ui_mode = ITE82901_UI_WAVE;
            speed   = MsToSpeed(p.time_base[0], 500, 50);
            break;

        default:
            break;
    }


    /*-----------------------------------------------------*\
    | A list without "Off" (PWM LED) shows Off as Static    |
    \*-----------------------------------------------------*/
    if(ui_mode == ITE82901_UI_OFF && FindModeIndex(list, ITE82901_UI_OFF) < 0)
    {
        ui_mode = ITE82901_UI_STATIC;
    }

    int mode_idx = (ui_mode < 0) ? -1 : FindModeIndex(list, ui_mode);

    if(mode_idx < 0)
    {
        return(-1);
    }

    mode& m = list[mode_idx];

    if(m.flags & MODE_FLAG_HAS_SPEED)
    {
        m.speed = speed;
    }

    if(m.flags & MODE_FLAG_HAS_BRIGHTNESS)
    {
        m.brightness = (p.max_brightness > ITE82901_BRIGHTNESS_MAX) ? ITE82901_BRIGHTNESS_MAX : p.max_brightness;
    }

    if(m.colors.size() > 0)
    {
        m.colors[0] = FromDeviceColor(p.color0);
    }

    if(m.colors.size() > 1)
    {
        m.colors[1] = FromDeviceColor(p.color1);
    }

    if(random && (m.flags & MODE_FLAG_HAS_RANDOM_COLOR))
    {
        m.color_mode = MODE_COLORS_RANDOM;
    }

    return(mode_idx);
}
