/*---------------------------------------------------------*\
| ITE82901ControllerDetect_Windows.cpp                      |
|                                                           |
|   Detector for ITE 82901 LED controller over PCH I2C      |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later                   |
\*---------------------------------------------------------*/

#include <cstdio>
#include <string>
#include <vector>
#include "DetectionManager.h"
#include "ITEI2CBridge_Windows.h"
#include "ITE82901Controller_Windows.h"
#include "RGBController_ITE82901_Windows.h"
#include "LogManager.h"
#include "ResourceManager.h"
#include "SettingsManager.h"

#define ITE82901_SETTINGS_KEY   "ITE82901Devices"
#define ITE82901_DEFAULT_DLL    "ITEI2CBridge.dll"
#define ITE82901_DEFAULT_BUS    0
#define ITE82901_DEFAULT_ADDR   0x68        /* 7-bit slave address          */
#define ITE82901_DEFAULT_SPEED  100         /* kHz                          */

/*---------------------------------------------------------*\
| Accept either a JSON number (e.g. 64) or a string          |
| (e.g. "0x40") for numeric settings                         |
\*---------------------------------------------------------*/
static int ReadIntSetting(const json& obj, const char* key, int default_value)
{
    if(!obj.contains(key))
    {
        return default_value;
    }

    const json& val = obj[key];

    if(val.is_number_integer())
    {
        return val.get<int>();
    }

    if(val.is_string())
    {
        try
        {
            return (int)std::stoul(val.get<std::string>(), nullptr, 0);
        }
        catch(...)
        {
        }
    }

    return default_value;
}

static bool ReadBoolSetting(const json& obj, const char* key, bool default_value)
{
    if(obj.contains(key) && obj[key].is_boolean())
    {
        return obj[key].get<bool>();
    }

    return default_value;
}

/*---------------------------------------------------------*\
| Write the default entry (bus 0, address 0x68) the first   |
| time, so it can be edited later in OpenRGB.json           |
\*---------------------------------------------------------*/
static json MakeDefaultDevice()
{
    char addr_str[8];
    snprintf(addr_str, sizeof(addr_str), "0x%02X", ITE82901_DEFAULT_ADDR);

    json device;
    device["enabled"]       = true;
    device["name"]          = "ITE 82901";
    device["bus"]           = ITE82901_DEFAULT_BUS;
    device["address"]       = addr_str;
    device["speed_khz"]     = ITE82901_DEFAULT_SPEED;
    device["probe_read"]    = false;
    return device;
}

static json WriteDefaultSettings(SettingsManager* settings_manager)
{
    json settings;

    settings["dll"]         = ITE82901_DEFAULT_DLL;
    settings["devices"]     = json::array();
    settings["devices"].push_back(MakeDefaultDevice());

    settings_manager->SetSettings(ITE82901_SETTINGS_KEY, settings);
    settings_manager->SaveSettings();

    LOG_INFO("[ITE82901] Wrote default configuration (bus %d, address 0x%02X) to OpenRGB.json",
             ITE82901_DEFAULT_BUS, ITE82901_DEFAULT_ADDR);

    return settings;
}

/*---------------------------------------------------------*\
| Replace the old untouched template (enabled:false,        |
| address "0x00") written by the previous build             |
\*---------------------------------------------------------*/
static void UpgradeOldTemplate(SettingsManager* settings_manager, json& settings)
{
    bool changed = false;

    for(json& dev : settings["devices"])
    {
        if(dev.contains("enabled") && dev["enabled"].is_boolean() && !dev["enabled"].get<bool>()
        && ReadIntSetting(dev, "address", -1) == 0x00)
        {
            dev = MakeDefaultDevice();
            changed = true;
        }
    }

    if(changed)
    {
        settings_manager->SetSettings(ITE82901_SETTINGS_KEY, settings);
        settings_manager->SaveSettings();
        LOG_INFO("[ITE82901] Upgraded placeholder configuration to bus %d, address 0x%02X",
                 ITE82901_DEFAULT_BUS, ITE82901_DEFAULT_ADDR);
    }
}

/******************************************************************************************\
*                                                                                          *
*   DetectITE82901Controllers                                                              *
*                                                                                          *
*       Create ITE 82901 controllers from the "ITE82901Devices" settings                   *
*                                                                                          *
\******************************************************************************************/

DetectedControllers DetectITE82901Controllers()
{
    DetectedControllers detected_controllers;

    SettingsManager* settings_manager = ResourceManager::get()->GetSettingsManager();
    json             settings         = settings_manager->GetSettings(ITE82901_SETTINGS_KEY);

    if(!settings.contains("devices") || !settings["devices"].is_array())
    {
        settings = WriteDefaultSettings(settings_manager);
    }
    else
    {
        UpgradeOldTemplate(settings_manager, settings);
    }

    /*-----------------------------------------------------*\
    | Skip loading the DLL if nothing is enabled            |
    \*-----------------------------------------------------*/
    bool any_enabled = false;

    for(const json& dev : settings["devices"])
    {
        if(ReadBoolSetting(dev, "enabled", true))
        {
            any_enabled = true;
        }
    }

    if(!any_enabled)
    {
        return(detected_controllers);
    }

    std::string dll_name = ITE82901_DEFAULT_DLL;

    if(settings.contains("dll") && settings["dll"].is_string())
    {
        dll_name = settings["dll"].get<std::string>();
    }

    ITEI2CBridge* bridge = ITEI2CBridge::Get();

    if(!bridge->Load(dll_name))
    {
        return(detected_controllers);
    }

    for(const json& dev : settings["devices"])
    {
        if(!ReadBoolSetting(dev, "enabled", true))
        {
            continue;
        }

        std::string name    = dev.contains("name") && dev["name"].is_string() ? dev["name"].get<std::string>() : "ITE 82901";
        int         bus     = ReadIntSetting(dev, "bus",       ITE82901_DEFAULT_BUS);
        int         address = ReadIntSetting(dev, "address",   ITE82901_DEFAULT_ADDR);
        int         speed   = ReadIntSetting(dev, "speed_khz", ITE82901_DEFAULT_SPEED);

        if(address <= 0x02 || address >= 0x78)
        {
            LOG_ERROR("[ITE82901] Invalid 7-bit I2C address 0x%02X for \"%s\", skipping", address, name.c_str());
            continue;
        }

        if(!bridge->Open(bus, address, speed))
        {
            LOG_ERROR("[ITE82901] Could not open WinRT I2C bus %d / address 0x%02X for \"%s\"", bus, address, name.c_str());
            continue;
        }

        /*-------------------------------------------------*\
        | Optional presence check: 1-byte read must be ACKed|
        \*-------------------------------------------------*/
        if(ReadBoolSetting(dev, "probe_read", false))
        {
            std::vector<unsigned char> probe(1, 0);

            if(!bridge->Read(bus, address, speed, probe))
            {
                LOG_ERROR("[ITE82901] No response at bus %d / address 0x%02X, skipping \"%s\"", bus, address, name.c_str());
                continue;
            }
        }

        LOG_INFO("[ITE82901] Registered \"%s\" on bus %d, address 0x%02X, %d kHz", name.c_str(), bus, address, speed);

        ITE82901Controller*     controller     = new ITE82901Controller(name, bus, address, speed);
        RGBController_ITE82901* rgb_controller = new RGBController_ITE82901(controller);

        detected_controllers.push_back(rgb_controller);
    }

    return(detected_controllers);
}   /* DetectITE82901Controllers() */

REGISTER_DETECTOR("ITE 82901", DetectITE82901Controllers);
