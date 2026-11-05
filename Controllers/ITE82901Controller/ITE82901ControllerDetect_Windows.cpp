/*---------------------------------------------------------*\
| ITE82901ControllerDetect_Windows.cpp                      |
|                                                           |
|   Detector for ITE 82901 LED controller over PCH I2C,     |
|   through the ITE SPB peripheral kernel driver            |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <memory>
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
#define ITE82901_DEFAULT_PARAMETER      0                       /* strip: 0x20          */
#define ITE82901_DEFAULT_PWM_ENABLED    true                    /* PWM LED device       */
#define ITE82901_DEFAULT_PWM_PARAMETER  1                       /* PWM: 0x21            */
#define ITE82901_DEFAULT_PWM_LED_MASK   "0xFFFFFFFFFFFFFFFF"    /* PWM lZone_Sel        */
#define ITE82901_DEFAULT_STRIP0_LEDS    40                      /* Direct mode length   */
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
    device["strip0_led_count"]  = ITE82901_DEFAULT_STRIP0_LEDS;
    device["pwm_enabled"]       = ITE82901_DEFAULT_PWM_ENABLED;
    device["pwm_parameter_index"] = ITE82901_DEFAULT_PWM_PARAMETER;
    device["pwm_led_mask"]      = ITE82901_DEFAULT_PWM_LED_MASK;

    return(device);
}

/*---------------------------------------------------------*\
| Build the settings, adding defaults / missing keys in     |
| memory. They are only saved to OpenRGB.json once a device |
| has actually been found (see DetectITE82901Controllers),  |
| so PCs without the hardware are left untouched.           |
\*---------------------------------------------------------*/
static json LoadSettings(SettingsManager* settings_manager, bool* changed)
{
    json settings = settings_manager->GetSettings(ITE82901_SETTINGS_KEY);

    *changed = false;

    if(!settings.contains("devices") || !settings["devices"].is_array())
    {
        settings["devices"] = json::array();
        settings["devices"].push_back(MakeDefaultDevice());
        *changed = true;
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
                *changed = true;
            }

            for(json::iterator it = defaults.begin(); it != defaults.end(); ++it)
            {
                if(!dev.contains(it.key()))
                {
                    dev[it.key()] = it.value();
                    *changed = true;
                }
            }
        }
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

    SettingsManager*    settings_manager = ResourceManager::get()->GetSettingsManager();
    bool                settings_changed = false;
    json                settings         = LoadSettings(settings_manager, &settings_changed);

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
        int                 strip_leds  = ReadIntSetting   (dev, "strip0_led_count", ITE82901_DEFAULT_STRIP0_LEDS);

        if(strip_leds < 1 || strip_leds > ITE82901_STRIP_MAX_LEDS)
        {
            LOG_ERROR("[ITE82901] strip0_led_count %d out of range (1-%d) for \"%s\", using %d", strip_leds, ITE82901_STRIP_MAX_LEDS, name.c_str(), ITE82901_DEFAULT_STRIP0_LEDS);
            strip_leds = ITE82901_DEFAULT_STRIP0_LEDS;
        }

        bool                pwm_on      = ReadBoolSetting  (dev, "pwm_enabled",         ITE82901_DEFAULT_PWM_ENABLED);
        int                 pwm_index   = ReadIntSetting   (dev, "pwm_parameter_index", ITE82901_DEFAULT_PWM_PARAMETER);
        unsigned long long  pwm_mask    = ReadU64Setting   (dev, "pwm_led_mask",        0xFFFFFFFFFFFFFFFFULL);

        if(param_index < 0 || param_index >= ITE82901_PARAMETER_COUNT)
        {
            LOG_ERROR("[ITE82901] parameter_index %d out of range (0-%d) for \"%s\", using %d", param_index, ITE82901_PARAMETER_COUNT - 1, name.c_str(), ITE82901_DEFAULT_PARAMETER);
            param_index = ITE82901_DEFAULT_PARAMETER;
        }

        if(pwm_index < 0 || pwm_index >= ITE82901_PARAMETER_COUNT || pwm_index == param_index)
        {
            LOG_ERROR("[ITE82901] pwm_parameter_index %d invalid (0-%d, must differ from parameter_index %d) for \"%s\", PWM LED disabled", pwm_index, ITE82901_PARAMETER_COUNT - 1, param_index, name.c_str());
            pwm_on = false;
        }

        /*-------------------------------------------------*\
        | One driver handle (opened exclusively); the strip |
        | and the PWM LED are two zones of one device       |
        \*-------------------------------------------------*/
        std::shared_ptr<ITE82901Controller> controller = std::make_shared<ITE82901Controller>(name, driver_name, uid, i2c_mode, strip_leds);

        /*-------------------------------------------------*\
        | Driver not installed / device not present: skip   |
        \*-------------------------------------------------*/
        if(!controller->Open())
        {
            continue;
        }

        LOG_INFO("[ITE82901] Registered \"%s\" (%s) - LED Strip: parameter %d, LED mask 0x%016llX; PWM LED: %s",
                 name.c_str(), controller->GetLocation().c_str(), param_index, led_mask, pwm_on ? "enabled" : "disabled");

        detected_controllers.push_back(new RGBController_ITE82901(controller, param_index, led_mask, pwm_on, pwm_index, pwm_mask));
    }

    /*-----------------------------------------------------*\
    | Persist defaults / new keys only on PCs that have the |
    | device                                                |
    \*-----------------------------------------------------*/
    if(settings_changed && !detected_controllers.empty())
    {
        settings_manager->SetSettings(ITE82901_SETTINGS_KEY, settings);
        settings_manager->SaveSettings();
        LOG_INFO("[ITE82901] Updated \"" ITE82901_SETTINGS_KEY "\" in OpenRGB.json");
    }

    return(detected_controllers);
}   /* DetectITE82901Controllers() */

REGISTER_DETECTOR("ITE 82901", DetectITE82901Controllers);
