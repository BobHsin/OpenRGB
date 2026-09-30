/*---------------------------------------------------------*\
| ITE82901Controller_Windows.h                              |
|                                                           |
|   Driver for ITE 82901 LED controller over PCH I2C        |
|   (via ITEI2CBridge.dll)                                  |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later                   |
\*---------------------------------------------------------*/

#pragma once

#include <string>

/*---------------------------------------------------------*\
| Command index                                             |
\*---------------------------------------------------------*/
enum
{
    ITE82901_CMD_PATTERN_SET            = 0x22,     /* LED pattern set for all LEDs     */
};

/*---------------------------------------------------------*\
| Pattern index (data byte of ITE82901_CMD_PATTERN_SET)     |
\*---------------------------------------------------------*/
enum
{
    ITE82901_PATTERN_OFF                = 0x00,     /* Shut down (0x00 and 0x01)        */
    ITE82901_PATTERN_FLASH              = 0x02,
    ITE82901_PATTERN_DOUBLE_FLASH       = 0x03,
    ITE82901_PATTERN_BREATH             = 0x04,
    ITE82901_PATTERN_JUMP               = 0x05,
    ITE82901_PATTERN_RAINBOW_CYCLE      = 0x06,
    ITE82901_PATTERN_COLOR_CYCLE        = 0x07,
    ITE82901_PATTERN_WAVE               = 0x08,
    ITE82901_PATTERN_DEFAULT            = 0x1C,     /* Reset: Group A purple, B white   */
};

class ITE82901Controller
{
public:
    ITE82901Controller(std::string dev_name, int bus, int address, int speed_khz);
    ~ITE82901Controller();

    std::string     GetName();
    std::string     GetLocation();

    bool            SetPattern(unsigned char pattern);

private:
    std::string     name;
    int             bus;
    int             address;
    int             speed_khz;
};
