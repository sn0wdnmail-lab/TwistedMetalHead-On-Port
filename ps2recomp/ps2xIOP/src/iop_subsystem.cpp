#include "ps2x/iop/iop_subsystem.h"

#include "iop_service.h"
#include "iop_module_manager.h"
#include "emulator/iop_emulator.h"
#include "module_factories.h"
#include "ps2x/iop/iop_audio.h"
#include "ps2x/iop/ps2_path.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The IOP thread
// --------------
// The I/O processor used to run on the game thread: every ~2048 EE cycles the game thread ran it up
// to where the EE's clock said it should be. Now a thread of its own does that, in batches of game
// time handed over by the game thread, so the IOP's own work (989snd's sound ticks and SPU2 mixing,
// the disc and memory card modules) runs alongside the game instead of inside it.
//
// The IOP is never run by two threads at once (iopMutex), and it only ever runs behind the game:
//  - Whenever the game talks to the IOP (an RPC call, a SIF transfer, IOP memory, a module load),
//    the game thread first waits for the IOP thread's batch, then runs the rest of the game time
//    itself, so the IOP is exactly where it was when it ran on the game thread.
//  - What the IOP does to the EE's side while on its own thread - writes into EE memory (IOP-side
//    SIF DMA) and SIF commands to the EE - is kept and done by the game thread at its next step,
//    between two pieces of game code, never in the middle of one.
//  - If the IOP thread falls too far behind (a slow PC), the game thread catches it up itself.
// settings.ini iop_thread=0 or PS2X_IOP_THREAD=0 keeps the IOP on the game thread.

namespace ps2x::iop
{
    namespace
    {
        std::atomic<bool> g_threadEnabled{true};
        std::atomic<void (*)()> g_threadStartHook{nullptr};

        struct ThreadCounters
        {
            std::atomic<bool> running{false};
            std::atomic<uint64_t> busyNs{0}, aheadNs{0}, batches{0}, syncs{0}, syncWaitNs{0}, inlineNs{0}, lagCatchUps{0}, effects{0};
        };
        ThreadCounters g_counters;

        thread_local int t_iopDepth = 0;         // this thread is running the IOP (its own batch or a call into it)
        thread_local bool t_onIopThread = false; // this is the IOP thread

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }

        // Game time handed to the IOP thread at a time (EE cycles: 131072 is about 0.44 ms), and how
        // much may wait for it before the game thread catches the IOP up itself. Bigger batches wake
        // the IOP thread less often; smaller ones leave less for the game thread to run itself when
        // it calls into the IOP.
        constexpr uint64_t kBatchEeCycles = 131072u;
        constexpr uint64_t kMaxBehindEeCycles = 4u * kBatchEeCycles;
        // How far ahead of the game the IOP may run while the game waits (1/30 s).
        constexpr uint64_t kMaxAheadEeCycles = 294912000u / 30u;

        // What the IOP thread did to the EE's side, to be done by the game thread.
        enum class EffectKind : uint8_t
        {
            Write,
            Zero,
            Command,
        };
        struct Effect
        {
            EffectKind kind = EffectKind::Write;
            uint32_t address = 0u;
            uint32_t commandId = 0u;
            uint32_t size = 0u;
            std::vector<uint8_t> bytes;
        };

