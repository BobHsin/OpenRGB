/*---------------------------------------------------------*\
| ITE82901Controller_Windows.cpp                            |
|                                                           |
|   Driver for ITE 82901 LED controller (IT8297/IT8237      |
|   LED parameter protocol) over PCH I2C, through the ITE   |
|   SPB peripheral kernel driver (IOCTL)                    |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include "ITE82901Controller_Windows.h"
#include "LogManager.h"
#include <mmsystem.h>

#pragma comment(lib, "winmm")

/*---------------------------------------------------------*\
| Overlapped operation types                                |
\*---------------------------------------------------------*/
enum
{
    ITE_SPB_OP_WRITE    = 0,
    ITE_SPB_OP_READ     = 1,
    ITE_SPB_OP_IOCTL    = 2,
};

/*---------------------------------------------------------*\
| Little-endian helpers (no struct packing assumptions)     |
\*---------------------------------------------------------*/
static void PutLE16(unsigned char* buf, unsigned short value)
{
    buf[0] = (unsigned char)(value & 0xFF);
    buf[1] = (unsigned char)((value >> 8) & 0xFF);
}

static void PutLE32(unsigned char* buf, unsigned int value)
{
    for(int i = 0; i < 4; i++)
    {
        buf[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
    }
}

static void PutLE64(unsigned char* buf, unsigned long long value)
{
    for(int i = 0; i < 8; i++)
    {
        buf[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
    }
}

static unsigned short GetLE16(const unsigned char* buf)
{
    return((unsigned short)(buf[0] | (buf[1] << 8)));
}

static unsigned int GetLE32(const unsigned char* buf)
{
    return((unsigned int)buf[0] | ((unsigned int)buf[1] << 8) | ((unsigned int)buf[2] << 16) | ((unsigned int)buf[3] << 24));
}

/*---------------------------------------------------------*\
| Sanity check of a 32-byte gLedParameter image             |
\*---------------------------------------------------------*/
static bool LooksLikeParameter(const unsigned char* buf)
{
    return((buf[8] == 0x00)                         /* bReserve0                */
        && (buf[9] <= ITE82901_MODE_WAVE2)          /* bMode_Sel                */
        && (buf[10] <= 100)                         /* bMax_brightness (%)      */
        && (buf[11] <= buf[10]));                   /* bMin <= bMax             */
}

static unsigned long long NowMs()
{
    return((unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count());
}

/*---------------------------------------------------------*\
| 0x34 size code for a strip length                         |
|   0: 32, 1: 64, 2: 256, 3: 512, 4: 1024 LEDs              |
\*---------------------------------------------------------*/
static unsigned char StripSizeCode(int leds)
{
    if(leds <= 32)  return(0);
    if(leds <= 64)  return(1);
    if(leds <= 256) return(2);
    if(leds <= 512) return(3);
    return(4);
}

static void CommandGap()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ITE82901_COMMAND_GAP_MS));
}

ITE82901Controller::ITE82901Controller
    (
    std::string         dev_name,
    std::string         driver_name,
    int                 driver_uid,
    unsigned long long  zone_mask,
    int                 parameter_index,
    bool                i2c_protocol,
    int                 strip0_led_count
    )
{
    char path[128];

    snprintf(path, sizeof(path), "\\\\.\\%s%d", driver_name.c_str(), driver_uid);

    name            = dev_name;
    device_path     = path;
    led_zone_mask   = zone_mask;
    param_index     = parameter_index;
    use_i2c_protocol = i2c_protocol;
    strip_led_count  = strip0_led_count;
    direct_enabled   = false;
    timer_raised     = false;
    last_protocol_ms = 0;

    if(strip_led_count < 1)
    {
        strip_led_count = 1;
    }

    if(strip_led_count > ITE82901_STRIP_MAX_LEDS)
    {
        strip_led_count = ITE82901_STRIP_MAX_LEDS;
    }
    dev_handle      = INVALID_HANDLE_VALUE;

    if(param_index < 0 || param_index >= ITE82901_PARAMETER_COUNT)
    {
        param_index = 0;
    }
}

ITE82901Controller::~ITE82901Controller()
{
    std::lock_guard<std::mutex> lock(io_mutex);

    if(timer_raised)
    {
        timeEndPeriod(1);
        timer_raised = false;
    }

    CloseLocked();
}

int ITE82901Controller::GetStripLedCount()
{
    return(strip_led_count);
}

bool ITE82901Controller::Open()
{
    std::lock_guard<std::mutex> lock(io_mutex);
    return(OpenLocked());
}

