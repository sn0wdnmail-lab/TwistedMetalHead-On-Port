// Closing the game at once - see include/runtime/ps2_exit.h.

#include "runtime/ps2_exit.h"
#include "runtime/ps2_android.h"
#include "runtime/ps2_perf.h"
#include "runtime/ps2_settings.h"
#include "runtime/gs/gs_gpu_renderer.h"

#include "raylib.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

#if defined(_WIN32)
// Declared here instead of including <windows.h>, whose names clash with raylib's.
extern "C" __declspec(dllimport) void *__stdcall GetCurrentProcess(void);
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void *process, unsigned int exitCode);
#endif

namespace ps2exit
{
#if !defined(_WIN32)
    void watchWindowClose(void *) {}
    bool closeAsked() noexcept { return false; }
#endif

    [[noreturn]] void now(int code)
    {
        static std::atomic<bool> closing{false};
        if (closing.exchange(true))
        {
            // Another call is already closing the game.
            for (;;)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        const auto started = std::chrono::steady_clock::now();
        if (!ps2android::active())
        {
            // Gone from the screen and silent straight away; the rest happens out of sight.
            if (IsAudioDeviceReady())
                SetMasterVolume(0.0f);
            if (IsWindowReady())
            {
                if (IsWindowFullscreen())
                    MinimizeWindow(); // gives the monitor its own display mode back first
                SetWindowState(FLAG_WINDOW_HIDDEN);
            }
        }
        ps2settings::saveForExit();
        ps2gpu::saveShaderCacheForExit(1500);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        std::fprintf(stderr, "[exit] closing: settings and shaders saved in %.0f ms; ending the process\n", ms);
        ps2perf::shutdown(); // the session summary at the end of the logs
        std::cout.flush();
        std::cerr.flush();
        std::fflush(nullptr);
#if defined(_WIN32)
        // Skips the DLL detach routines ExitProcess runs: a graphics driver's or the audio
        // thread's can wait forever and leave a frozen window behind.
        TerminateProcess(GetCurrentProcess(), static_cast<unsigned int>(code));
#endif
#if defined(__ANDROID__)
        // Close the app's screen first, or Android may start it again when the process ends.
        ps2android::finishActivity();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
#endif
        std::_Exit(code);
    }
}
