/*---------------------------------------------------------*\
| RGBController_ITE82901_Windows.h                          |
|                                                           |
|   RGBController for ITE 82901 LED controller              |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <memory>
#include "RGBController.h"
#include "ITE82901Controller_Windows.h"

/*---------------------------------------------------------*\
| Speed slider: 0 = slowest, 10 = fastest                   |
\*---------------------------------------------------------*/
#define ITE82901_SPEED_MIN          0
#define ITE82901_SPEED_MAX          10
#define ITE82901_SPEED_DEFAULT      5

#define ITE82901_BRIGHTNESS_MIN     0
#define ITE82901_BRIGHTNESS_MAX     100

/*---------------------------------------------------------*\
| Device variants sharing one ITE82901Controller            |
|   STRIP : LED strip, gLedParameter[0] (cmd 0x20), Direct  |
|   PWM   : single PWM LED, gLedParameter[1] (cmd 0x21),    |
|           Static / Breathing / Flashing / Rainbow only    |
\*---------------------------------------------------------*/
enum
{
    ITE82901_VARIANT_STRIP      = 0,
    ITE82901_VARIANT_PWM        = 1,
};

class RGBController_ITE82901 : public RGBController
{
public:
    RGBController_ITE82901
        (
        std::shared_ptr<ITE82901Controller> controller_ptr,
        int                                 device_variant,
        int                                 parameter_slot,
        unsigned long long                  zone_mask
        );
    ~RGBController_ITE82901();

    void        SetupZones();

    void        DeviceUpdateLEDs();
    void        DeviceUpdateZoneLEDs(int zone);
    void        DeviceUpdateSingleLED(int led);

    void        DeviceUpdateMode();
    void        DeviceSaveMode();

private:
    std::shared_ptr<ITE82901Controller> controller;
    int                                 variant;
    int                                 param_slot;
    unsigned long long                  led_zone_mask;

    int         FindModeIndex(int value);
    void        LoadModeFromDevice();
};