std::string ITE82901Controller::GetName()
{
    return(name);
}

std::string ITE82901Controller::GetLocation()
{
    char buf[160];

    snprintf(buf, sizeof(buf), "ITE SPB driver: %s, parameter %d, %s", device_path.c_str(), param_index, use_i2c_protocol ? "I2C" : "SMBus");

    return(std::string(buf));
}

/*---------------------------------------------------------*\
| Send set protocol (cmd 0xE0, ITE82901_SET_PROTOCOL_DATA)  |
|                                                           |
|   [0xE0][data] is the same on the wire in SMBus and I2C   |
|   framing, so it works whatever mode the FW is in. It is  |
|   resent before every LED update because the FW returns   |
|   to its SMBus default after a power cycle (e.g. resume)  |
|   and a raw I2C packet would then be misread silently.    |
\*---------------------------------------------------------*/
bool ITE82901Controller::SetProtocolLocked()
{
    if(!use_i2c_protocol)
    {
        return(true);
    }

    bool ok = SendByteLocked(ITE82901_CMD_SET_PROTOCOL, ITE82901_SET_PROTOCOL_DATA);
    CommandGap();

    if(ok)
    {
        last_protocol_ms = NowMs();
    }

    if(!ok)
    {
        LOG_ERROR("[ITE82901] Set protocol (0xE0 0x%02X) failed via %s", ITE82901_SET_PROTOCOL_DATA, device_path.c_str());
    }

    return(ok);
}

/*---------------------------------------------------------*\
| Write gLedParameter[N] (cmd 0x20+N, 32 bytes) and apply   |
| it (cmd 0x28, bitN)                                       |
|                                                           |
|   Offset  Size  Field                                     |
|   0       8     lZone_Sel (bit x = LED x)                 |
|   8       1     bReserve0                                 |
|   9       1     bMode_Sel                                 |
|   10      1     bMax_brightness (%)                       |
|   11      1     bMin_brightness (%)                       |
|   12      4     dwColor0 (0x00RRGGBB)                     |
|   16      4     dwColor1 (0x00RRGGBB)                     |
|   20      8     wTime_base0-3 (ms)                        |
|   28      4     bCtrl_value0-3                            |
\*---------------------------------------------------------*/
bool ITE82901Controller::SetLedParameter(const ite82901_led_parameter* param)
{
    unsigned char buf[ITE82901_PARAMETER_SIZE];

    memset(buf, 0, sizeof(buf));

    PutLE64(&buf[0],  led_zone_mask);
    buf[8]  = 0x00;
    buf[9]  = param->mode;
    buf[10] = param->max_brightness;
    buf[11] = param->min_brightness;
    PutLE32(&buf[12], param->color0);
    PutLE32(&buf[16], param->color1);

    for(int i = 0; i < 4; i++)
    {
        PutLE16(&buf[20 + (2 * i)], param->time_base[i]);
        buf[28 + i] = param->ctrl_value[i];
    }

    std::lock_guard<std::mutex> lock(io_mutex);

    bool ok = SetProtocolLocked();

    /*-----------------------------------------------------*\
    | Hardware effects need Direct mode turned off first    |
    \*-----------------------------------------------------*/
    if(ok && direct_enabled)
    {
        ok = SetDirectModeLocked(false);
    }

    if(ok)
    {
        ok = SendBlockLocked((unsigned char)(ITE82901_CMD_LED_PARAMETER_0 + param_index), buf, sizeof(buf));
        CommandGap();
    }

    if(ok)
    {
        ok = SendByteLocked(ITE82901_CMD_APPLY, (unsigned char)(1 << param_index));
        CommandGap();
    }

    if(!ok)
    {
        LOG_ERROR("[ITE82901] Failed to set LED parameter %d (mode %d) via %s", param_index, param->mode, device_path.c_str());
    }

    return(ok);
}

