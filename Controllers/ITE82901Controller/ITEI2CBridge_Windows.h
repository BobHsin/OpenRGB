/*---------------------------------------------------------*\
| ITEI2CBridge_Windows.h                                    |
|                                                           |
|   Runtime loader / thread-safe wrapper for ITE's          |
|   ITEI2CBridge.dll (WinRT Windows.Devices.I2c bridge)     |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later                   |
\*---------------------------------------------------------*/

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/*---------------------------------------------------------*\
| Notes on ITEI2CBridge.dll behaviour (from inspecting the  |
| x64 binary):                                              |
|                                                           |
|  - bus          : index into the device list returned by  |
|                   DeviceInformation::FindAllAsync(         |
|                   I2cDevice::GetDeviceSelector()). i.e.    |
|                   0 = first I2C controller exposed to      |
|                   WinRT, 1 = second, ...                   |
|  - slaveAddress : 7-bit I2C address                       |
|  - speed        : kHz. 400 -> FastMode, anything else ->  |
|                   StandardMode (100 kHz)                   |
|  - The DLL holds ONE open device globally.                |
|  - initialdll() calls CoInitializeEx(STA) on the first    |
|    thread that uses it and throws C++ exceptions on       |
|    HRESULT failures.                                      |
|                                                           |
| Because of the last two points every DLL call is executed |
| on one private worker thread owned by this class, and the |
| wrapper re-opens the device whenever a different          |
| bus/address/speed is requested.                           |
\*---------------------------------------------------------*/

class ITEI2CBridge
{
public:
    static ITEI2CBridge*    Get();

    bool                    Load(const std::string& dll_name);
    bool                    IsLoaded();

    bool                    Open(int bus, int address, int speed_khz);
    bool                    Write(int bus, int address, int speed_khz, const std::vector<unsigned char>& data);
    bool                    Read(int bus, int address, int speed_khz, std::vector<unsigned char>& data);

private:
    ITEI2CBridge();
    ~ITEI2CBridge();

    bool                    RunOnWorker(std::function<bool()> task);
    void                    WorkerThreadFunction();
    bool                    EnsureOpenLocked(int bus, int address, int speed_khz);

    typedef bool (*pfnInitialDll)(int bus, int slaveAddress, int speed);
    typedef bool (*pfnWriteBlock)(unsigned char* WData, int WDataLength);
    typedef bool (*pfnReadBlock)(unsigned char* RData, int RDataLength);
    typedef bool (*pfnWrite2ReadBlock)(int WDataLength, unsigned char* WData, int RDataLength, unsigned char* RData);
    typedef void (*pfnUninitialDll)();

    void*                   dll_handle;
    pfnInitialDll           fn_initialdll;
    pfnWriteBlock           fn_writeblock;
    pfnReadBlock            fn_readblock;
    pfnWrite2ReadBlock      fn_write2readblock;
    pfnUninitialDll         fn_uninitialdll;

    /*-----------------------------------------------------*\
    | Currently opened device (only touched on worker)      |
    \*-----------------------------------------------------*/
    bool                    is_open;
    int                     open_bus;
    int                     open_address;
    int                     open_speed;

    /*-----------------------------------------------------*\
    | Worker thread / task queue                            |
    \*-----------------------------------------------------*/
    std::mutex                          api_mutex;
    std::mutex                          queue_mutex;
    std::condition_variable             queue_cv;
    std::deque<std::function<void()>>   queue;
    std::thread*                        worker;
};
