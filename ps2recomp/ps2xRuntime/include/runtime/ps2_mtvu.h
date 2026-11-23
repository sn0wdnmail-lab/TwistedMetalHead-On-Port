#ifndef PS2_MTVU_H
#define PS2_MTVU_H

// The VU1 thread ("MTVU"). With the GPU renderer, the work the EE hands to VIF1 and the GIF runs
// on a second CPU core, in the order the EE issued it: VIF1 packets and the VU1 microprograms
// they start, PATH1/2/3 packets for the GS, the GS privileged register writes and the vsync.
// The EE thread meanwhile carries on with game code - on the PS2, VU1 and the GS also run in
// parallel with the EE.
//
// Everything that reads or changes that state from the EE side either queues behind the work
// already handed over (post) or waits until the VU1 thread has caught up (sync): VU1 memory
// accesses, GIF_STAT reads. DMA completion stays immediate, as before.
//
// PS2X_MTVU=0 (or the settings menu) keeps everything on the EE thread.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace ps2mtvu
{
    extern std::atomic<bool> g_active;

    // Starts the VU1 thread (once). Returns false when it is turned off.
    bool start();
    // Runs what is queued, then stops the thread (EE thread or shutdown).
    void stop();

    inline bool active() noexcept { return g_active.load(std::memory_order_relaxed); }
    bool onWorker() noexcept;

    // Queue a task (EE thread). bytes: size of the data it carries, for back-pressure.
    // On the VU1 thread itself (or when inactive) the task runs immediately.
    void post(std::function<void()> task, size_t bytes = 0u);
    // Queue the task that ends a frame (the vsync). The EE waits here when the VU1 thread is
    // still more than one frame behind, so the picture never lags further than that.
    void postFrame(std::function<void()> task);
    // Wait until every queued task has run (EE thread; no-op on the VU1 thread or when inactive).
    enum class SyncReason : uint32_t
    {
        Vu1Memory = 0,
        GifStatus,
        Other,
        Count
    };
    void sync(SyncReason reason = SyncReason::Other);

    struct Stats
    {
        uint64_t tasks = 0;
        uint64_t frames = 0;
        uint64_t syncs[static_cast<uint32_t>(SyncReason::Count)] = {};
        uint64_t syncWaitNs = 0;  // EE waiting in sync()
        uint64_t frameWaitNs = 0; // EE waiting because the VU1 thread was a frame behind
        uint64_t fullWaitNs = 0;  // EE waiting because the queue was full
        uint64_t queuedBytes = 0; // data handed over
        uint64_t wakeups = 0;     // times the VU1 thread went to sleep for lack of work (each one is woken later)
    };
    // Counters since the last call (performance log).
    Stats takeStats() noexcept;
}

#endif
