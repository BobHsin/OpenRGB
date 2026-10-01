/*---------------------------------------------------------*\
| ITE82901Controller_Windows.cpp                            |
|                                                           |
|   Driver for ITE 82901 LED controller over PCH I2C,       |
|   through the ITE SPB peripheral kernel driver (IOCTL)    |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <cstdio>
#include <cstring>
#include "ITE82901Controller_Windows.h"
#include "LogManager.h"

/*---------------------------------------------------------*\
| Overlapped operation types                                |
\*---------------------------------------------------------*/
enum
{
    ITE_SPB_OP_WRITE    = 0,
    ITE_SPB_OP_READ     = 1,
    ITE_SPB_OP_IOCTL    = 2,
};

ITE82901Controller::ITE82901Controller(std::string dev_name, std::string driver_name, int driver_uid)
{
    char path[128];

    snprintf(path, sizeof(path), "\\\\.\\%s%d", driver_name.c_str(), driver_uid);

    name        = dev_name;
    device_path = path;
    dev_handle  = INVALID_HANDLE_VALUE;
}

ITE82901Controller::~ITE82901Controller()
{
    std::lock_guard<std::mutex> lock(io_mutex);
    CloseLocked();
}

bool ITE82901Controller::Open()
{
    std::lock_guard<std::mutex> lock(io_mutex);
    return(OpenLocked());
}

bool ITE82901Controller::IsOpen()
{
    std::lock_guard<std::mutex> lock(io_mutex);
    return(dev_handle != INVALID_HANDLE_VALUE);
}

std::string ITE82901Controller::GetName()
{
    return(name);
}

std::string ITE82901Controller::GetLocation()
{
    return("ITE SPB driver: " + device_path);
}

/*---------------------------------------------------------*\
| Packet: [ 0x22 ][ pattern index ]                         |
\*---------------------------------------------------------*/
bool ITE82901Controller::SetPattern(unsigned char pattern)
{
    unsigned char packet[2] = { ITE82901_CMD_PATTERN_SET, pattern };

    bool ok = Write(packet, sizeof(packet));

    if(!ok)
    {
        LOG_ERROR("[ITE82901] Failed to write pattern 0x%02X via %s", pattern, device_path.c_str());
    }

    return(ok);
}

/*---------------------------------------------------------*\
| 1-byte read, used only to check the device responds       |
\*---------------------------------------------------------*/
bool ITE82901Controller::ProbeRead()
{
    unsigned char data[1] = { 0 };

    return(Read(data, sizeof(data)));
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
bool ITE82901Controller::Write(const unsigned char* data, int len)
{
    std::lock_guard<std::mutex> lock(io_mutex);

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

    LOG_ERROR("[ITE82901] Write failed after %d attempts (error %lu), re-opening on next use", ITE_SPB_IO_RETRIES, GetLastError());

    /*-----------------------------------------------------*\
    | Handle may be stale (e.g. after sleep/resume)         |
    \*-----------------------------------------------------*/
    CloseLocked();
    return(false);
}

bool ITE82901Controller::Read(unsigned char* data, int len)
{
    std::lock_guard<std::mutex> lock(io_mutex);

    if(len <= 0 || len > ITE_SPB_MAX_TRANSFER)
    {
        return(false);
    }

    for(int attempt = 0; attempt < ITE_SPB_IO_RETRIES; attempt++)
    {
        if(!OpenLocked())
        {
            return(false);
        }

        DWORD transferred = 0;

        if(Overlapped(ITE_SPB_OP_READ, 0, NULL, 0, data, (DWORD)len, &transferred) && (int)transferred == len)
        {
            return(true);
        }
    }

    return(false);
}