/*---------------------------------------------------------*\
| Read gLedParameter[N] back from the FW                    |
|                                                           |
|   [0xE0][protocol]   same prefix as every update          |
|   write [0x20+N], repeated start, read 32 bytes           |
|                                                           |
|   No 0xE1 is sent. The 32 bytes are used directly as the  |
|   gLedParameter image (same layout as the write).         |
\*---------------------------------------------------------*/
bool ITE82901Controller::ReadLedParameter(ite82901_led_parameter* param)
{
    std::lock_guard<std::mutex> lock(io_mutex);

    if(!SetProtocolLocked())
    {
        return(false);
    }

    unsigned char cmd = (unsigned char)(ITE82901_CMD_LED_PARAMETER_0 + param_index);
    unsigned char buf[ITE82901_PARAMETER_SIZE];
    int           count = 0;
    bool          ok    = false;

    for(int attempt = 0; attempt < ITE82901_READ_RETRIES && !ok; attempt++)
    {
        memset(buf, 0, sizeof(buf));
        ok = WriteReadLocked(&cmd, 1, buf, sizeof(buf), &count);
        CommandGap();
    }

    if(!ok)
    {
        LOG_ERROR("[ITE82901] Read of parameter %d (0x%02X) failed via %s", param_index, cmd, device_path.c_str());
        return(false);
    }

    /*-----------------------------------------------------*\
    | Log the raw bytes for checking with a logic analyzer  |
    \*-----------------------------------------------------*/
    char hex[3 * sizeof(buf) + 1];
    int  pos = 0;

    hex[0] = '\0';

    for(int i = 0; i < count && i < (int)sizeof(buf); i++)
    {
        pos += snprintf(&hex[pos], sizeof(hex) - pos, "%02X ", buf[i]);
    }

    LOG_INFO("[ITE82901] Read 0x%02X returned %d byte(s): %s", cmd, count, hex);

    if(count < ITE82901_PARAMETER_SIZE)
    {
        LOG_ERROR("[ITE82901] Read-back too short (%d of %d bytes), ignoring", count, ITE82901_PARAMETER_SIZE);
        return(false);
    }

    /*-----------------------------------------------------*\
    | All 0x00 / all 0xFF means the FW returned no data     |
    \*-----------------------------------------------------*/
    bool all_00 = true;
    bool all_ff = true;

    for(int i = 0; i < ITE82901_PARAMETER_SIZE; i++)
    {
        all_00 = all_00 && (buf[i] == 0x00);
        all_ff = all_ff && (buf[i] == 0xFF);
    }

    if(all_00 || all_ff)
    {
        LOG_ERROR("[ITE82901] Read-back is all 0x%s: FW returned no parameter data, ignoring", all_00 ? "00" : "FF");
        return(false);
    }

    if(!LooksLikeParameter(buf))
    {
        LOG_ERROR("[ITE82901] Read-back data does not look like a gLedParameter, ignoring");
        return(false);
    }

    memset(param, 0, sizeof(*param));

    param->mode             = buf[9];
    param->max_brightness   = buf[10];
    param->min_brightness   = buf[11];
    param->color0           = GetLE32(&buf[12]);
    param->color1           = GetLE32(&buf[16]);

    for(int i = 0; i < 4; i++)
    {
        param->time_base[i]  = GetLE16(&buf[20 + (2 * i)]);
        param->ctrl_value[i] = buf[28 + i];
    }

    return(true);
}

/*---------------------------------------------------------*\
| Save parameters as the FW default (cmd 0x5E, no data)     |
\*---------------------------------------------------------*/
bool ITE82901Controller::SaveToFlash()
{
    std::lock_guard<std::mutex> lock(io_mutex);

    bool ok = SetProtocolLocked();

    if(ok)
    {
        unsigned char packet[1] = { ITE82901_CMD_SAVE };

        ok = WriteLocked(packet, sizeof(packet));
        CommandGap();
    }

    if(ok)
    {
        LOG_INFO("[ITE82901] Saved current effect as default (0x5E) via %s", device_path.c_str());
    }
    else
    {
        LOG_ERROR("[ITE82901] Save (0x5E) failed via %s", device_path.c_str());
    }

    return(ok);
}

/*---------------------------------------------------------*\
| Direct mode on strip-0                                    |
|                                                           |
|   On : [0xE0][protocol] [0x34][size code] [0x32][0x01]    |
|   Off: [0x32][0x00]                                       |
|   Both wait ITE82901_DIRECT_SETTLE_MS after 0x32.         |
\*---------------------------------------------------------*/
bool ITE82901Controller::SetDirectMode(bool enable)
{
    std::lock_guard<std::mutex> lock(io_mutex);

    if(enable && !SetProtocolLocked())
    {
        return(false);
    }

    return(SetDirectModeLocked(enable));
}

