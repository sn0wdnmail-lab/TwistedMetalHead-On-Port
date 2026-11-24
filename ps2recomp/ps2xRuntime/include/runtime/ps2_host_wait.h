#pragma once

// Precise host waits for the EE scheduler.
//
// Windows condition-variable timeouts end on a system timer tick (1-16 ms), so the scheduler's
// microsecond pacing waits overshoot badly. On Windows this uses a high-resolution waitable timer
// plus a wake event; elsewhere it is unused (the standard waits are precise there).

#include <chrono>

namespace ps2host
{
    // True when waitUntil() gives sub-millisecond wake-ups on this platform.
    bool preciseWaitSupported() noexcept;

    // Block until the deadline or until wake() is called (whichever comes first; may also return
    // early). Call without holding locks that wake() callers need.
    void waitUntil(std::chrono::steady_clock::time_point deadline) noexcept;

    // Release a thread blocked in waitUntil(). A wake() that happens before waitUntil() starts is
    // remembered, so the next waitUntil() returns at once.
    void wake() noexcept;

    // Once per process: tell Windows this is not background work, so it keeps the game on the
    // fast cores of hybrid CPUs (no EcoQoS) and keeps the 1 ms timer while the window is hidden.
    void preferPerformanceCores() noexcept;

    // For the threads that run the game (game, VU1, GS feed, window): slightly above normal
    // priority and never power-throttled, so other programs cannot delay a frame.
    void raiseThisThread() noexcept;
}
