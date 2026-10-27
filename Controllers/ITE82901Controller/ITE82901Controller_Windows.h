/*---------------------------------------------------------*\
| ITE82901Controller_Windows.h                              |
|                                                           |
|   Driver for ITE 82901 LED controller (IT8297/IT8237      |
|   LED parameter protocol) over PCH I2C, through the ITE   |
|   SPB peripheral kernel driver (IOCTL)                    |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <windows.h>
#include <winioctl.h>
#include <mutex>
#include <string>

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
| IT8297/IT8237 LED controller commands                     |
\*---------------------------------------------------------*/
enum
{
    ITE82901_CMD_LED_PARAMETER_0        = 0x20,     /* 0x20+N: set LED parameter N (32 B)   */
    ITE82901_CMD_APPLY                  = 0x28,     /* 1 B: bitN=1 applies parameter N      */
    ITE82901_CMD_SET_PROTOCOL           = 0xE0,     /* 1 B: 1 = I2C, 0 = SMBus (FW default) */
};

enum
{
    ITE82901_PROTOCOL_SMBUS             = 0x00,
    ITE82901_PROTOCOL_I2C               = 0x01,
};

/*---------------------------------------------------------*\
| Data byte sent with ITE82901_CMD_SET_PROTOCOL before      |
| every LED update. Test build: 0x00 (doc: 0 = SMBus).      |
| Change back to ITE82901_PROTOCOL_I2C (0x01) if needed.    |
\*---------------------------------------------------------*/
#define ITE82901_SET_PROTOCOL_DATA          ITE82901_PROTOCOL_SMBUS

#define ITE82901_PARAMETER_SIZE             32      /* gLedParameter size in bytes          */
#define ITE82901_PARAMETER_COUNT            8       /* 0x20-0x27, selectable by 0x28 mask   */
#define ITE82901_COMMAND_GAP_MS             2       /* Spec: min 1 ms between commands      */

/*---------------------------------------------------------*\
| bMode_Sel values                                          |
\*---------------------------------------------------------*/
enum
{
    ITE82901_MODE_NULL                  = 0,
    ITE82901_MODE_STATIC                = 1,
    ITE82901_MODE_PULSE                 = 2,        /* Breath                           */
    ITE82901_MODE_FLASH                 = 3,
    ITE82901_MODE_COLOR_CYCLE           = 4,
    ITE82901_MODE_METEOR                = 5,
    ITE82901_MODE_RAINBOW               = 6,
    ITE82901_MODE_RIPPLE                = 7,
    ITE82901_MODE_RANDOM                = 8,
    ITE82901_MODE_WAVE2                 = 9,
};

/*---------------------------------------------------------*\
| One gLedParameter (zone mask is set by the controller)    |
|   Colors are 0x00RRGGBB, times are milliseconds,          |
|   brightness is percent (0-100)                           |
\*---------------------------------------------------------*/
typedef struct
{
    unsigned char   mode;               /* bMode_Sel                        */
    unsigned char   max_brightness;     /* bMax_brightness                  */
    unsigned char   min_brightness;     /* bMin_brightness                  */
    unsigned int    color0;             /* dwColor0                         */
    unsigned int    color1;             /* dwColor1                         */
    unsigned short  time_base[4];       /* wTime_base0-3                    */
    unsigned char   ctrl_value[4];      /* bCtrl_value0-3                   */
} ite82901_led_parameter;

class ITE82901Controller
{
public:
    ITE82901Controller
        (
        std::string         dev_name,
        std::string         driver_name,
        int                 driver_uid,
        unsigned long long  zone_mask,
        int                 parameter_index,
        bool                i2c_protocol
        );
    ~ITE82901Controller();

    bool            Open();
    std::string     GetName();
    std::string     GetLocation();

    bool            SetLedParameter(const ite82901_led_parameter* param);

private:
    std::string         name;
    std::string         device_path;
    unsigned long long  led_zone_mask;
    int                 param_index;
    bool                use_i2c_protocol;
    HANDLE              dev_handle;
    std::mutex          io_mutex;

    bool            OpenLocked();
    void            CloseLocked();
    bool            Overlapped(int op, DWORD ioctl, void* in, DWORD in_len, void* out, DWORD out_len, DWORD* transferred);
    bool            WriteLocked(const unsigned char* data, int len);
    bool            SendBlockLocked(unsigned char cmd, const unsigned char* data, int len);
    bool            SendByteLocked(unsigned char cmd, unsigned char data);
    bool            SetProtocolLocked();
};
