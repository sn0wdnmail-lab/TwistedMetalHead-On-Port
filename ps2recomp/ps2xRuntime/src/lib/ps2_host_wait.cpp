#include "runtime/ps2_host_wait.h"

#include <algorithm>
#include <cmath>

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

    namespace
    {
        // PROCESS_POWER_THROTTLING_STATE / THREAD_POWER_THROTTLING_STATE (Windows 10 1709+),
        // spelled out so older SDK headers build too.
        struct PowerThrottlingState
        {
            ULONG version;
            ULONG controlMask;
            ULONG stateMask;
        };
        constexpr ULONG kThrottlingVersion = 1u;
        constexpr ULONG kExecutionSpeed = 0x1u;       // EcoQoS / efficiency-core placement
        constexpr ULONG kIgnoreTimerResolution = 0x4u; // process only: keep timeBeginPeriod(1)
        constexpr int kProcessPowerThrottling = 4;    // PROCESS_INFORMATION_CLASS
        constexpr int kThreadPowerThrottling = 3;     // THREAD_INFORMATION_CLASS

        HMODULE kernel32()
        {
            static const HMODULE module = GetModuleHandleW(L"kernel32.dll");
            return module;
        }
    }

    void preferPerformanceCores() noexcept
    {
        using SetProcessInformationFn = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
        const auto fn = kernel32() ? reinterpret_cast<SetProcessInformationFn>(
                                         reinterpret_cast<void *>(GetProcAddress(kernel32(), "SetProcessInformation")))
                                   : nullptr;
        if (!fn)
            return; // before Windows 8: nothing to opt out of
        PowerThrottlingState state{kThrottlingVersion, kExecutionSpeed | kIgnoreTimerResolution, 0u};
        if (!fn(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state)))
        {
            // Windows 10 before 2004 knows execution speed but not the timer flag.
            state.controlMask = kExecutionSpeed;
            fn(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state));
        }
    }

    void raiseThisThread() noexcept
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        using SetThreadInformationFn = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
        const auto fn = kernel32() ? reinterpret_cast<SetThreadInformationFn>(
                                         reinterpret_cast<void *>(GetProcAddress(kernel32(), "SetThreadInformation")))
                                   : nullptr;
        if (fn)
        {
            PowerThrottlingState state{kThrottlingVersion, kExecutionSpeed, 0u};
            fn(GetCurrentThread(), kThreadPowerThrottling, &state, sizeof(state));
        }
    }

    void setPadVibration(float left, float right) noexcept
    {
        struct Vibration
        {
            WORD left;
            WORD right;
        };
        struct GamepadState
        {
            DWORD packet;
            WORD buttons;
            BYTE leftTrigger, rightTrigger;
            SHORT thumbLX, thumbLY, thumbRX, thumbRY;
        };
        using SetStateFn = DWORD(WINAPI *)(DWORD, Vibration *);
        using GetStateFn = DWORD(WINAPI *)(DWORD, GamepadState *);
        static SetStateFn setState = nullptr;
        static GetStateFn getState = nullptr;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            for (const char *dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
            {
                if (HMODULE module = LoadLibraryA(dll))
                {
                    setState = reinterpret_cast<SetStateFn>(reinterpret_cast<void *>(GetProcAddress(module, "XInputSetState")));
                    getState = reinterpret_cast<GetStateFn>(reinterpret_cast<void *>(GetProcAddress(module, "XInputGetState")));
                    if (setState && getState)
                        break;
                    setState = nullptr;
                    getState = nullptr;
                }
            }
        }
        if (!setState || !getState)
            return;
        // The first connected controller, looked for again every couple of seconds (and whenever
        // the one in use stops answering).
        static DWORD user = 0xFFFFFFFFu;
        static ULONGLONG lookedAt = 0;
        const ULONGLONG now = GetTickCount64();
        if (user == 0xFFFFFFFFu && now - lookedAt > 2000u)
        {
            lookedAt = now;
            for (DWORD i = 0; i < 4u; ++i)
            {
                GamepadState state{};
                if (getState(i, &state) == ERROR_SUCCESS)
                {
                    user = i;
                    break;
                }
            }
        }
        if (user == 0xFFFFFFFFu)
            return;
        auto toMotor = [](float v) { return static_cast<WORD>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)); };
        Vibration vibration{toMotor(left), toMotor(right)};
        if (setState(user, &vibration) != ERROR_SUCCESS)
            user = 0xFFFFFFFFu;
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

    void preferPerformanceCores() noexcept
    {
    }

    void raiseThisThread() noexcept
    {
    }

    void setPadVibration(float, float) noexcept {}
}

#endif
