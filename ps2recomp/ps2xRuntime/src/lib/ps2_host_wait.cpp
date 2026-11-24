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
#include <setupapi.h>
#include <hidsdi.h>
#include <cstdint>
#include <cstring>
#include <vector>
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

    namespace
    {
        // ---- PlayStation controllers (V9). A DualShock 4 or DualSense on Windows is a HID device,
        // not an XInput one: its motors are set with an output report (on Bluetooth with a CRC-32
        // at the end, over the 0xA2 HID header and the report). hid.dll and setupapi.dll are
        // loaded when first needed.
        struct SonyPad
        {
            HANDLE handle = INVALID_HANDLE_VALUE;
            bool dualSense = false;
            bool bluetooth = false;
            USHORT outLength = 0;
        };

        struct HidApi
        {
            void(__stdcall *getHidGuid)(GUID *) = nullptr;
            BOOLEAN(__stdcall *getAttributes)(HANDLE, PHIDD_ATTRIBUTES) = nullptr;
            BOOLEAN(__stdcall *getPreparsedData)(HANDLE, PHIDP_PREPARSED_DATA *) = nullptr;
            BOOLEAN(__stdcall *freePreparsedData)(PHIDP_PREPARSED_DATA) = nullptr;
            NTSTATUS(__stdcall *getCaps)(PHIDP_PREPARSED_DATA, PHIDP_CAPS) = nullptr;
            BOOLEAN(__stdcall *setOutputReport)(HANDLE, PVOID, ULONG) = nullptr;
            HDEVINFO(WINAPI *getClassDevs)(const GUID *, PCWSTR, HWND, DWORD) = nullptr;
            BOOL(WINAPI *enumInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA) = nullptr;
            BOOL(WINAPI *interfaceDetail)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA) = nullptr;
            BOOL(WINAPI *destroyList)(HDEVINFO) = nullptr;
            bool ok = false;
        };

        const HidApi &hidApi()
        {
            static const HidApi api = []
            {
                HidApi a;
                HMODULE hid = LoadLibraryA("hid.dll");
                HMODULE setup = LoadLibraryA("setupapi.dll");
                if (!hid || !setup)
                    return a;
                auto get = [](HMODULE module, const char *name) { return reinterpret_cast<void *>(GetProcAddress(module, name)); };
                a.getHidGuid = reinterpret_cast<decltype(a.getHidGuid)>(get(hid, "HidD_GetHidGuid"));
                a.getAttributes = reinterpret_cast<decltype(a.getAttributes)>(get(hid, "HidD_GetAttributes"));
                a.getPreparsedData = reinterpret_cast<decltype(a.getPreparsedData)>(get(hid, "HidD_GetPreparsedData"));
                a.freePreparsedData = reinterpret_cast<decltype(a.freePreparsedData)>(get(hid, "HidD_FreePreparsedData"));
                a.getCaps = reinterpret_cast<decltype(a.getCaps)>(get(hid, "HidP_GetCaps"));
                a.setOutputReport = reinterpret_cast<decltype(a.setOutputReport)>(get(hid, "HidD_SetOutputReport"));
                a.getClassDevs = reinterpret_cast<decltype(a.getClassDevs)>(get(setup, "SetupDiGetClassDevsW"));
                a.enumInterfaces = reinterpret_cast<decltype(a.enumInterfaces)>(get(setup, "SetupDiEnumDeviceInterfaces"));
                a.interfaceDetail = reinterpret_cast<decltype(a.interfaceDetail)>(get(setup, "SetupDiGetDeviceInterfaceDetailW"));
                a.destroyList = reinterpret_cast<decltype(a.destroyList)>(get(setup, "SetupDiDestroyDeviceInfoList"));
                a.ok = a.getHidGuid && a.getAttributes && a.getPreparsedData && a.freePreparsedData && a.getCaps && a.setOutputReport &&
                       a.getClassDevs && a.enumInterfaces && a.interfaceDetail && a.destroyList;
                return a;
            }();
            return api;
        }

        uint32_t crc32(uint32_t crc, const uint8_t *data, size_t size)
        {
            crc = ~crc;
            for (size_t i = 0; i < size; ++i)
            {
                crc ^= data[i];
                for (int k = 0; k < 8; ++k)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        }

        bool findSonyPad(SonyPad &pad)
        {
            const HidApi &api = hidApi();
            if (!api.ok)
                return false;
            GUID hidGuid{};
            api.getHidGuid(&hidGuid);
            HDEVINFO list = api.getClassDevs(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (list == INVALID_HANDLE_VALUE)
                return false;
            for (DWORD index = 0;; ++index)
            {
                SP_DEVICE_INTERFACE_DATA iface{};
                iface.cbSize = sizeof(iface);
                if (!api.enumInterfaces(list, nullptr, &hidGuid, index, &iface))
                    break;
                DWORD size = 0;
                api.interfaceDetail(list, &iface, nullptr, 0, &size, nullptr);
                if (size == 0)
                    continue;
                std::vector<uint8_t> buffer(size + sizeof(WCHAR));
                auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
                if (!api.interfaceDetail(list, &iface, detail, size, nullptr, nullptr))
                    continue;
                HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                    continue;
                HIDD_ATTRIBUTES attributes{};
                attributes.Size = sizeof(attributes);
                bool keep = false;
                if (api.getAttributes(handle, &attributes) && attributes.VendorID == 0x054Cu)
                {
                    const USHORT product = attributes.ProductID;
                    const bool ds4 = product == 0x05C4u || product == 0x09CCu || product == 0x0BA0u;
                    const bool ds5 = product == 0x0CE6u || product == 0x0DF2u;
                    PHIDP_PREPARSED_DATA preparsed = nullptr;
                    if ((ds4 || ds5) && api.getPreparsedData(handle, &preparsed))
                    {
                        HIDP_CAPS caps{};
                        api.getCaps(preparsed, &caps);
                        api.freePreparsedData(preparsed);
                        if (caps.OutputReportByteLength > 0)
                        {
                            pad.handle = handle;
                            pad.dualSense = ds5;
                            pad.outLength = caps.OutputReportByteLength;
                            // Over USB the output report is 32 (DualShock 4) or 48 (DualSense) bytes.
                            pad.bluetooth = caps.OutputReportByteLength != (ds5 ? 48u : 32u);
                            keep = true;
                        }
                    }
                }
                if (keep)
                    break;
                CloseHandle(handle);
            }
            api.destroyList(list);
            return pad.handle != INVALID_HANDLE_VALUE;
        }

        // strong: the big low-frequency motor (left), weak: the small one (right); 0..255.
        bool sendSonyRumble(const SonyPad &pad, uint8_t strong, uint8_t weak)
        {
            std::vector<uint8_t> report(std::max<size_t>(pad.outLength, 78u), 0u);
            size_t size = 0;
            if (!pad.dualSense)
            {
                if (!pad.bluetooth)
                {
                    report[0] = 0x05; // USB effects report
                    report[1] = 0x01; // rumble only (lightbar untouched)
                    report[4] = weak;
                    report[5] = strong;
                    size = 32;
                }
                else
                {
                    report[0] = 0x11; // Bluetooth effects report
                    report[1] = 0xC0; // HID + CRC
                    report[3] = 0x01; // rumble only
                    report[6] = weak;
                    report[7] = strong;
                    size = 78;
                }
            }
            else
            {
                size_t at = 1;
                if (!pad.bluetooth)
                {
                    report[0] = 0x02; // USB effects report
                    size = 48;
                }
                else
                {
                    report[0] = 0x31; // Bluetooth effects report
                    report[1] = 0x02;
                    at = 2;
                    size = 78;
                }
                report[at + 0] = 0x01 | 0x02; // rumble the classic way (not the audio haptics)
                report[at + 2] = weak;
                report[at + 3] = strong;
            }
            if (pad.bluetooth)
            {
                const uint8_t header = 0xA2;
                uint32_t crc = crc32(0, &header, 1);
                crc = crc32(crc, report.data(), size - 4);
                std::memcpy(report.data() + size - 4, &crc, 4);
            }
            DWORD written = 0;
            const DWORD length = pad.outLength ? pad.outLength : static_cast<DWORD>(size);
            if (WriteFile(pad.handle, report.data(), length, &written, nullptr))
                return true;
            return hidApi().setOutputReport(pad.handle, report.data(), static_cast<ULONG>(size)) != FALSE;
        }

        void sonyRumble(float left, float right)
        {
            static SonyPad pad;
            static ULONGLONG lookedAt = 0;
            static uint8_t sentStrong = 0, sentWeak = 0;
            const ULONGLONG now = GetTickCount64();
            if (pad.handle == INVALID_HANDLE_VALUE)
            {
                if (now - lookedAt < 3000u && lookedAt != 0)
                    return;
                lookedAt = now;
                if (!findSonyPad(pad))
                    return;
            }
            auto toByte = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            const uint8_t strong = toByte(left), weak = toByte(right);
            (void)sentStrong;
            (void)sentWeak;
            if (!sendSonyRumble(pad, strong, weak))
            {
                CloseHandle(pad.handle);
                pad = SonyPad{};
                lookedAt = now;
                return;
            }
            sentStrong = strong;
            sentWeak = weak;
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
        {
            sonyRumble(left, right);
            return;
        }
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
        {
            sonyRumble(left, right); // no Xbox-style controller: a PlayStation one, maybe
            return;
        }
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
