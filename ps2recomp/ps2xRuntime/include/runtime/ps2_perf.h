#pragma once

// Performance and graphics accounting.
//
// On by default (PS2X_PERF=0 turns it off). Every two seconds the main thread writes one report to
//   <port>\Logs\performancelogs.txt  - speed, where the game thread's time goes, CPU use per thread
//   <port>\Logs\Graphicslogs.txt     - which device renders, GS work per second, GPU time
// (the folder is next to bin\, or set PS2X_LOG_DIR). PS2X_PERF=2 also prints the reports to stderr.
//
// Each Scope adds its *exclusive* time (minus the time of scopes nested inside it on the same
// thread) to one bucket, so the buckets of a thread add up to the time that thread spent in
// instrumented code; the rest of the game thread's wall time is EE game code and scheduling.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace ps2perf
{
    enum Bucket : uint32_t
    {
        // game thread
        Vu1 = 0,
        Vu0,
        Vif1,
        GsPacket,  // GIF packet parse (software frontend, or paraLLEl-GS's gif_transfer)
        GsRaster,  // software GS drawing (the game thread's share plus waiting for the raster workers)
        GpuFrame,  // GPU renderer: flush + scanout + readback bookkeeping at VBlank
        GpuSync,   // GPU renderer: CPU waiting for the GPU (VRAM readbacks, local->host transfers)
        Iop,
        IdleWait,
        PaceWait,
        MtvuWait,  // waiting for the VU1 thread to catch up (ps2_mtvu)
        // VU1 thread (ps2_mtvu): its scopes land here instead of the game thread's buckets
        VuThreadVu1,
        VuThreadVif1,
        VuThreadGs,     // GIF packets into the GS (paraLLEl-GS gif_transfer)
        VuThreadGpu,    // vsync: flush + scanout + readback bookkeeping
        VuThreadIdle,   // waiting for work
        VuThreadOther,
        // main (window) thread
        MainLatch,
        MainRaster,
        MainUpload,
        MainEndDrawing,
        BucketCount
    };

    enum Counter : uint32_t
    {
        IdleWaits = 0,
        IdleRequestedNs,
        IdleOvershootNs,
        IdleOvershootMaxNs,
        PaceWaits,
        PaceRequestedNs,
        PaceOvershootNs,
        PaceOvershootMaxNs,
        VuPairs,
        VuPairsInterpreted, // VU1 pairs run by the interpreter while a compiled image was active
        GifPackets,
        GifBytes,
        RasterFlushes,
        RasterPoolRuns,
        MainFrames,
        DisplayFlips,
        SlowFrames,       // window frames that took longer than 25 ms
        MaxFrameNs,       // longest window frame in the interval
        // GS queue flushes that had work queued, by cause
        FlushTexDependency,
        FlushQueueFull,
        FlushClut,
        FlushExplicit,
        FlushTexflush,
        FlushSync,
        FlushVramAccess,
        FlushCacheEvict,
        FlushTransfer,
        FlushImage,
        FlushLocalToHost,
        FlushClear,
        FlushReset,
        // GS work (software renderer counts these itself; the GPU renderer reports its own below)
        GsPoints,
        GsLines,
        GsTriangles,
        GsSprites,
        GsPixels,
        TexUploads,
        TexUploadBytes,
        ClutLoads,
        LocalToHostReads,
        GsDownloadsPredicted, // small per-frame downloads answered from the frame before
        // GPU renderer
        GpuFrames,
        GpuReadbacks,
        GpuDroppedReadbacks,
        GpuFieldFollows, // interlaced frames shown late or twice, scanned out as the field they were drawn for
        GpuPrimitives,
        GpuRenderPasses,
        GpuCopies,
        GpuPaletteUpdates,
        GpuBusyNs,
        CounterCount
    };

    enum Gauge : uint32_t
    {
        GpuMemoryUsedBytes = 0, // video memory this process uses (VK_EXT_memory_budget)
        GpuMemoryBudgetBytes,   // video memory the driver lets this process use
        GaugeCount
    };

    bool enabled() noexcept;
    void setGauge(Gauge gauge, uint64_t value) noexcept;
    void markMainThread() noexcept; // GS raster work done on this thread is reported separately
    uint64_t nowNs() noexcept;
    void add(Bucket bucket, uint64_t ns) noexcept;
    void count(Counter counter, uint64_t value = 1u) noexcept;
    void countMax(Counter counter, uint64_t value) noexcept;

    // Called once on a thread to have its CPU time reported ("game", "window", "raster worker").
    void registerThread(const char *name) noexcept;

    // Watchdog heartbeats. When the game, VU1 or window thread stops making progress for 5 seconds
    // (not while the F2 drawer pauses the game), the watchdog writes where each of them is (their
    // call stacks on Windows) to the console and performance logs.
    enum class Beat
    {
        Game,    // the game thread reached a VBlank
        VuBegin, // the VU1 thread started a batch of work
        VuEnd,   // ... and finished it
        Closing, // the window closed: the threads are being stopped. If that takes over 10 s the
                 // watchdog logs where they are, writes the session summary and ends the process.
    };
    void heartbeat(Beat beat) noexcept;

    // The Logs folder (created on first use), for other diagnostic output.
    std::string logDirectory();

    // Graphics description, shown at the top of Graphicslogs.txt and on every report line.
    struct RendererInfo
    {
        std::string name;        // "Software (CPU)" / "Vulkan GPU (paraLLEl-GS)"
        std::string device;      // GPU name, or "CPU, 8 raster threads"
        std::string details;     // driver / API version / memory
        bool gpu = false;        // true when the GS is drawn by the GPU
        uint32_t superSampling = 1u;
    };
    void setRenderer(const RendererInfo &info) noexcept;
    void setPresenter(const std::string &description) noexcept;            // OpenGL window that shows the frames
    void setOutputSize(uint32_t gameWidth, uint32_t gameHeight, uint32_t windowWidth, uint32_t windowHeight) noexcept;
    // A stretch of VU1 code that had to be interpreted although its image is compiled: the
    // compiled code had no entry for this pc. The busiest ones are listed in performancelogs.txt
    // (they become extra entry points the next time the programs are generated).
    void vu1CompiledMiss(uint64_t imageHash, uint32_t pc, uint32_t interpretedPairs) noexcept;
    // Emulation speed of the last report (100 = full speed), or -1 before the first one.
    double lastSpeedPercent() noexcept;

    // Numbers for the on-screen FPS counter, measured over about the last second. They work with
    // the performance logs on or off. Window thread.
    struct LiveStats
    {
        double gameFps = -1.0;      // new pictures the game showed per second (PS2 flips)
        double windowFps = -1.0;    // frames the PC window drew per second
        double speedPercent = -1.0; // emulated PS2 time vs real time
        bool paused = false;        // the F2 drawer has the game stopped
    };
    LiveStats liveStats() noexcept;
    void noteGameFrame() noexcept; // the game showed a new picture (game thread, at VBlank)
    void graphicsNote(const char *fmt, ...) noexcept;                     // free-form line in Graphicslogs.txt
    void performanceNote(const char *fmt, ...) noexcept;                  // free-form line in performancelogs.txt

    // Called by the main thread every frame; writes a report every two seconds.
    // eeCycle: scheduler cycle counter; rdram: guest RAM (for PS2X_PERF_TICK_ADDR).
    void mainThreadTick(uint64_t eeCycle, const uint8_t *rdram) noexcept;
    // Instead of mainThreadTick while the window is minimized (nothing drawn): keeps the watchdog
    // and the game-speed numbers going without counting frames; such intervals are left out of
    // the session averages, like the F2-paused ones.
    void mainThreadIdle(uint64_t eeCycle, const uint8_t *rdram) noexcept;

    // Writes the session summary and closes the log files.
    void shutdown() noexcept;

    struct Scope
    {
        explicit Scope(Bucket bucket) noexcept;
        ~Scope();
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        Bucket m_bucket;
        bool m_active;
        uint64_t m_start = 0;
        uint64_t m_savedChild = 0;
    };

    // Records one timed wait: how long was asked for and how long it actually took.
    void recordWait(bool pacing, uint64_t requestedNs, uint64_t actualNs, bool timedOut) noexcept;
}