bool ITE82901Controller::SetDirectModeLocked(bool enable)
{
    bool ok = true;

    if(enable)
    {
        ok = SendByteLocked(ITE82901_CMD_STRIP_LENGTH, StripSizeCode(strip_led_count));
        CommandGap();
    }

    if(ok)
    {
        ok = SendByteLocked(ITE82901_CMD_DIRECT_ENABLE, enable ? ITE82901_DIRECT_STRIP0_BIT : 0x00);
        std::this_thread::sleep_for(std::chrono::milliseconds(ITE82901_DIRECT_SETTLE_MS));
    }

    if(!ok)
    {
        LOG_ERROR("[ITE82901] %s Direct mode failed via %s", enable ? "Enable" : "Disable", device_path.c_str());
        return(false);
    }

    direct_enabled = enable;

    /*-----------------------------------------------------*\
    | 1 ms timer resolution only while streaming, so the    |
    | 5 ms packet gap is ~5 ms instead of ~16 ms            |
    \*-----------------------------------------------------*/
    if(enable && !timer_raised)
    {
        timeBeginPeriod(1);
        timer_raised = true;
    }
    else if(!enable && timer_raised)
    {
        timeEndPeriod(1);
        timer_raised = false;
    }

    LOG_INFO("[ITE82901] Direct mode %s (strip-0, %d LEDs)", enable ? "on" : "off", strip_led_count);
    return(true);
}

/*---------------------------------------------------------*\
| Send RGB data to strip-0 (cmd 0x58)                       |
|   [0x58][offset lo][offset hi][length][R G B R G B ...]   |
|   offset and length are in bytes, max 57 data bytes       |
\*---------------------------------------------------------*/
bool ITE82901Controller::SetStripColors(const unsigned char* rgb, int led_count)
{
    std::lock_guard<std::mutex> lock(io_mutex);

    if(!direct_enabled)
    {
        return(false);
    }

    if(led_count > strip_led_count)
    {
        led_count = strip_led_count;
    }

    /*-----------------------------------------------------*\
    | Refresh the protocol now and then instead of on every |
    | frame, in case the FW was reset (e.g. sleep/resume)   |
    \*-----------------------------------------------------*/
    if(NowMs() - last_protocol_ms >= ITE82901_PROTOCOL_REFRESH_MS)
    {
        if(!SetProtocolLocked())
        {
            return(false);
        }
    }

    int total_bytes = led_count * 3;
    int sent        = 0;

    while(sent < total_bytes)
    {
        int           chunk = total_bytes - sent;
        unsigned char data[3 + ITE82901_DIRECT_MAX_DATA];

        if(chunk > ITE82901_DIRECT_MAX_DATA)
        {
            chunk = ITE82901_DIRECT_MAX_DATA;
        }

        data[0] = (unsigned char)(sent & 0xFF);
        data[1] = (unsigned char)((sent >> 8) & 0xFF);
        data[2] = (unsigned char)chunk;
        memcpy(&data[3], &rgb[sent], chunk);

        bool ok = SendBlockLocked(ITE82901_CMD_DIRECT_STRIP0, data, 3 + chunk);
        CommandGap();

        if(!ok)
        {
            LOG_ERROR("[ITE82901] Direct data (0x58, offset %d) failed via %s", sent, device_path.c_str());
            return(false);
        }

        sent += chunk;
    }

    return(true);
}

/*---------------------------------------------------------*\
| Command framing                                           |
|   I2C (default)     : [cmd][data...]                      |
|   SMBus block write : [cmd][count][data...]               |
\*---------------------------------------------------------*/
bool ITE82901Controller::SendBlockLocked(unsigned char cmd, const unsigned char* data, int len)
{
    unsigned char packet[2 + ITE82901_MAX_BLOCK];
    int           pos = 0;

    if(len < 0 || len > ITE82901_MAX_BLOCK)
    {
        return(false);
    }

    packet[pos++] = cmd;

    if(!use_i2c_protocol)
    {
        packet[pos++] = (unsigned char)len;
    }

    memcpy(&packet[pos], data, len);
    pos += len;

    return(WriteLocked(packet, pos));
}

bool ITE82901Controller::SendByteLocked(unsigned char cmd, unsigned char data)
{
    unsigned char packet[2] = { cmd, data };

    return(WriteLocked(packet, sizeof(packet)));
}

/*---------------------------------------------------------*\
| Open \\.\<name><UID> and the driver's SPB target          |
\*---------------------------------------------------------*/
bool ITE82901Controller::OpenLocked()
{
    if(dev_handle != INVALID_HANDLE_VALUE)
    {
        return(true);
    }

    HANDLE handle = CreateFileA(device_path.c_str(),
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED,
                                NULL);

    if(handle == INVALID_HANDLE_VALUE)
    {
        LOG_ERROR("[ITE82901] Could not open %s (error %lu)", device_path.c_str(), GetLastError());
        return(false);
    }

    dev_handle = handle;

    DWORD transferred = 0;

    if(!Overlapped(ITE_SPB_OP_IOCTL, ITE_SPB_IOCTL_OPEN, NULL, 0, NULL, 0, &transferred))
    {
        LOG_ERROR("[ITE82901] IOCTL_SPBTESTTOOL_OPEN failed on %s (error %lu)", device_path.c_str(), GetLastError());
        CloseHandle(dev_handle);
        dev_handle = INVALID_HANDLE_VALUE;
        return(false);
    }

    LOG_INFO("[ITE82901] Opened %s", device_path.c_str());
    return(true);
}

