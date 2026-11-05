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
| Zones                                                     |
|   0: LED strip  - gLedParameter[0] (0x20), Direct capable |
|   1: PWM LED    - gLedParameter[1] (0x21), own zone modes |
\*---------------------------------------------------------*/
enum
{
    ITE82901_ZONE_STRIP         = 0,
    ITE82901_ZONE_PWM           = 1,
};

class RGBController_ITE82901 : public RGBController
{
public:
    RGBController_ITE82901
        (
        std::shared_ptr<ITE82901Controller> controller_ptr,
        int                                 strip_parameter_slot,
        unsigned long long                  strip_zone_mask,
        bool                                pwm_enabled,
        int                                 pwm_parameter_slot,
        unsigned long long                  pwm_zone_mask
        );
    ~RGBController_ITE82901();

    void        SetupZones();

    void        DeviceUpdateLEDs();
    void        DeviceUpdateZoneLEDs(int zone);
    void        DeviceUpdateSingleLED(int led);

    void        DeviceUpdateMode();
    void        DeviceUpdateZoneMode(int zone);
    void        DeviceSaveMode();

private:
    std::shared_ptr<ITE82901Controller> controller;
    int                                 strip_slot;
    unsigned long long                  strip_mask;
    bool                                pwm_present;
    int                                 pwm_slot;
    unsigned long long                  pwm_mask;

    void        AddStripModes(std::vector<mode>& list, bool with_direct);
    void        AddPwmModes(std::vector<mode>& list);
    int         FindModeIndex(const std::vector<mode>& list, int value);
    bool        PwmSupports(int value);

    const mode* EffectiveZoneMode(int zone);
    void        ApplyZone(int zone);
    bool        BuildParameter(const mode& m, ite82901_led_parameter* out);

    void        LoadModeFromDevice();
    int         ApplyParameterToModes(const ite82901_led_parameter& p, std::vector<mode>& list);
};
