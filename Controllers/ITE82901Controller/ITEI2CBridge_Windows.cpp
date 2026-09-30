/*---------------------------------------------------------*\
| ITEI2CBridge_Windows.cpp                                  |
|                                                           |
|   Runtime loader / thread-safe wrapper for ITE's          |
|   ITEI2CBridge.dll (WinRT Windows.Devices.I2c bridge)     |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later                   |
\*---------------------------------------------------------*/

#include <windows.h>
#include <future>
#include "ITEI2CBridge_Windows.h"
#include "LogManager.h"

ITEI2CBridge* ITEI2CBridge::Get()
{
    /*-----------------------------------------------------*\
    | Intentionally leaked: the worker thread must not be   |
    | joined from a static destructor during process exit.  |
    \*-----------------------------------------------------*/
    static ITEI2CBridge* instance = new ITEI2CBridge();
    return instance;
}

ITEI2CBridge::ITEI2CBridge()
{
    dll_handle          = NULL;
    fn_initialdll       = NULL;
    fn_writeblock       = NULL;
    fn_readblock        = NULL;
    fn_write2readblock  = NULL;
    fn_uninitialdll     = NULL;

    is_open             = false;
    open_bus            = -1;
    open_address        = -1;
    open_speed          = -1;

    worker              = new std::thread(&ITEI2CBridge::WorkerThreadFunction, this);
}

ITEI2CBridge::~ITEI2CBridge()
{
    /*-----------------------------------------------------*\
    | Never called (singleton is leaked on purpose)         |
    \*-----------------------------------------------------*/
}

void ITEI2CBridge::WorkerThreadFunction()
{
    while(true)
    {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            queue_cv.wait(lock, [this]{ return !queue.empty(); });
            task = std::move(queue.front());
            queue.pop_front();
        }

        task();
    }
}

bool ITEI2CBridge::RunOnWorker(std::function<bool()> task)
{
    std::shared_ptr<std::promise<bool>> result = std::make_shared<std::promise<bool>>();
    std::future<bool>                   future = result->get_future();

    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        queue.push_back([task, result]()
        {
            bool ok = false;

            /*---------------------------------------------*\
            | The DLL throws C++/WinRT exceptions on        |
            | HRESULT failure; never let them escape.       |
            \*---------------------------------------------*/
            try
            {
                ok = task();
            }
            catch(...)
            {
                LOG_ERROR("[ITEI2CBridge] Exception thrown inside ITEI2CBridge.dll");
                ok = false;
            }

            result->set_value(ok);
        });
    }
    queue_cv.notify_one();

    return future.get();
}

bool ITEI2CBridge::Load(const std::string& dll_name)
{
    std::lock_guard<std::mutex> lock(api_mutex);

    if(dll_handle != NULL)
    {
        return true;
    }

    /*-----------------------------------------------------*\
    | Prefer the DLL next to OpenRGB.exe unless an absolute |
    | path was given                                        |
    \*-----------------------------------------------------*/
    std::string path = dll_name;

    if(dll_name.find(':') == std::string::npos && dll_name.find("\\\\") != 0)
    {
        char exe_path[MAX_PATH] = { 0 };
        DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);

        if(len > 0 && len < MAX_PATH)
        {
            std::string exe_dir(exe_path);
            size_t      slash = exe_dir.find_last_of("\\/");

            if(slash != std::string::npos)
            {
                path = exe_dir.substr(0, slash + 1) + dll_name;
            }
        }
    }

    HMODULE module = LoadLibraryExA(path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

    if(module == NULL)
    {
        LOG_ERROR("[ITEI2CBridge] Failed to load %s (error %lu)", path.c_str(), GetLastError());
        return false;
    }

    fn_initialdll       = (pfnInitialDll)     (void*)GetProcAddress(module, "initialdll");
    fn_writeblock       = (pfnWriteBlock)     (void*)GetProcAddress(module, "writeblock");
    fn_readblock        = (pfnReadBlock)      (void*)GetProcAddress(module, "readblock");
    fn_write2readblock  = (pfnWrite2ReadBlock)(void*)GetProcAddress(module, "write2readblock");
    fn_uninitialdll     = (pfnUninitialDll)   (void*)GetProcAddress(module, "uninitialdll");

    if(!fn_initialdll || !fn_writeblock || !fn_readblock || !fn_uninitialdll)
    {
        LOG_ERROR("[ITEI2CBridge] %s is missing required exports", path.c_str());
        FreeLibrary(module);
        fn_initialdll = NULL; fn_writeblock = NULL; fn_readblock = NULL;
        fn_write2readblock = NULL; fn_uninitialdll = NULL;
        return false;
    }

    dll_handle = module;
    LOG_INFO("[ITEI2CBridge] Loaded %s", path.c_str());
    return true;
}

bool ITEI2CBridge::IsLoaded()
{
    std::lock_guard<std::mutex> lock(api_mutex);
    return dll_handle != NULL;
}

/*---------------------------------------------------------*\
| Must be called on the worker thread with api_mutex held   |
\*---------------------------------------------------------*/
bool ITEI2CBridge::EnsureOpenLocked(int bus, int address, int speed_khz)
{
    if(is_open && open_bus == bus && open_address == address && open_speed == speed_khz)
    {
        return true;
    }

    if(is_open)
    {
        fn_uninitialdll();
        is_open = false;
    }

    if(!fn_initialdll(bus, address, speed_khz))
    {
        LOG_ERROR("[ITEI2CBridge] initialdll(bus=%d, addr=0x%02X, speed=%d) failed", bus, address, speed_khz);
        return false;
    }

    is_open      = true;
    open_bus     = bus;
    open_address = address;
    open_speed   = speed_khz;
    return true;
}

bool ITEI2CBridge::Open(int bus, int address, int speed_khz)
{
    std::lock_guard<std::mutex> lock(api_mutex);

    if(dll_handle == NULL)
    {
        return false;
    }

    return RunOnWorker([=]()
    {
        return EnsureOpenLocked(bus, address, speed_khz);
    });
}

bool ITEI2CBridge::Write(int bus, int address, int speed_khz, const std::vector<unsigned char>& data)
{
    std::lock_guard<std::mutex> lock(api_mutex);

    if(dll_handle == NULL || data.empty())
    {
        return false;
    }

    std::vector<unsigned char> buf = data;

    return RunOnWorker([&]()
    {
        if(!EnsureOpenLocked(bus, address, speed_khz))
        {
            return false;
        }

        if(!fn_writeblock(buf.data(), (int)buf.size()))
        {
            /*-------------------------------------------------*\
            | Force a re-open next time in case the bus handle  |
            | went stale (e.g. after sleep/resume)              |
            \*-------------------------------------------------*/
            fn_uninitialdll();
            is_open = false;
            return false;
        }

        return true;
    });
}

bool ITEI2CBridge::Read(int bus, int address, int speed_khz, std::vector<unsigned char>& data)
{
    std::lock_guard<std::mutex> lock(api_mutex);

    if(dll_handle == NULL || data.empty())
    {
        return false;
    }

    return RunOnWorker([&]()
    {
        if(!EnsureOpenLocked(bus, address, speed_khz))
        {
            return false;
        }

        return fn_readblock(data.data(), (int)data.size());
    });
}
