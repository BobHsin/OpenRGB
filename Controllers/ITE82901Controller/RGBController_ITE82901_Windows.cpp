/*---------------------------------------------------------*\
| RGBController_ITE82901_Windows.cpp                        |
|                                                           |
|   RGBController for ITE 82901 LED controller              |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later                   |
\*---------------------------------------------------------*/

#include "RGBController_ITE82901_Windows.h"

/**------------------------------------------------------------------*\
    @name ITE 82901
    @category Motherboard
    @type I2C
    @save :x:
    @direct :x:
    @effects :white_check_mark:
    @detectors DetectITE82901Controllers
    @comment Uses ITEI2CBridge.dll (WinRT Windows.Devices.I2c) on the
        PCH I2C controller. Bus index / slave address are configured in
        OpenRGB.json under "ITE82901Devices". The protocol only exposes
        hardware patterns (command 0x22), no color or speed control.
\*-------------------------------------------------------------------*/

RGBController_ITE82901::RGBController_ITE82901(ITE82901Controller* controller_ptr)
{
    controller  = controller_ptr;

    name        = controller->GetName();
    vendor      = "ITE";
    type        = DEVICE_TYPE_MOTHERBOARD;
    description = "ITE 82901 LED Controller (PCH I2C)";
    location    = controller->GetLocation();

    /*-----------------------------------------------------*\
    | Mode 0 is "Default" so OpenRGB does not assume the    |
    | LEDs are off at start-up (state cannot be read back)  |
    \*-----------------------------------------------------*/
    AddMode("Default",          ITE82901_PATTERN_DEFAULT);
    AddMode("Off",              ITE82901_PATTERN_OFF);
    AddMode("Flashing",         ITE82901_PATTERN_FLASH);
    AddMode("Double Flashing",  ITE82901_PATTERN_DOUBLE_FLASH);
    AddMode("Breathing",        ITE82901_PATTERN_BREATH);
    AddMode("Jump",             ITE82901_PATTERN_JUMP);
    AddMode("Rainbow Cycle",    ITE82901_PATTERN_RAINBOW_CYCLE);
    AddMode("Color Cycle",      ITE82901_PATTERN_COLOR_CYCLE);
    AddMode("Wave",             ITE82901_PATTERN_WAVE);

    active_mode = 0;

    SetupZones();
}

RGBController_ITE82901::~RGBController_ITE82901()
{
    Shutdown();

    delete controller;
}

void RGBController_ITE82901::AddMode(const char* mode_name, unsigned char pattern)
{
    mode new_mode;
    new_mode.name       = mode_name;
    new_mode.value      = pattern;
    new_mode.flags      = 0;
    new_mode.color_mode = MODE_COLORS_NONE;
    modes.push_back(new_mode);
}

void RGBController_ITE82901::SetupZones()
{
    /*-----------------------------------------------------*\
    | Pattern command applies to all LEDs (Group A + B), so |
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
    | No color command in the 82901 protocol                |
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

void RGBController_ITE82901::DeviceUpdateMode()
{
    controller->SetPattern((unsigned char)modes[active_mode].value);
}
