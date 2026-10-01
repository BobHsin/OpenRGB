/*---------------------------------------------------------*\
| ITE82901Controller_Windows.h                              |
|                                                           |
|   Driver for ITE 82901 LED controller over PCH I2C,       |
|   through the ITE SPB peripheral kernel driver (IOCTL)    |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <windows.h>
#include <winioctl.h>
#include <mutex>
#include <string>
#include <vector>

/*---------------------------------------------------------*\
| ITE SPB peripheral driver interface                       |
|   Device path : \\.\<name><UID>   e.g. \\.\ITE8853_0       |
|   IOCTL codes : same values as the driver's               |
|                 spbtestioctl.h                            |
\*---------------------------------------------------------*/
#define ITE_SPB_FILE_DEVICE                 0x400

#define ITE_SPB_IOCTL_OPEN                  CTL_CODE(ITE_SPB_FILE_DEVICE, 0x700, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ITE_SPB_IOCTL_CLOSE                 CTL_CODE(ITE_SPB_FILE_DEVICE, 0x701, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define ITE_SPB_IOCTL_WRITEREAD             CTL_CODE(ITE_SPB_FILE_DEVICE, 0x704, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define ITE_SPB_MAX_TRANSFER                256     /* Driver buffer size           */
#define ITE_SPB_IO_TIMEOUT_MS               1000    /* Per-transfer timeout         */
#define ITE_SPB_IO_RETRIES                  50      /* Same as ITE reference DLL    */

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
    ITE82901Controller(std::string dev_name, std::string driver_name, int driver_uid);
    ~ITE82901Controller();

    bool            Open();
    bool            IsOpen();
    std::string     GetName();
    std::string     GetLocation();

    bool            SetPattern(unsigned char pattern);
    bool            ProbeRead();

private:
    std::string     name;
    std::string     device_path;
    HANDLE          dev_handle;
    std::mutex      io_mutex;

    bool            OpenLocked();
    void            CloseLocked();
    bool            Overlapped(int op, DWORD ioctl, void* in, DWORD in_len, void* out, DWORD out_len, DWORD* transferred);
    bool            Write(const unsigned char* data, int len);
    bool            Read(unsigned char* data, int len);
};
