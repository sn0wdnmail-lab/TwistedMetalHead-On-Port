#include "runtime/ps2_host_wait.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace ps2host
{
    namespace
    {
        struct Handles
        {
            HANDLE timer = nullptr;
            HANDLE wakeEvent = nullptr;
            bool highResolution = false;

            Handles()
            {
                timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                highResolution = timer != nullptr;
                if (!timer)
                    timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            }
        };

        Handles &handles()
        {
            static Handles instance;
            return instance;
        }
    }

    bool preciseWaitSupported() noexcept
    {
        return handles().timer != nullptr && handles().wakeEvent != nullptr;
    }

    void waitUntil(std::chrono::steady_clock::time_point deadline) noexcept
    {
        Handles &h = handles();
        const auto now = std::chrono::steady_clock::now();
        if (deadline <= now)
            return;
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(remaining / 100); // relative, 100 ns units
        if (due.QuadPart == 0)
            return;
        if (!h.timer || !SetWaitableTimerEx(h.timer, &due, 0, nullptr, nullptr, nullptr, 0))
        {
            Sleep(0);
            return;
        }
        HANDLE objects[2] = {h.wakeEvent, h.timer};
        WaitForMultipleObjects(h.wakeEvent ? 2u : 1u, h.wakeEvent ? objects : objects + 1, FALSE, INFINITE);
    }

    void wake() noexcept
    {
        if (HANDLE event = handles().wakeEvent)
            SetEvent(event);
    }
}

#else

#include <thread>

namespace ps2host
{
    bool preciseWaitSupported() noexcept
    {
        return false;
    }

    void waitUntil(std::chrono::steady_clock::time_point deadline) noexcept
    {
        std::this_thread::sleep_until(deadline);
    }

    void wake() noexcept
    {
    }
}

#endif
