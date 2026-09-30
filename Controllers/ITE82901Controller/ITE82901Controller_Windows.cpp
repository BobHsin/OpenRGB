/*---------------------------------------------------------*\
| ITE82901Controller_Windows.cpp                            |
|                                                           |
|   Driver for ITE 82901 LED controller over PCH I2C        |
|   (via ITEI2CBridge.dll)                                  |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later                   |
\*---------------------------------------------------------*/

#include <cstdio>
#include <vector>
#include "ITE82901Controller_Windows.h"
#include "ITEI2CBridge_Windows.h"
#include "LogManager.h"

ITE82901Controller::ITE82901Controller(std::string dev_name, int bus_idx, int dev_addr, int speed)
{
    name        = dev_name;
    bus         = bus_idx;
    address     = dev_addr;
    speed_khz   = speed;
}

ITE82901Controller::~ITE82901Controller()
{
}

std::string ITE82901Controller::GetName()
{
    return name;
}

std::string ITE82901Controller::GetLocation()
{
    char buf[64];
    snprintf(buf, sizeof(buf), "WinRT I2C: bus %d, address 0x%02X, %d kHz", bus, address, speed_khz);
    return std::string(buf);
}

/*---------------------------------------------------------*\
| Packet: [ 0x22 ][ pattern index ]                          |
\*---------------------------------------------------------*/
bool ITE82901Controller::SetPattern(unsigned char pattern)
{
    std::vector<unsigned char> packet = { ITE82901_CMD_PATTERN_SET, pattern };

    bool ok = ITEI2CBridge::Get()->Write(bus, address, speed_khz, packet);

    if(!ok)
    {
        LOG_ERROR("[ITE82901] Failed to write pattern 0x%02X to bus %d addr 0x%02X", pattern, bus, address);
    }

    return ok;
}