void ITE82901Controller::CloseLocked()
{
    if(dev_handle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD transferred = 0;
    Overlapped(ITE_SPB_OP_IOCTL, ITE_SPB_IOCTL_CLOSE, NULL, 0, NULL, 0, &transferred);

    CloseHandle(dev_handle);
    dev_handle = INVALID_HANDLE_VALUE;
}

/*---------------------------------------------------------*\
| Issue one overlapped WriteFile / ReadFile / IOCTL and     |
| wait for it to complete (with timeout)                    |
\*---------------------------------------------------------*/
bool ITE82901Controller::Overlapped(int op, DWORD ioctl, void* in, DWORD in_len, void* out, DWORD out_len, DWORD* transferred)
{
    OVERLAPPED  overlap;
    BOOL        started;

    ZeroMemory(&overlap, sizeof(overlap));
    overlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if(overlap.hEvent == NULL)
    {
        return(false);
    }

    *transferred = 0;

    if(op == ITE_SPB_OP_WRITE)
    {
        started = WriteFile(dev_handle, in, in_len, NULL, &overlap);
    }
    else if(op == ITE_SPB_OP_READ)
    {
        started = ReadFile(dev_handle, out, out_len, NULL, &overlap);
    }
    else
    {
        started = DeviceIoControl(dev_handle, ioctl, in, in_len, out, out_len, NULL, &overlap);
    }

    if(!started && GetLastError() != ERROR_IO_PENDING)
    {
        CloseHandle(overlap.hEvent);
        return(false);
    }

    /*-----------------------------------------------------*\
    | Do not hang OpenRGB if the driver never completes     |
    \*-----------------------------------------------------*/
    if(WaitForSingleObject(overlap.hEvent, ITE_SPB_IO_TIMEOUT_MS) != WAIT_OBJECT_0)
    {
        CancelIoEx(dev_handle, &overlap);
    }

    BOOL ok = GetOverlappedResult(dev_handle, &overlap, transferred, TRUE);

    CloseHandle(overlap.hEvent);

    return(ok == TRUE);
}

/*---------------------------------------------------------*\
| Write with retries (same policy as the ITE reference DLL) |
| and automatic re-open after a failure                     |
\*---------------------------------------------------------*/
bool ITE82901Controller::WriteLocked(const unsigned char* data, int len)
{
    if(len <= 0 || len > ITE_SPB_MAX_TRANSFER)
    {
        return(false);
    }

    unsigned char buf[ITE_SPB_MAX_TRANSFER];
    memcpy(buf, data, len);

    for(int attempt = 0; attempt < ITE_SPB_IO_RETRIES; attempt++)
    {
        if(!OpenLocked())
        {
            return(false);
        }

        DWORD transferred = 0;

        if(Overlapped(ITE_SPB_OP_WRITE, 0, buf, (DWORD)len, NULL, 0, &transferred))
        {
            return(true);
        }
    }

    LOG_ERROR("[ITE82901] Write of command 0x%02X failed after %d attempts (error %lu), re-opening on next use", buf[0], ITE_SPB_IO_RETRIES, GetLastError());

    /*-----------------------------------------------------*\
    | Handle may be stale (e.g. after sleep/resume)         |
    \*-----------------------------------------------------*/
    CloseLocked();
    return(false);
}

/*---------------------------------------------------------*\
| Combined write + repeated-start read through the driver's |
| IOCTL_SPBTESTTOOL_WRITEREAD (input = write, output = read)|
\*---------------------------------------------------------*/
bool ITE82901Controller::WriteReadLocked(unsigned char* write_data, int write_len, unsigned char* read_data, int read_len, int* read_count)
{
    *read_count = 0;

    if(!OpenLocked())
    {
        return(false);
    }

    DWORD transferred = 0;

    if(!Overlapped(ITE_SPB_OP_IOCTL, ITE_SPB_IOCTL_WRITEREAD, write_data, (DWORD)write_len, read_data, (DWORD)read_len, &transferred))
    {
        return(false);
    }

    *read_count = (int)transferred;

    return(transferred > 0);
}
