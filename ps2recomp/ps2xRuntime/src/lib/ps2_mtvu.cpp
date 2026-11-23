// The VU1 thread - see include/runtime/ps2_mtvu.h.

#include "runtime/ps2_mtvu.h"
#include "runtime/ps2_perf.h"

#include <chrono>
#include <condition_variable>
#include "runtime/ps2_fast_sync.h"
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace ps2mtvu
{
    std::atomic<bool> g_active{false};

    namespace
    {
        struct Task
        {
            std::function<void()> fn;
            size_t bytes = 0;
            bool frame = false;
        };

        // Limits on how far the EE may run ahead.
        constexpr size_t kMaxQueuedBytes = 96u << 20;
        constexpr size_t kMaxQueuedTasks = 16384u;
        constexpr uint32_t kMaxFramesQueued = 1u; // frames handed over but not yet finished

        ps2sync::Mutex g_mutex;
        ps2sync::CondVar g_workCv; // VU1 thread: work arrived / stop
        ps2sync::CondVar g_doneCv; // EE: work finished
        bool g_workerSleeping = false; // under g_mutex: the VU1 thread waits for work
        std::deque<Task> g_queue;
        size_t g_queuedBytes = 0;   // under g_mutex: posted, not yet finished
        size_t g_queuedTasks = 0;   // under g_mutex
        uint32_t g_queuedFrames = 0; // under g_mutex
        bool g_stop = false;
        std::thread g_thread;
        std::atomic<uint64_t> g_posted{0};
        std::atomic<uint64_t> g_completed{0};
        thread_local bool t_onWorker = false;

        std::atomic<uint64_t> s_tasks{0}, s_frames{0}, s_syncWaitNs{0}, s_frameWaitNs{0}, s_fullWaitNs{0}, s_bytes{0}, s_wakeups{0};
        std::atomic<uint64_t> s_syncs[static_cast<uint32_t>(SyncReason::Count)];

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }

        void nameThisThread()
        {
#if defined(_WIN32)
            using SetDescription = HRESULT(WINAPI *)(HANDLE, PCWSTR);
            if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
                if (auto fn = reinterpret_cast<SetDescription>(reinterpret_cast<void *>(GetProcAddress(kernel, "SetThreadDescription"))))
                    fn(GetCurrentThread(), L"VU1 thread");
#elif defined(__linux__)
            pthread_setname_np(pthread_self(), "VU1Thread");
#endif
        }

        void workerMain()
        {
            t_onWorker = true;
            nameThisThread();
            ps2perf::registerThread("VU1 thread (VIF1, VU1 microcode)");
            std::deque<Task> batch;
            for (;;)
            {
                {
                    std::unique_lock<ps2sync::Mutex> lock(g_mutex);
                    if (g_queue.empty())
                    {
                        ps2perf::Scope idle(ps2perf::IdleWait);
                        s_wakeups.fetch_add(1u, std::memory_order_relaxed);
                        g_workerSleeping = true;
                        g_workCv.wait(lock, [] { return g_stop || !g_queue.empty(); });
                        g_workerSleeping = false;
                    }
                    if (g_queue.empty())
                        return; // stop requested and nothing left
                    batch.swap(g_queue);
                }
                size_t bytes = 0;
                uint32_t frames = 0;
                ps2perf::heartbeat(ps2perf::Beat::VuBegin);
                for (Task &task : batch)
                {
                    try
                    {
                        task.fn();
                    }
                    catch (const std::exception &e)
                    {
                        std::fprintf(stderr, "[mtvu] task failed: %s\n", e.what());
                    }
                    catch (...)
                    {
                        std::fprintf(stderr, "[mtvu] task failed\n");
                    }
                    bytes += task.bytes;
                    frames += task.frame ? 1u : 0u;
                }
                ps2perf::heartbeat(ps2perf::Beat::VuEnd);
                const size_t count = batch.size();
                batch.clear();
                {
                    std::lock_guard<ps2sync::Mutex> lock(g_mutex);
                    g_queuedBytes -= bytes;
                    g_queuedTasks -= count;
                    g_queuedFrames -= frames;
                    g_completed.fetch_add(count, std::memory_order_release);
                }
                g_doneCv.notify_all();
            }
        }

        bool enabledByEnvironment()
        {
            const char *env = std::getenv("PS2X_MTVU");
            return !(env && *env == '0');
        }
    }

    bool start()
    {
        if (g_active.load())
            return true;
        if (!enabledByEnvironment())
            return false;
        g_stop = false;
        g_thread = std::thread(workerMain);
        g_active.store(true);
        return true;
    }

    void stop()
    {
        if (!g_active.load())
            return;
        {
            std::lock_guard<ps2sync::Mutex> lock(g_mutex);
            g_stop = true;
        }
        g_workCv.notify_all();
        if (g_thread.joinable())
            g_thread.join();
        g_active.store(false);
    }

    bool onWorker() noexcept
    {
        return t_onWorker;
    }

    namespace
    {
        void enqueue(Task &&task, bool frame)
        {
            std::unique_lock<ps2sync::Mutex> lock(g_mutex);
            // Back-pressure: the EE waits while the VU1 thread is too far behind.
            if (g_queuedBytes > kMaxQueuedBytes || g_queuedTasks > kMaxQueuedTasks)
            {
                const uint64_t start = nowNs();
                ps2perf::Scope wait(ps2perf::MtvuWait);
                g_doneCv.wait(lock, [] { return g_queuedBytes <= kMaxQueuedBytes / 2u && g_queuedTasks <= kMaxQueuedTasks / 2u; });
                s_fullWaitNs.fetch_add(nowNs() - start, std::memory_order_relaxed);
            }
            if (frame && g_queuedFrames >= kMaxFramesQueued)
            {
                const uint64_t start = nowNs();
                ps2perf::Scope wait(ps2perf::MtvuWait);
                g_doneCv.wait(lock, [] { return g_queuedFrames < kMaxFramesQueued; });
                s_frameWaitNs.fetch_add(nowNs() - start, std::memory_order_relaxed);
            }
            task.frame = frame;
            g_queuedBytes += task.bytes;
            g_queuedTasks += 1u;
            g_queuedFrames += frame ? 1u : 0u;
            s_bytes.fetch_add(task.bytes, std::memory_order_relaxed);
            g_queue.push_back(std::move(task));
            g_posted.fetch_add(1u, std::memory_order_relaxed);
            const bool wake = g_workerSleeping;
            lock.unlock();
            if (wake)
                g_workCv.notify_one();
            s_tasks.fetch_add(1u, std::memory_order_relaxed);
            if (frame)
                s_frames.fetch_add(1u, std::memory_order_relaxed);
        }
    }

    void post(std::function<void()> task, size_t bytes)
    {
        if (!active() || t_onWorker)
        {
            task();
            return;
        }
        Task item;
        item.fn = std::move(task);
        item.bytes = bytes;
        enqueue(std::move(item), false);
    }

    void postFrame(std::function<void()> task)
    {
        if (!active() || t_onWorker)
        {
            task();
            return;
        }
        Task item;
        item.fn = std::move(task);
        enqueue(std::move(item), true);
    }

    void sync(SyncReason reason)
    {
        if (!active() || t_onWorker)
            return;
        // Posted only grows on this (the EE) thread, so this snapshot is exact.
        const uint64_t target = g_posted.load(std::memory_order_relaxed);
        if (g_completed.load(std::memory_order_acquire) >= target)
            return;
        s_syncs[static_cast<uint32_t>(reason)].fetch_add(1u, std::memory_order_relaxed);
        const uint64_t start = nowNs();
        {
            ps2perf::Scope wait(ps2perf::MtvuWait);
            std::unique_lock<ps2sync::Mutex> lock(g_mutex);
            g_doneCv.wait(lock, [target] { return g_completed.load(std::memory_order_acquire) >= target; });
        }
        s_syncWaitNs.fetch_add(nowNs() - start, std::memory_order_relaxed);
    }

    Stats takeStats() noexcept
    {
        Stats stats;
        stats.tasks = s_tasks.exchange(0, std::memory_order_relaxed);
        stats.frames = s_frames.exchange(0, std::memory_order_relaxed);
        for (uint32_t i = 0; i < static_cast<uint32_t>(SyncReason::Count); ++i)
            stats.syncs[i] = s_syncs[i].exchange(0, std::memory_order_relaxed);
        stats.syncWaitNs = s_syncWaitNs.exchange(0, std::memory_order_relaxed);
        stats.frameWaitNs = s_frameWaitNs.exchange(0, std::memory_order_relaxed);
        stats.fullWaitNs = s_fullWaitNs.exchange(0, std::memory_order_relaxed);
        stats.queuedBytes = s_bytes.exchange(0, std::memory_order_relaxed);
        stats.wakeups = s_wakeups.exchange(0, std::memory_order_relaxed);
        return stats;
    }
}