        class EffectQueue
        {
        public:
            void push(Effect &&effect)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_items.push_back(std::move(effect));
                m_waiting.store(true, std::memory_order_release);
            }
            bool waiting() const noexcept { return m_waiting.load(std::memory_order_acquire); }
            void takeAll(std::vector<Effect> &out)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                out.swap(m_items);
                m_waiting.store(false, std::memory_order_relaxed);
            }

        private:
            std::mutex m_mutex;
            std::vector<Effect> m_items;
            std::atomic<bool> m_waiting{false};
        };

        // The host the IOP emulator sees. On the IOP thread, writes into EE memory and SIF commands
        // to the EE are queued for the game thread; everything else (reads, files, logs) goes
        // straight through, and so does everything on any other thread.
        class DeferringHost final : public IopHost
        {
        public:
            DeferringHost(IopHost &real, EffectQueue &queue) : m_real(real), m_queue(queue) {}

            bool readGuest(uint32_t address, void *destination, size_t size) const override
            {
                return m_real.readGuest(address, destination, size);
            }
            bool writeGuest(uint32_t address, const void *source, size_t size) override
            {
                if (!t_onIopThread)
                    return m_real.writeGuest(address, source, size);
                Effect effect;
                effect.kind = EffectKind::Write;
                effect.address = address;
                effect.size = static_cast<uint32_t>(size);
                effect.bytes.resize(size);
                // The same range check the write makes (it fails the same way).
                if (size != 0u && !m_real.readGuest(address, effect.bytes.data(), size))
                    return false;
                if (size != 0u)
                    std::memcpy(effect.bytes.data(), source, size);
                m_queue.push(std::move(effect));
                return true;
            }
            bool zeroGuest(uint32_t address, size_t size) override
            {
                if (!t_onIopThread)
                    return m_real.zeroGuest(address, size);
                if (size != 0u)
                {
                    std::vector<uint8_t> check(size);
                    if (!m_real.readGuest(address, check.data(), size))
                        return false;
                }
                Effect effect;
                effect.kind = EffectKind::Zero;
                effect.address = address;
                effect.size = static_cast<uint32_t>(size);
                m_queue.push(std::move(effect));
                return true;
            }
            bool sendSifCommand(uint32_t commandId, const void *packet, size_t packetSize) override
            {
                if (!t_onIopThread)
                    return m_real.sendSifCommand(commandId, packet, packetSize);
                Effect effect;
                effect.kind = EffectKind::Command;
                effect.commandId = commandId;
                effect.size = static_cast<uint32_t>(packetSize);
                const uint8_t *bytes = static_cast<const uint8_t *>(packet);
                effect.bytes.assign(bytes, bytes + packetSize);
                m_queue.push(std::move(effect));
                return true;
            }

            bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override
            {
                return m_real.normalizeGuestAddress(address, normalized);
            }
            bool readIopMemory(uint32_t address, void *destination, size_t size) const override
            {
                return m_real.readIopMemory(address, destination, size);
            }
            bool writeIopMemory(uint32_t address, const void *source, size_t size) override
            {
                return m_real.writeIopMemory(address, source, size);
            }
            bool zeroIopMemory(uint32_t address, size_t size) override { return m_real.zeroIopMemory(address, size); }
            bool normalizeIopAddress(uint32_t address, uint32_t &normalized) const override
            {
                return m_real.normalizeIopAddress(address, normalized);
            }
            uint32_t allocateIopHandle(IopHandleKind kind) override { return m_real.allocateIopHandle(kind); }
            uint32_t allocateGuest(uint32_t size, uint32_t alignment) override { return m_real.allocateGuest(size, alignment); }
            void freeGuest(uint32_t address) override { m_real.freeGuest(address); }
            void audioCommand(uint32_t sid, uint32_t function, GuestBuffer send, GuestBuffer receive) override
            {
                m_real.audioCommand(sid, function, send, receive);
            }
            std::string hostPath(HostPathKind kind) const override { return m_real.hostPath(kind); }
            std::string translateGuestPath(std::string_view path) const override { return m_real.translateGuestPath(path); }
            uint64_t openHostFile(std::string_view path) override { return m_real.openHostFile(path); }
            bool hostFileSize(uint64_t handle, uint64_t &size) const override { return m_real.hostFileSize(handle, size); }
            bool readHostFile(uint64_t handle, uint64_t offset, void *destination, size_t size, size_t &bytesRead) override
            {
                return m_real.readHostFile(handle, offset, destination, size, bytesRead);
            }
            void closeHostFile(uint64_t handle) override { m_real.closeHostFile(handle); }
            int32_t memoryCard(const MemoryCardRequest &request) override { return m_real.memoryCard(request); }
            bool hasGuestFunction(uint32_t address) const override { return m_real.hasGuestFunction(address); }
            bool invokeGuestFunction(uint64_t callToken, uint32_t address, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                     uint32_t *resultAddress) override
            {
                return m_real.invokeGuestFunction(callToken, address, a0, a1, a2, a3, resultAddress);
            }
            void log(LogLevel level, std::string_view message) override { m_real.log(level, message); }

        private:
            IopHost &m_real;
            EffectQueue &m_queue;
        };
    }

    namespace detail
    {
        // What the EE scheduler asks of the IOP thread as the game goes idle (see iop_audio.h).
        class IopIdleHooks
        {
        public:
            virtual bool catchUpBeforeIdle() = 0;
            virtual void runAheadWhileIdle(uint64_t eeCycles) = 0;

        protected:
            ~IopIdleHooks() = default;
        };
        std::atomic<IopIdleHooks *> g_threadedImpl{nullptr};
    }

    void setIopThreadEnabled(bool enabled) noexcept
    {
        g_threadEnabled.store(enabled, std::memory_order_relaxed);
    }

    void setIopThreadStartHook(void (*hook)()) noexcept
    {
        g_threadStartHook.store(hook, std::memory_order_relaxed);
    }

    IopThreadStats iopThreadStats() noexcept
    {
        IopThreadStats stats;
        stats.running = g_counters.running.load(std::memory_order_relaxed);
        stats.busyNs = g_counters.busyNs.load(std::memory_order_relaxed);
        stats.aheadNs = g_counters.aheadNs.load(std::memory_order_relaxed);
        stats.batches = g_counters.batches.load(std::memory_order_relaxed);
        stats.syncs = g_counters.syncs.load(std::memory_order_relaxed);
        stats.syncWaitNs = g_counters.syncWaitNs.load(std::memory_order_relaxed);
        stats.inlineNs = g_counters.inlineNs.load(std::memory_order_relaxed);
        stats.lagCatchUps = g_counters.lagCatchUps.load(std::memory_order_relaxed);
        stats.effects = g_counters.effects.load(std::memory_order_relaxed);
        return stats;
    }

    class IopSubsystem::Impl final : public detail::IopIdleHooks
    {
    public:
        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              workerHost(hostRef, effects),
              emulator(workerHost)
        {
            coreServices.emplace_back(detail::createMcservService(host));
            coreServices.emplace_back(detail::createDbcmanService(host));
            coreServices.emplace_back(detail::createLibSdService(host));
            refreshServiceModuleKeys();
            rebuildRoutes();
        }

        bool serviceActive(const detail::IopService &service) const
        {
            return moduleManager.isLoaded(service.moduleAliases());
        }

        void refreshServiceModuleKeys()
        {
            std::vector<std::string> keys;
            for (const auto &service : coreServices)
            {
                for (std::string_view alias : service->moduleAliases())
                    keys.emplace_back(alias);
            }
            moduleManager.setServiceModuleKeys(std::move(keys));
        }

        void rebuildRoutes()
        {
            routes.clear();
            lastError.clear();
            for (const auto &service : coreServices)
            {
                if (!serviceActive(*service))
                    continue;
                for (const uint32_t sid : service->sids())
                {
                    if (!routes.emplace(sid, service.get()).second)
                    {
                        std::ostringstream out;
                        out << "duplicate IOP SID 0x" << std::hex << sid << " in core services";
                        lastError = out.str();
                        routes.clear();
                        return;
                    }
                }
            }
        }

        void recordLoadOutcome(std::string_view path, bool hle)
        {
            constexpr size_t maxOutcomes = 32u;
            if (loadOutcomes.size() >= maxOutcomes || !loggedLoadPaths.emplace(path).second)
                return;
            std::string message = hle ? "[IOP:HLE] fallback module='" : "[IOP:load-failed] module='";
            message.append(path);
            message += hle ? "' physical IRX unavailable; using registered HLE provider"
                           : "' no HLE provider accepted the module; physical IRX was not loaded";
            loadOutcomes.push_back(message);
            host.log(hle ? LogLevel::Info : LogLevel::Warning, message);
        }

        ~Impl()
        {
            stopWorker();
        }

        // ---- IOP thread
        // Positions are in EE cycles of game time handed to the IOP since it started:
        //   handedEe - the game's time: the IOP must get at least here
        //   aheadEe  - the game is idle until here (its next VBlank or timer): the IOP may run ahead to it
        //   doneEe   - where the IOP is, or will be once the piece running now and anything the game
        //              thread took over are done
        // The IOP thread runs pieces of up to kBatchEeCycles towards max(handedEe, aheadEe), never
        // while the game thread has taken the IOP (gameHasIop).

        // Exclusive use of the IOP for one call from outside it. On the game thread it first brings
        // the IOP up to the game's time: waits for the IOP thread's piece, does what the IOP thread did
        // to the EE's side, then runs the game time the IOP thread has not run itself.
        class Call
        {
        public:
            explicit Call(Impl &impl) : m_impl(impl)
            {
                if (t_iopDepth > 0)
                    return; // a call made from inside the IOP (it already has it)
                m_game = impl.threaded.load(std::memory_order_relaxed) && std::this_thread::get_id() == impl.gameThread;
                uint64_t catchUp = 0u;
                if (m_game)
                    catchUp = impl.takeForGame();
                m_lock = std::unique_lock<std::mutex>(impl.iopMutex);
                m_entered = true;
                ++t_iopDepth;
                if (m_game)
                {
                    ++impl.gameNested;
                    impl.applyEffects();
                    if (catchUp != 0u)
                        impl.runHere(catchUp);
                }
            }
            ~Call()
            {
                if (!m_entered)
                    return;
                --t_iopDepth;
                if (m_game)
                    --m_impl.gameNested;
                m_lock.unlock();
                if (m_game)
                    m_impl.giveBackFromGame();
            }
            Call(const Call &) = delete;
            Call &operator=(const Call &) = delete;

        private:
            Impl &m_impl;
            std::unique_lock<std::mutex> m_lock;
            bool m_entered = false;
            bool m_game = false;
        };

        // Game thread, first IOP step: start the IOP thread (unless turned off).
        void startWorker()
        {
            workerDecided = true;
            gameThread = std::this_thread::get_id();
            const char *env = std::getenv("PS2X_IOP_THREAD");
            if (!g_threadEnabled.load(std::memory_order_relaxed) || (env && *env == '0'))
                return;
            try
            {
                worker = std::thread([this] { workerMain(); });
                threaded.store(true, std::memory_order_relaxed);
                detail::g_threadedImpl.store(this, std::memory_order_release);
            }
            catch (...)
            {
                threaded.store(false, std::memory_order_relaxed);
            }
        }

        void stopWorker()
        {
            if (!worker.joinable())
                return;
            detail::g_threadedImpl.store(nullptr, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                stopping = true;
            }
            workCv.notify_all();
            worker.join();
            threaded.store(false, std::memory_order_relaxed); // anything later runs on the caller
        }

        void workerMain()
        {
            t_onIopThread = true;
            if (void (*hook)() = g_threadStartHook.load(std::memory_order_relaxed))
                hook();
            g_counters.running.store(true, std::memory_order_relaxed);
            std::unique_lock<std::mutex> lock(queueMutex);
            while (!stopping)
            {
                const uint64_t target = std::max(handedEe, aheadEe);
                if (gameHasIop || doneEe >= target)
                {
                    workerAsleep = true;
                    workCv.wait(lock);
                    workerAsleep = false;
                    continue;
                }
                const uint64_t piece = std::min<uint64_t>(target - doneEe, kBatchEeCycles);
                const bool ahead = doneEe + piece > handedEe; // beyond the game's time (it is idle)
                doneEe += piece;
                pieceRunning = true;
                lock.unlock();
                const uint64_t start = nowNs();
                {
                    std::lock_guard<std::mutex> state(iopMutex);
                    ++t_iopDepth;
                    emulator.runEeCycles(piece);
                    --t_iopDepth;
                }
                const uint64_t spent = nowNs() - start;
                g_counters.busyNs.fetch_add(spent, std::memory_order_relaxed);
                if (ahead)
                    g_counters.aheadNs.fetch_add(spent, std::memory_order_relaxed);
                g_counters.batches.fetch_add(1u, std::memory_order_relaxed);
                lock.lock();
                pieceRunning = false;
                doneCv.notify_all();
            }
            g_counters.running.store(false, std::memory_order_relaxed);
        }

        // Holding queueMutex: the game thread takes the IOP - the IOP thread starts no new piece, the
        // one running finishes, and the game time it has not run is the game thread's to run (returned).
        uint64_t takeLocked(std::unique_lock<std::mutex> &lock)
        {
            gameHasIop = true;
            if (pieceRunning)
            {
                const uint64_t start = nowNs();
                while (pieceRunning)
                    doneCv.wait(lock);
                g_counters.syncWaitNs.fetch_add(nowNs() - start, std::memory_order_relaxed);
            }
            const uint64_t catchUp = handedEe > doneEe ? handedEe - doneEe : 0u;
            doneEe += catchUp;
            return catchUp;
        }

        // Holding queueMutex: the IOP thread may go on (wakes it if there is anything to run).
        void giveBackLocked()
        {
            gameHasIop = false;
            if (workerAsleep && std::max(handedEe, aheadEe) > doneEe)
                workCv.notify_one();
        }

        // Game thread, before a call: everything gathered so far is the IOP's; take the IOP.
        uint64_t takeForGame()
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            handedEe += localEe;
            localEe = 0u;
            const bool wasRunning = pieceRunning;
            const uint64_t catchUp = takeLocked(lock);
            if (wasRunning || catchUp != 0u)
                g_counters.syncs.fetch_add(1u, std::memory_order_relaxed);
            return catchUp;
        }

        void giveBackFromGame()
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            giveBackLocked();
        }

        // Game thread: the IOP's next piece of game time.
        void step(uint64_t eeCycles)
        {
            if (effects.waiting())
                applyEffects();
            localEe += eeCycles;
            if (localEe < kBatchEeCycles)
                return;
            std::unique_lock<std::mutex> lock(queueMutex);
            handedEe += localEe;
            localEe = 0u;
            if (handedEe <= doneEe)
                return; // the IOP ran ahead while the game waited: already done
            if (handedEe - doneEe > kMaxBehindEeCycles)
            {
                // The IOP thread is too far behind: catch the IOP up here, after its piece.
                const uint64_t catchUp = takeLocked(lock);
                lock.unlock();
                g_counters.lagCatchUps.fetch_add(1u, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> state(iopMutex);
                    ++t_iopDepth;
                    ++gameNested;
                    applyEffects();
                    runHere(catchUp);
                    --gameNested;
                    --t_iopDepth;
                }
                lock.lock();
                giveBackLocked();
                return;
            }
            if (workerAsleep && !gameHasIop)
                workCv.notify_one();
        }

        // Game thread, the game going idle: the IOP up to the game's time, and what it did to the
        // EE's side done. True when there was anything to do on the EE's side.
        bool catchUpBeforeIdle() override
        {
            if (!threaded.load(std::memory_order_relaxed) || gameNested != 0)
                return false;
            uint64_t catchUp = 0u;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                handedEe += localEe;
                localEe = 0u;
                if (!pieceRunning && handedEe <= doneEe)
                {
                    // Nothing to wait for or run; only what the IOP thread did may be waiting.
                    lock.unlock();
                    if (!effects.waiting())
                        return false;
                    std::lock_guard<std::mutex> state(iopMutex);
                    ++t_iopDepth;
                    ++gameNested;
                    const size_t applied = applyEffects();
                    --gameNested;
                    --t_iopDepth;
                    return applied != 0u;
                }
                catchUp = takeLocked(lock);
                g_counters.syncs.fetch_add(1u, std::memory_order_relaxed);
            }
            size_t applied = 0u;
            {
                std::lock_guard<std::mutex> state(iopMutex);
                ++t_iopDepth;
                ++gameNested;
                applied = applyEffects();
                if (catchUp != 0u)
                    runHere(catchUp);
                --gameNested;
                --t_iopDepth;
            }
            giveBackFromGame();
            // IOP time run here may have sent the EE a SIF command (written straight through on this
            // thread): let the scheduler look again before it waits.
            return applied != 0u || catchUp != 0u;
        }

        // Game thread, about to wait: the IOP thread may run up to eeCycles ahead meanwhile.
        void runAheadWhileIdle(uint64_t eeCycles) override
        {
            if (!threaded.load(std::memory_order_relaxed) || eeCycles == 0u)
                return;
            std::lock_guard<std::mutex> lock(queueMutex);
            handedEe += localEe;
            localEe = 0u;
            aheadEe = std::max(aheadEe, handedEe + std::min(eeCycles, kMaxAheadEeCycles));
            if (workerAsleep && !gameHasIop && aheadEe > doneEe)
                workCv.notify_one();
        }

        // Holding iopMutex: run game time on this thread.
        void runHere(uint64_t eeCycles)
        {
            const uint64_t start = nowNs();
            emulator.runEeCycles(eeCycles);
            g_counters.inlineNs.fetch_add(nowNs() - start, std::memory_order_relaxed);
        }

        // Game thread: what the IOP thread did to the EE's side, in order. Returns how many.
        size_t applyEffects()
        {
            if (!effects.waiting())
                return 0u;
            effects.takeAll(applying);
            for (const Effect &effect : applying)
            {
                switch (effect.kind)
                {
                case EffectKind::Write:
                    (void)host.writeGuest(effect.address, effect.bytes.data(), effect.size);
                    break;
                case EffectKind::Zero:
                    (void)host.zeroGuest(effect.address, effect.size);
                    break;
                case EffectKind::Command:
                    (void)host.sendSifCommand(effect.commandId, effect.bytes.data(), effect.size);
                    break;
                }
            }
            const size_t count = applying.size();
            g_counters.effects.fetch_add(count, std::memory_order_relaxed);
            applying.clear();
            return count;
        }

        IopHost &host;
        detail::ServiceList coreServices;
        std::unordered_map<uint32_t, detail::IopService *> routes;
        std::vector<std::string> loadOutcomes;
        std::unordered_set<std::string> loggedLoadPaths;
        std::string lastError;
        detail::IopModuleManager moduleManager;
        EffectQueue effects;
        DeferringHost workerHost; // what the emulator calls; must be built before it
        detail::IopEmulator emulator;
        std::vector<Effect> applying;

        std::mutex iopMutex; // the IOP itself: one piece on the IOP thread, or one call from outside
        std::mutex queueMutex;
        std::condition_variable workCv, doneCv;
        std::thread worker;
        std::thread::id gameThread{};
        bool workerDecided = false;
        std::atomic<bool> threaded{false};
        bool stopping = false;     // (queueMutex)
        bool workerAsleep = false; // (queueMutex)
        bool pieceRunning = false; // (queueMutex)
        bool gameHasIop = false;   // (queueMutex) the game thread took the IOP
        uint64_t handedEe = 0u;    // (queueMutex)
        uint64_t aheadEe = 0u;     // (queueMutex)
        uint64_t doneEe = 0u;      // (queueMutex)
        uint64_t localEe = 0u;     // game thread: gathered, not handed over yet
        int gameNested = 0;        // game thread: inside a call into the IOP (it has the IOP)
    };

    bool catchUpIopBeforeIdle() noexcept
    {
        detail::IopIdleHooks *impl = detail::g_threadedImpl.load(std::memory_order_acquire);
        if (!impl)
            return false;
        try
        {
            return impl->catchUpBeforeIdle();
        }
        catch (...)
        {
            return false;
        }
    }

    void runIopAheadWhileIdle(uint64_t eeCycles) noexcept
    {
        detail::IopIdleHooks *impl = detail::g_threadedImpl.load(std::memory_order_acquire);
        if (!impl)
            return;
        try
        {
            impl->runAheadWhileIdle(eeCycles);
        }
        catch (...)
        {
        }
    }

    IopSubsystem::IopSubsystem(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopSubsystem::~IopSubsystem() = default;
    IopSubsystem::IopSubsystem(IopSubsystem &&) noexcept = default;
    IopSubsystem &IopSubsystem::operator=(IopSubsystem &&) noexcept = default;

    void IopSubsystem::reset()
    {
        Impl::Call call(*m_impl);
        m_impl->moduleManager.reset();
        m_impl->loadOutcomes.clear();
        m_impl->loggedLoadPaths.clear();
        for (auto &service : m_impl->coreServices)
        {
            if (service)
            {
                service->reset();
            }
        }
        m_impl->emulator.reset();
        m_impl->refreshServiceModuleKeys();
        m_impl->rebuildRoutes();
    }

    ModuleLoadResult IopSubsystem::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        Impl::Call call(*m_impl);
        const ParsedPs2Path parsed = parsePs2Path(path);
        if (!parsed)
            return {true, -1, -1};

        if (parsed.device != Ps2PathDevice::Rom0)
        {
            ModuleLoadResult physical = m_impl->emulator.loadModule(path, arguments, argumentSize);
            if (physical.moduleId > 0)
            {
                m_impl->moduleManager.observePhysicalLoad(physical.moduleId, path);
                m_impl->rebuildRoutes();
                return physical;
            }
        }

        ModuleLoadResult hle = m_impl->moduleManager.loadHle(path);
        if (hle.moduleId > 0)
        {
            m_impl->rebuildRoutes();
            if (parsed.device != Ps2PathDevice::Rom0)
                m_impl->recordLoadOutcome(path, true);
        }
        else
        {
            m_impl->recordLoadOutcome(path, false);
        }
        return hle;
    }

    ModuleLoadResult IopSubsystem::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        Impl::Call call(*m_impl);
        return m_impl->emulator.loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopSubsystem::stopModule(int32_t moduleId, int32_t *result)
    {
        Impl::Call call(*m_impl);
        if (m_impl->moduleManager.stopHle(moduleId, result))
        {
            m_impl->rebuildRoutes();
            return true;
        }
        if (!m_impl->emulator.stopModule(moduleId, result))
            return false;
        m_impl->moduleManager.observePhysicalStop(moduleId);
        m_impl->rebuildRoutes();
        return true;
    }

    void IopSubsystem::runEeCycles(uint64_t eeCycles) noexcept
    {
        // Game thread only (the EE scheduler's clock).
        Impl &impl = *m_impl;
        if (!impl.workerDecided)
            impl.startWorker();
        if (!impl.threaded.load(std::memory_order_relaxed) || impl.gameNested != 0)
        {
            // On this thread: the IOP thread is off, or this is EE code run from inside a call into
            // the IOP (which this thread already has).
            impl.emulator.runEeCycles(eeCycles);
            return;
        }
        try
        {
            impl.step(eeCycles);
        }
        catch (...)
        {
            // Never through the EE scheduler (a failed lock or wait: the IOP just waits a step).
        }
    }

    RpcAbi IopSubsystem::selectRpcAbi(const RpcAbiRequest &request) const
    {
        Impl::Call call(*m_impl);
        for (const auto &service : m_impl->coreServices)
        {
            if (service && m_impl->serviceActive(*service))
            {
                const RpcAbi selected = service->selectRpcAbi(request);
                if (selected != RpcAbi::RuntimeDefault)
                {
                    return selected;
                }
            }
        }
        return RpcAbi::RuntimeDefault;
    }

    bool IopSubsystem::canBindRpc(uint32_t sid) const noexcept
    {
        Impl::Call call(*m_impl);
        if (m_impl->routes.find(sid) != m_impl->routes.end())
        {
            return true;
        }
        return m_impl->emulator.hasRpcServer(sid);
    }

    RpcResult IopSubsystem::handleRpc(const RpcRequest &request)
    {
        Impl::Call call(*m_impl);
        const auto route = m_impl->routes.find(request.sid);
        detail::IopService *hle = route != m_impl->routes.end() ? route->second : nullptr;

        RpcResult emulated = m_impl->emulator.handleRpc(request);
        if (emulated.handled || !hle)
        {
            return emulated;
        }
        return hle->handleRpc(request);
    }

    void IopSubsystem::onSifTransfer(const SifTransfer &transfer)
    {
        Impl::Call call(*m_impl);
        for (auto &service : m_impl->coreServices)
        {
            if (service && m_impl->serviceActive(*service))
            {
                service->onSifTransfer(transfer);
            }
        }
        m_impl->emulator.onSifTransfer(transfer);
    }

    uint32_t IopSubsystem::allocateMemory(uint32_t size, uint32_t alignment)
    {
        Impl::Call call(*m_impl);
        return m_impl->emulator.allocateMemory(size, alignment);
    }

    bool IopSubsystem::freeMemory(uint32_t address)
    {
        Impl::Call call(*m_impl);
        return m_impl->emulator.freeMemory(address);
    }

    bool IopSubsystem::readMemory(uint32_t address, void *destination, size_t size) const
    {
        Impl::Call call(*m_impl);
        return m_impl->emulator.readMemory(address, destination, size);
    }

    bool IopSubsystem::writeMemory(uint32_t address, const void *source, size_t size)
    {
        Impl::Call call(*m_impl);
        return m_impl->emulator.writeMemory(address, source, size);
    }

    bool IopSubsystem::zeroMemory(uint32_t address, size_t size)
    {
        Impl::Call call(*m_impl);
        return m_impl->emulator.zeroMemory(address, size);
    }

    bool IopSubsystem::isMemoryRange(uint32_t address, size_t size) const
    {
        Impl::Call call(*m_impl);
        return m_impl->emulator.isMemoryRange(address, size);
    }

    DebugSnapshot IopSubsystem::debugSnapshot() const
    {
        Impl::Call call(*m_impl);
        DebugSnapshot snapshot;
        snapshot.emulatorCycles = m_impl->emulator.cycles();
        snapshot.emulatorInstructions = m_impl->emulator.instructions();
        snapshot.emulatorLoadedModules = m_impl->emulator.loadedModuleCount();
        snapshot.emulatorThreads = m_impl->emulator.threadCount();
        snapshot.emulatorRpcServers = m_impl->emulator.rpcServerCount();
        snapshot.diagnostics = m_impl->loadOutcomes;
        for (std::string &line : m_impl->emulator.threadDiagnostics())
            snapshot.diagnostics.push_back(std::move(line));
        if (!m_impl->lastError.empty())
        {
            snapshot.diagnostics.push_back(m_impl->lastError);
        }

        for (const auto &service : m_impl->coreServices)
        {
            DebugService row;
            row.name = service->name();
            row.sids.assign(service->sids().begin(), service->sids().end());
            row.active = m_impl->serviceActive(*service);
            service->appendDebugMetrics(row.metrics);
            snapshot.services.push_back(std::move(row));
        }
        return snapshot;
    }
}
