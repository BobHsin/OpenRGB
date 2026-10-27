/*---------------------------------------------------------*\
| RGBController_ITE82901_Windows.h                          |
|                                                           |
|   RGBController for ITE 82901 LED controller              |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

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

class RGBController_ITE82901 : public RGBController
{
public:
    RGBController_ITE82901(ITE82901Controller* controller_ptr);
    ~RGBController_ITE82901();

    void        SetupZones();

    void        DeviceUpdateLEDs();
    void        DeviceUpdateZoneLEDs(int zone);
    void        DeviceUpdateSingleLED(int led);

    void        DeviceUpdateMode();

private:
    ITE82901Controller* controller;
};
