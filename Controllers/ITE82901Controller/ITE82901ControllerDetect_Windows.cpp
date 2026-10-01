/*---------------------------------------------------------*\
| ITE82901ControllerDetect_Windows.cpp                      |
|                                                           |
|   Detector for ITE 82901 LED controller over PCH I2C,     |
|   through the ITE SPB peripheral kernel driver            |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <string>
#include "DetectionManager.h"
#include "ITE82901Controller_Windows.h"
#include "RGBController_ITE82901_Windows.h"
#include "LogManager.h"
#include "ResourceManager.h"
#include "SettingsManager.h"

#define ITE82901_SETTINGS_KEY           "ITE82901Devices"
#define ITE82901_DEFAULT_NAME           "ITE 82901"
#define ITE82901_DEFAULT_DRIVER_NAME    "ITE8853_"      /* \\.\ITE8853_<UID>        */
#define ITE82901_DEFAULT_UID            0

/*---------------------------------------------------------*\
| Settings helpers                                          |
\*---------------------------------------------------------*/
static int ReadIntSetting(const json& obj, const char* key, int default_value)
{
    if(obj.contains(key))
    {
        if(obj[key].is_number_integer())
        {
            return(obj[key].get<int>());
        }

        if(obj[key].is_string())
        {
            try
            {
                return((int)std::stoul(obj[key].get<std::string>(), nullptr, 0));
            }
            catch(...)
            {
            }
        }
    }

    return(default_value);
}

static bool ReadBoolSetting(const json& obj, const char* key, bool default_value)
{
    if(obj.contains(key) && obj[key].is_boolean())
    {
        return(obj[key].get<bool>());
    }

    return(default_value);
}

static std::string ReadStringSetting(const json& obj, const char* key, const char* default_value)
{
    if(obj.contains(key) && obj[key].is_string())
    {
        return(obj[key].get<std::string>());
    }

    return(default_value);
}

static json MakeDefaultDevice()
{
    json device;

    device["enabled"]       = true;
    device["name"]          = ITE82901_DEFAULT_NAME;
    device["driver_name"]   = ITE82901_DEFAULT_DRIVER_NAME;
    device["uid"]           = ITE82901_DEFAULT_UID;
    device["probe_read"]    = false;

    return(device);
}

/*---------------------------------------------------------*\
| Create default settings, or add the driver keys to        |
| entries written by earlier versions (bus/address based)   |
\*---------------------------------------------------------*/
static json LoadSettings(SettingsManager* settings_manager)
{
    json settings = settings_manager->GetSettings(ITE82901_SETTINGS_KEY);
    bool changed  = false;

    if(!settings.contains("devices") || !settings["devices"].is_array())
    {
        settings["devices"] = json::array();
        settings["devices"].push_back(MakeDefaultDevice());
        changed = true;
    }
    else
    {
        for(json& dev : settings["devices"])
        {
            if(!dev.contains("driver_name"))
            {
                dev["driver_name"] = ITE82901_DEFAULT_DRIVER_NAME;
                changed = true;
            }

            if(!dev.contains("uid"))
            {
                dev["uid"] = ITE82901_DEFAULT_UID;
                changed = true;
            }

            /*---------------------------------------------*\
            | Old placeholder template (enabled:false,      |
            | address "0x00") from the first version        |
            \*---------------------------------------------*/
            if(!ReadBoolSetting(dev, "enabled", true) && ReadIntSetting(dev, "address", -1) == 0x00)
            {
                dev["enabled"] = true;
                changed = true;
            }
        }
    }

    if(changed)
    {
        settings_manager->SetSettings(ITE82901_SETTINGS_KEY, settings);
        settings_manager->SaveSettings();
        LOG_INFO("[ITE82901] Updated \"" ITE82901_SETTINGS_KEY "\" in OpenRGB.json");
    }

    return(settings);
}

/******************************************************************************************\
*                                                                                          *
*   DetectITE82901Controllers                                                              *
*                                                                                          *
*       Open \\.\<driver_name><uid> for each configured device                             *
*                                                                                          *
\******************************************************************************************/

DetectedControllers DetectITE82901Controllers()
{
    DetectedControllers detected_controllers;

    json settings = LoadSettings(ResourceManager::get()->GetSettingsManager());

    for(const json& dev : settings["devices"])
    {
        if(!ReadBoolSetting(dev, "enabled", true))
        {
            continue;
        }

        std::string name        = ReadStringSetting(dev, "name",        ITE82901_DEFAULT_NAME);
        std::string driver_name = ReadStringSetting(dev, "driver_name", ITE82901_DEFAULT_DRIVER_NAME);
        int         uid         = ReadIntSetting   (dev, "uid",         ITE82901_DEFAULT_UID);

        ITE82901Controller* controller = new ITE82901Controller(name, driver_name, uid);

        /*-------------------------------------------------*\
        | Driver not installed / device not present: skip   |
        \*-------------------------------------------------*/
        if(!controller->Open())
        {
            delete controller;
            continue;
        }

        if(ReadBoolSetting(dev, "probe_read", false) && !controller->ProbeRead())
        {
            LOG_ERROR("[ITE82901] No response from %s, skipping \"%s\"", controller->GetLocation().c_str(), name.c_str());
            delete controller;
            continue;
        }

        LOG_INFO("[ITE82901] Registered \"%s\" (%s)", name.c_str(), controller->GetLocation().c_str());

        detected_controllers.push_back(new RGBController_ITE82901(controller));
    }

    return(detected_controllers);
}   /* DetectITE82901Controllers() */

REGISTER_DETECTOR("ITE 82901", DetectITE82901Controllers);
