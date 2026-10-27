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
#define ITE82901_DEFAULT_DRIVER_NAME    "ITE8853_"              /* \\.\ITE8853_<UID>    */
#define ITE82901_DEFAULT_UID            0
#define ITE82901_DEFAULT_LED_MASK       "0xFFFFFFFFFFFFFFFF"    /* lZone_Sel: all LEDs  */
#define ITE82901_DEFAULT_PARAMETER      0                       /* gLedParameter[0]     */
#define ITE82901_DEFAULT_I2C_PROTOCOL   true                    /* 0xE0 + data, [cmd][data] */

/*---------------------------------------------------------*\
| Settings helpers                                          |
\*---------------------------------------------------------*/
static unsigned long long ReadU64Setting(const json& obj, const char* key, unsigned long long default_value)
{
    if(obj.contains(key))
    {
        if(obj[key].is_number_unsigned())
        {
            return(obj[key].get<unsigned long long>());
        }

        if(obj[key].is_number_integer())
        {
            return((unsigned long long)obj[key].get<long long>());
        }

        if(obj[key].is_string())
        {
            try
            {
                return(std::stoull(obj[key].get<std::string>(), nullptr, 0));
            }
            catch(...)
            {
            }
        }
    }

    return(default_value);
}

static int ReadIntSetting(const json& obj, const char* key, int default_value)
{
    return((int)ReadU64Setting(obj, key, (unsigned long long)default_value));
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

    device["enabled"]           = true;
    device["name"]              = ITE82901_DEFAULT_NAME;
    device["driver_name"]       = ITE82901_DEFAULT_DRIVER_NAME;
    device["uid"]               = ITE82901_DEFAULT_UID;
    device["led_mask"]          = ITE82901_DEFAULT_LED_MASK;
    device["parameter_index"]   = ITE82901_DEFAULT_PARAMETER;
    device["i2c_protocol"]      = ITE82901_DEFAULT_I2C_PROTOCOL;

    return(device);
}

/*---------------------------------------------------------*\
| Create default settings, or add missing keys to entries   |
| written by earlier versions                               |
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
        json defaults = MakeDefaultDevice();

        for(json& dev : settings["devices"])
        {
            /*---------------------------------------------*\
            | "smbus_block" (previous version, default true)|
            | is replaced by "i2c_protocol"                 |
            \*---------------------------------------------*/
            if(dev.contains("smbus_block"))
            {
                dev.erase("smbus_block");
                changed = true;
            }

            for(json::iterator it = defaults.begin(); it != defaults.end(); ++it)
            {
                if(!dev.contains(it.key()))
                {
                    dev[it.key()] = it.value();
                    changed = true;
                }
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

        std::string         name        = ReadStringSetting(dev, "name",            ITE82901_DEFAULT_NAME);
        std::string         driver_name = ReadStringSetting(dev, "driver_name",     ITE82901_DEFAULT_DRIVER_NAME);
        int                 uid         = ReadIntSetting   (dev, "uid",             ITE82901_DEFAULT_UID);
        unsigned long long  led_mask    = ReadU64Setting   (dev, "led_mask",        0xFFFFFFFFFFFFFFFFULL);
        int                 param_index = ReadIntSetting   (dev, "parameter_index", ITE82901_DEFAULT_PARAMETER);
        bool                i2c_mode    = ReadBoolSetting  (dev, "i2c_protocol",    ITE82901_DEFAULT_I2C_PROTOCOL);

        if(param_index < 0 || param_index >= ITE82901_PARAMETER_COUNT)
        {
            LOG_ERROR("[ITE82901] parameter_index %d out of range (0-%d) for \"%s\", using 0", param_index, ITE82901_PARAMETER_COUNT - 1, name.c_str());
            param_index = 0;
        }

        ITE82901Controller* controller = new ITE82901Controller(name, driver_name, uid, led_mask, param_index, i2c_mode);

        /*-------------------------------------------------*\
        | Driver not installed / device not present: skip   |
        \*-------------------------------------------------*/
        if(!controller->Open())
        {
            delete controller;
            continue;
        }

        LOG_INFO("[ITE82901] Registered \"%s\" (%s, LED mask 0x%016llX)",
                 name.c_str(), controller->GetLocation().c_str(), led_mask);

        detected_controllers.push_back(new RGBController_ITE82901(controller));
    }

    return(detected_controllers);
}   /* DetectITE82901Controllers() */

REGISTER_DETECTOR("ITE 82901", DetectITE82901Controllers);
