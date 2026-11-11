/*---------------------------------------------------------*\
| HIDLampArrayControllerDetect.cpp                          |
|                                                           |
|   Detector for HID LampArray Devices                      |
|                                                           |
|   Adam Honse (calcprogrammer1@gmail.com)      26 Mar 2024 |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-only                   |
\*---------------------------------------------------------*/

#include "DetectionManager.h"
#include "HIDLampArrayController.h"
#include "RGBController.h"
#include "RGBController_HIDLampArray.h"
#include <vector>
#include <hidapi.h>
#include "LogManager.h"

DetectedControllers DetectHIDLampArrayControllers(hid_device_info* info, const std::string& /*name*/)
{
    DetectedControllers detected_controllers;
    hid_device*         dev;

    LOG_INFO("[HID LampArray DIAG] Candidate %04X:%04X usage page 0x%04X usage 0x%04X interface %d: %s",
             info->vendor_id, info->product_id, info->usage_page, info->usage, info->interface_number, info->path);

    dev = hid_open_path(info->path);

    if(!dev)
    {
        LOG_WARNING("[HID LampArray DIAG] hid_open_path failed for %04X:%04X", info->vendor_id, info->product_id);
    }

    if(dev)
    {
        HIDLampArrayController* controller = new HIDLampArrayController(dev, info->path);

        /*-------------------------------------------------*\
        | Only create the RGBController if there are lamps  |
        | detected in the controller.  Otherwise, delete    |
        | the controller.                                   |
        \*-------------------------------------------------*/
        if(controller->GetLampCount() > 0)
        {
            RGBController_HIDLampArray* rgb_controller = new RGBController_HIDLampArray(controller);

            detected_controllers.push_back(rgb_controller);
        }
        else
        {
            LOG_WARNING("[HID LampArray DIAG] %04X:%04X has 0 lamps, not registered", info->vendor_id, info->product_id);
            delete controller;
        }
    }

    return(detected_controllers);
}
    
REGISTER_HID_DETECTOR_PU_ONLY("HID LampArray Device", DetectHIDLampArrayControllers, 0x59, 0x01);
REGISTER_HID_DETECTOR_P("ITE LampArray Device", DetectHIDLampArrayControllers, 0x048D, 0x8290, 0xFF82);
