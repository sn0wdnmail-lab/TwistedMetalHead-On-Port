#include <deque>
#include <condition_variable>
// GPU GS renderer bridge - see include/runtime/gs/gs_gpu_renderer.h.
//
// paraLLEl-GS (third_party/parallel-gs, LGPLv3+) does the GS work on the GPU: the GIF packets go
// to GSInterface::gif_transfer on their own path index, the privileged display registers are
// copied in at every VBlank, and the scanout image is copied into a host buffer through a
// three-deep ring of fences that is never waited on, so neither the game thread nor the window
// thread blocks on the GPU in the common case.

#include "runtime/gs/gs_gpu_renderer.h"

#include "runtime/ps2_memory.h"
#include "runtime/ps2_perf.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#if defined(PS2X_HAVE_PGS)

#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"
#include "logging.hpp"
#include "thread_id.hpp"

namespace ps2gpu
{
    std::atomic<uint32_t> g_requestedSuperSampling{0u};
    std::atomic<uint32_t> g_currentSuperSampling{1u};
    namespace
    {
        void startFeed();
        void stopFeed();
    }

    namespace
    {
        using namespace Vulkan;
        using namespace ParallelGS;

        // Granite's log lines go to stderr as before and into Graphicslogs.txt.
        struct LogForward final : Util::LoggingInterface
        {
            bool log(const char *tag, const char *fmt, va_list va) override
            {
                char text[1024];
                std::vsnprintf(text, sizeof(text), fmt, va);
                size_t length = std::strlen(text);
                while (length != 0u && (text[length - 1] == '\n' || text[length - 1] == '\r'))
                    text[--length] = '\0';
                std::fprintf(stderr, "%s%s\n", tag ? tag : "", text);
                ps2perf::graphicsNote("Vulkan %s%s", tag ? tag : "", text);
                return true;
            }
        };

        // FINISH / SIGNAL / LABEL reach the guest's CSR and SIGLBLID from here, as the software
        // frontend does when it parses those registers.
        struct Signals final : SignalInterface
        {
            GSRegisters *regs = nullptr;

            bool on_signal(uint64_t payload) override
            {
                if (!regs)
                    return false;
                const uint32_t id = static_cast<uint32_t>(payload);
                const uint32_t mask = static_cast<uint32_t>(payload >> 32);
                uint32_t lo = static_cast<uint32_t>(regs->siglblid & 0xFFFFFFFFu);
                lo = (lo & ~mask) | (id & mask);
                regs->siglblid = (regs->siglblid & 0xFFFFFFFF00000000ull) | lo;
                regs->csr.fetch_or(0x1u);
                return false;
            }

            bool on_finish(uint64_t) override
            {
                if (regs)
                    regs->csr.fetch_or(0x2u);
                return false;
            }

            bool on_label(uint64_t payload) override
            {
                if (!regs)
                    return false;
                const uint32_t id = static_cast<uint32_t>(payload);
                const uint32_t mask = static_cast<uint32_t>(payload >> 32);
                uint32_t hi = static_cast<uint32_t>(regs->siglblid >> 32);
                hi = (hi & ~mask) | (id & mask);
                regs->siglblid = (static_cast<uint64_t>(hi) << 32) | (regs->siglblid & 0xFFFFFFFFu);
                return false;
            }
        };

        struct Readback
        {
            BufferHandle buffer;
            Fence fence;
            ImageHandle image; // kept alive until the copy finished
            uint32_t width = 0;
            uint32_t height = 0;
            VkFormat format = VK_FORMAT_UNDEFINED;
            uint32_t displayFbp = 0;
            bool pending = false;
        };

        struct State
        {
            std::mutex mutex; // everything that touches Vulkan / the GS emulator
            bool inited = false;
            Context context;
            Device device;
            GSInterface iface;
            Signals signals;
            LogForward log;
            GSRegisters *regs = nullptr;
            uint32_t superSampling = 1u;
            Readback ring[3];
            uint32_t ringIndex = 0;
            uint32_t packetsSinceCheck = 0;
            double gpuSecondsSeen = 0.0;
            std::thread::id lastThread{};
            bool warnedRegisterWrite = false;

            std::mutex frameMutex; // the newest picture, handed to the window thread
            std::vector<uint8_t> frame;
            uint32_t frameWidth = 0;
            uint32_t frameHeight = 0;
            uint32_t frameFbp = 0;
            bool frameFresh = false;
        };

        // Never destroyed: the device must not go away behind a thread that is still running.
        State &st()
        {
            static State *state = new State;
            return *state;
        }

        std::atomic<bool> g_active{false};

        // Granite keys its per-thread command pools by a registered index. All calls are
        // serialised by the state mutex, so every thread can share index 0.
        void registerThreadLocked(State &s)
        {
            const std::thread::id self = std::this_thread::get_id();
            if (self == s.lastThread)
                return;
            s.lastThread = self;
            Util::register_thread_index(0);
            Util::set_thread_logging_interface(&s.log);
        }

        void copyPrivRegistersLocked(State &s, const Scanout &r)
        {
            PrivRegisterState &p = s.iface.get_priv_register_state();
            auto put = [](void *dst, uint64_t value)
            { std::memcpy(dst, &value, sizeof(value)); };
            put(&p.pmode, r.pmode);
            // NTSC 640x448 analog when the CRTC registers were never written (SetGsCrt is HLE'd).
            put(&p.smode1, r.smode1 != 0u ? r.smode1 : 0x0000000740814504ull);
            put(&p.smode2, r.smode2);
            put(&p.srfsh, r.srfsh);
            put(&p.synch1, r.synch1);
            put(&p.synch2, r.synch2);
            put(&p.syncv, r.syncv);
            put(&p.dispfb1, r.dispfb1);
            put(&p.display1, r.display1);
            put(&p.dispfb2, r.dispfb2);
            put(&p.display2, r.display2);
            put(&p.extbuf, r.extbuf);
            put(&p.extdata, r.extdata);
            put(&p.extwrite, r.extwrite);
            put(&p.bgcolor, r.bgcolor);
            put(&p.csr, r.csr);
            put(&p.imr, r.imr);
            put(&p.busdir, r.busdir);
            put(&p.siglblid, r.siglblid);
        }

        // Moves a finished copy into the window thread's frame.
        void publishLocked(State &s, Readback &slot)
        {
            const auto *src = static_cast<const uint32_t *>(s.device.map_host_buffer(*slot.buffer, MEMORY_ACCESS_READ_BIT));
            const size_t pixels = static_cast<size_t>(slot.width) * slot.height;
            {
                std::lock_guard<std::mutex> lock(s.frameMutex);
                s.frame.resize(pixels * 4u);
                uint32_t *dst = reinterpret_cast<uint32_t *>(s.frame.data());
                const bool bgra = slot.format == VK_FORMAT_B8G8R8A8_UNORM || slot.format == VK_FORMAT_B8G8R8A8_SRGB;
                if (src)
                {
                    if (bgra)
                    {
                        for (size_t i = 0; i < pixels; ++i)
                        {
                            const uint32_t p = src[i];
                            dst[i] = ((p >> 16) & 0xFFu) | (p & 0xFF00u) | ((p & 0xFFu) << 16) | 0xFF000000u;
                        }
                    }
                    else
                    {
                        for (size_t i = 0; i < pixels; ++i)
                            dst[i] = src[i] | 0xFF000000u;
                    }
                }
                s.frameWidth = slot.width;
                s.frameHeight = slot.height;
                s.frameFbp = slot.displayFbp;
                s.frameFresh = true;
            }
            s.device.unmap_host_buffer(*slot.buffer, MEMORY_ACCESS_READ_BIT);
            slot.pending = false;
            slot.image.reset();
            slot.fence.reset();
            ps2perf::count(ps2perf::GpuReadbacks);
        }

        // Publishes every finished copy, oldest first, without waiting.
        void collectReadbacksLocked(State &s)
        {
            for (uint32_t k = 1; k <= 3; ++k)
            {
                Readback &slot = s.ring[(s.ringIndex + k) % 3u];
                if (slot.pending && slot.fence && slot.fence->wait_timeout(0))
                    publishLocked(s, slot);
            }
        }

        void queueReadbackLocked(State &s, const ScanoutResult &result, uint32_t displayFbp)
        {
            Readback &slot = s.ring[s.ringIndex];
            if (slot.pending)
            {
                if (slot.fence && slot.fence->wait_timeout(0))
                    publishLocked(s, slot);
                else
                {
                    ps2perf::count(ps2perf::GpuDroppedReadbacks); // the GPU is three frames behind: skip this one
                    return;
                }
            }
            const uint32_t width = result.image->get_width();
            const uint32_t height = result.image->get_height();
            if (!slot.buffer || slot.width != width || slot.height != height)
            {
                BufferCreateInfo info = {};
                info.size = static_cast<VkDeviceSize>(width) * height * 4u;
                info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                info.domain = BufferDomain::CachedHost;
                slot.buffer = s.device.create_buffer(info);
                slot.width = width;
                slot.height = height;
            }
            slot.format = result.image->get_format();
            slot.displayFbp = displayFbp;

            auto cmd = s.device.request_command_buffer();
            cmd->image_barrier(*result.image, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                               VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            cmd->copy_image_to_buffer(*slot.buffer, *result.image, 0, {}, {width, height, 1}, 0, 0,
                                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            cmd->barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            Fence fence;
            s.device.submit(cmd, &fence);
            slot.fence = std::move(fence);
            slot.image = result.image;
            slot.pending = true;
            s.ringIndex = (s.ringIndex + 1u) % 3u;
        }

        std::string driverVersionText(const VkPhysicalDeviceProperties &props)
        {
            char text[64];
            const uint32_t v = props.driverVersion;
            if (props.vendorID == 0x10DEu) // NVIDIA
                std::snprintf(text, sizeof(text), "%u.%02u", (v >> 22) & 0x3FFu, (v >> 14) & 0xFFu);
#if defined(_WIN32)
            else if (props.vendorID == 0x8086u) // Intel on Windows
                std::snprintf(text, sizeof(text), "%u.%u", v >> 14, v & 0x3FFFu);
#endif
            else
                std::snprintf(text, sizeof(text), "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
            return text;
        }

        SuperSampling toSuperSampling(uint32_t rate)
        {
            if (rate >= 16u)
                return SuperSampling::X16;
            if (rate >= 8u)
                return SuperSampling::X8;
            if (rate >= 4u)
                return SuperSampling::X4;
            if (rate >= 2u)
                return SuperSampling::X2;
            return SuperSampling::X1;
        }
    }

    bool compiledIn() noexcept
    {
        return true;
    }

    bool init(GSRegisters *privRegs, const Options &options, Description &outDescription, std::string &outError)
    {
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        registerThreadLocked(s);
        if (s.inited)
            return true;

        if (!Context::init_loader(nullptr))
        {
            outError = "no Vulkan driver found (vulkan-1 could not be loaded)";
            return false;
        }
        s.context.set_num_thread_indices(1);
        // Descriptor buffers/heaps are optional in paraLLEl-GS; PS2X_VK_DESCRIPTOR_BUFFER=1 turns
        // them on (classic descriptor sets are the most widely tested path across drivers).
        ContextCreationFlags flags = 0;
        if (!(std::getenv("PS2X_VK_PUSH_DESCRIPTOR") && *std::getenv("PS2X_VK_PUSH_DESCRIPTOR") == '0'))
            flags |= CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT;
        if (const char *env = std::getenv("PS2X_VK_DESCRIPTOR_BUFFER"); env && *env == '1')
            flags |= CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT | CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;
        if (!s.context.init_instance_and_device(nullptr, 0, nullptr, 0, flags))
        {
            outError = "the Vulkan device could not be created";
            return false;
        }
        s.device.set_context(s.context);
        s.device.init_frame_contexts(4);

        GSOptions gsOptions = {};
        gsOptions.vram_size = 4u * 1024u * 1024u;
        gsOptions.super_sampling = toSuperSampling(options.superSampling);
        // Lets the internal resolution change while the game runs (F2 > Video). VRAM for the
        // highest rate is set aside up front (about 150 MB); PS2X_PGS_STATIC_SS=1 turns this off.
        gsOptions.dynamic_super_sampling = !(std::getenv("PS2X_PGS_STATIC_SS") && *std::getenv("PS2X_PGS_STATIC_SS") == '1');
        gsOptions.super_sampled_textures = false;
        if (!s.iface.init(&s.device, gsOptions))
        {
            outError = "this graphics card or driver lacks a Vulkan feature paraLLEl-GS needs";
            return false;
        }
        s.regs = privRegs;
        s.signals.regs = privRegs;
        s.iface.set_signal_interface(&s.signals);
        if (ps2perf::enabled() && !std::getenv("PS2X_GPU_NO_TIMESTAMPS"))
        {
            DebugMode debug = {};
            debug.timestamps = true; // GPU time per second in Graphicslogs.txt
            s.iface.set_debug_mode(debug);
        }
        s.superSampling = static_cast<uint32_t>(gsOptions.super_sampling);
        g_currentSuperSampling.store(s.superSampling);

        const VkPhysicalDeviceProperties &props = s.device.get_gpu_properties();
        VkDeviceSize vram = 0;
        const VkPhysicalDeviceMemoryProperties &memory = s.device.get_memory_properties();
        for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
            if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0u)
                vram = std::max<VkDeviceSize>(vram, memory.memoryHeaps[i].size);
        char details[256];
        std::snprintf(details, sizeof(details), "driver %s, Vulkan %u.%u.%u, %.1f GB video memory, %s",
                      driverVersionText(props).c_str(), VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                      VK_VERSION_PATCH(props.apiVersion), static_cast<double>(vram) / (1024.0 * 1024.0 * 1024.0),
                      props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete GPU"
                      : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated GPU"
                      : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "software (CPU) Vulkan driver"
                                                                        : "GPU");
        outDescription.device = props.deviceName;
        outDescription.details = details;

        s.inited = true;
        g_active.store(true, std::memory_order_release);
        startFeed();
        return true;
    }

    bool active() noexcept
    {
        return g_active.load(std::memory_order_acquire);
    }

    // ---- GS feed thread -------------------------------------------------------------------
    // paraLLEl-GS parses GIF packets on the CPU before the GPU draws them. Done on the VU1 thread
    // that cost (a sixth of its time in a battle) competed with the VU1 microprograms, the
    // busiest work in the game. The packets, register writes and vsyncs now queue here, in the
    // order they arrive, and a thread of their own feeds them to paraLLEl-GS. A GS download waits
    // for the queue to empty first. PS2X_GS_THREAD=0 feeds them on the calling thread again.
    namespace
    {
        struct FeedItem
        {
            bool vsync = false;
            uint32_t path = 0;
            std::vector<uint8_t> bytes;
            Scanout scanout{};
        };

        constexpr size_t kFeedMaxBytes = 32u << 20; // the producer waits beyond this much data
        constexpr uint32_t kFeedMaxFrames = 1u;     // ... or when a whole frame is still waiting

        std::mutex g_feedMutex;
        std::condition_variable g_feedWorkCv;
        std::condition_variable g_feedDoneCv;
        std::deque<FeedItem> g_feed;
        size_t g_feedBytes = 0;
        uint32_t g_feedFrames = 0;
        bool g_feedBusy = false;
        bool g_feedStop = false;
        std::thread g_feedThread;
        std::atomic<bool> g_feedOn{false};
        thread_local bool t_onFeedThread = false;

        void gifTransferNow(uint32_t path, const uint8_t *data, size_t size);
        void vsyncNow(const Scanout &scanout);

        void feedMain()
        {
            t_onFeedThread = true;
            ps2perf::registerThread("GS feed thread");
            for (;;)
            {
                FeedItem item;
                {
                    std::unique_lock<std::mutex> lock(g_feedMutex);
                    g_feedWorkCv.wait(lock, [] { return g_feedStop || !g_feed.empty(); });
                    if (g_feed.empty())
                        return; // stopping, and everything handed over has been fed
                    item = std::move(g_feed.front());
                    g_feed.pop_front();
                    g_feedBusy = true;
                }
                if (item.vsync)
                    vsyncNow(item.scanout);
                else
                    gifTransferNow(item.path, item.bytes.data(), item.bytes.size());
                {
                    std::lock_guard<std::mutex> lock(g_feedMutex);
                    g_feedBytes -= item.bytes.size();
                    if (item.vsync)
                        --g_feedFrames;
                    g_feedBusy = false;
                }
                g_feedDoneCv.notify_all();
            }
        }

        bool feedQueues()
        {
            return g_feedOn.load(std::memory_order_acquire) && !t_onFeedThread;
        }

        void enqueueFeed(FeedItem &&item)
        {
            std::unique_lock<std::mutex> lock(g_feedMutex);
            g_feedDoneCv.wait(lock, [&item]
                              { return g_feedStop || (g_feedBytes < kFeedMaxBytes && (!item.vsync || g_feedFrames < kFeedMaxFrames)); });
            g_feedBytes += item.bytes.size();
            if (item.vsync)
                ++g_feedFrames;
            g_feed.push_back(std::move(item));
            lock.unlock();
            g_feedWorkCv.notify_one();
        }

        void startFeed()
        {
            const char *env = std::getenv("PS2X_GS_THREAD");
            if (env && *env == '0')
                return;
            g_feedStop = false;
            g_feedThread = std::thread(feedMain);
            g_feedOn.store(true, std::memory_order_release);
        }

        void stopFeed()
        {
            if (!g_feedOn.exchange(false))
                return;
            {
                std::lock_guard<std::mutex> lock(g_feedMutex);
                g_feedStop = true;
            }
            g_feedWorkCv.notify_all();
            g_feedDoneCv.notify_all();
            if (g_feedThread.joinable())
                g_feedThread.join();
        }
    }

    void drainFeed()
    {
        if (!g_feedOn.load(std::memory_order_acquire) || t_onFeedThread)
            return;
        std::unique_lock<std::mutex> lock(g_feedMutex);
        g_feedDoneCv.wait(lock, [] { return g_feedStop || (g_feed.empty() && !g_feedBusy); });
    }

    void gifTransfer(uint32_t path, const uint8_t *data, size_t size)
    {
        if (!data || size < 16u)
            return;
        if (feedQueues())
        {
            FeedItem item;
            item.path = path;
            item.bytes.assign(data, data + (size & ~static_cast<size_t>(15u)));
            enqueueFeed(std::move(item));
            return;
        }
        gifTransferNow(path, data, size);
    }

    namespace
    {
    void gifTransferNow(uint32_t path, const uint8_t *data, size_t size)
    {
        ps2perf::Scope scope(ps2perf::GsPacket);
        if (ps2perf::enabled())
        {
            ps2perf::count(ps2perf::GifPackets);
            ps2perf::count(ps2perf::GifBytes, size);
        }
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        registerThreadLocked(s);
        s.iface.gif_transfer(path & 3u, data, size & ~static_cast<size_t>(15u));
        // Hand finished pictures to the window as early as possible.
        if (++s.packetsSinceCheck >= 64u)
        {
            s.packetsSinceCheck = 0;
            collectReadbacksLocked(s);
        }
    }
    }

    void writeRegister(uint8_t reg, uint64_t value)
    {
        // One A+D qword: GIFtag (NLOOP=1, EOP=1, PACKED, NREG=1, REGS=0xE) + data.
        alignas(16) uint64_t packet[4];
        packet[0] = 1ull | (1ull << 15) | (1ull << 60);
        packet[1] = 0xEull;
        packet[2] = value;
        packet[3] = reg;
        State &s = st();
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            if (!s.warnedRegisterWrite)
            {
                s.warnedRegisterWrite = true;
                ps2perf::graphicsNote("GPU renderer: HLE register writes are forwarded as A+D packets (first: reg 0x%02X)", reg);
            }
        }
        // Path 0 has its own tag state, so this cannot split an open PATH1-3 tag.
        gifTransfer(0u, reinterpret_cast<const uint8_t *>(packet), sizeof(packet));
    }

    Scanout captureScanout(const GSRegisters &r)
    {
        Scanout c{};
        c.pmode = r.pmode;
        c.smode1 = r.smode1;
        c.smode2 = r.smode2;
        c.srfsh = r.srfsh;
        c.synch1 = r.synch1;
        c.synch2 = r.synch2;
        c.syncv = r.syncv;
        c.dispfb1 = r.dispfb1;
        c.display1 = r.display1;
        c.dispfb2 = r.dispfb2;
        c.display2 = r.display2;
        c.extbuf = r.extbuf;
        c.extdata = r.extdata;
        c.extwrite = r.extwrite;
        c.bgcolor = r.bgcolor;
        c.csr = r.csr.load(std::memory_order_acquire);
        c.imr = r.imr;
        c.busdir = r.busdir;
        c.siglblid = r.siglblid;
        return c;
    }

    void vsync(const Scanout &scanout)
    {
        if (!active())
            return;
        if (feedQueues())
        {
            FeedItem item;
            item.vsync = true;
            item.scanout = scanout;
            enqueueFeed(std::move(item));
            return;
        }
        vsyncNow(scanout);
    }

    namespace
    {
    void vsyncNow(const Scanout &scanout)
    {
        ps2perf::Scope scope(ps2perf::GpuFrame);
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        registerThreadLocked(s);

        if (const uint32_t wanted = g_requestedSuperSampling.exchange(0u); wanted != 0u && wanted != s.superSampling)
        {
            s.iface.flush();
            s.iface.set_super_sampling_rate(toSuperSampling(wanted), true, false);
            s.superSampling = static_cast<uint32_t>(toSuperSampling(wanted));
            g_currentSuperSampling.store(s.superSampling);
            std::fprintf(stderr, "[gpu] internal resolution: %ux super-sampling\n", s.superSampling);
        }

        copyPrivRegistersLocked(s, scanout);
        const uint32_t field = static_cast<uint32_t>((scanout.csr >> 13) & 1u);
        // Buffer identity for the flip counter: the circuit that is shown (this game uses circuit 2 only).
        const bool circuit2Only = (scanout.pmode & 3u) == 2u;
        const uint64_t dispfb = circuit2Only ? scanout.dispfb2 : scanout.dispfb1;
        const uint32_t displayFbp = static_cast<uint32_t>(dispfb ^ (dispfb >> 32));

        s.iface.flush();
        // Display (CRTC) registers, logged when they change: PS2X_GS_CRTC_LOG=1.
        static const bool crtcLog = []()
        {
            const char *env = std::getenv("PS2X_GS_CRTC_LOG");
            return env && *env && *env != '0';
        }();
        if (crtcLog)
        {
            static uint64_t last[8] = {};
            const uint64_t now[8] = {scanout.pmode, scanout.smode2, scanout.dispfb1, scanout.display1,
                                     scanout.dispfb2, scanout.display2, scanout.bgcolor, scanout.smode1};
            if (std::memcmp(last, now, sizeof(now)) != 0)
            {
                std::memcpy(last, now, sizeof(now));
                auto fb = [](uint64_t v, char *out, size_t n)
                {
                    std::snprintf(out, n, "FBP %u FBW %u PSM %u DBX %u DBY %u", unsigned(v & 0x1FF), unsigned((v >> 9) & 0x3F),
                                  unsigned((v >> 15) & 0x1F), unsigned((v >> 32) & 0x7FF), unsigned((v >> 43) & 0x7FF));
                };
                auto disp = [](uint64_t v, char *out, size_t n)
                {
                    std::snprintf(out, n, "DX %u DY %u MAGH %u MAGV %u DW %u DH %u", unsigned(v & 0xFFF), unsigned((v >> 12) & 0x7FF),
                                  unsigned((v >> 23) & 0xF), unsigned((v >> 27) & 0x3), unsigned((v >> 32) & 0xFFF), unsigned((v >> 44) & 0x7FF));
                };
                char f1[96], d1[96], f2[96], d2[96];
                fb(now[2], f1, sizeof(f1));
                disp(now[3], d1, sizeof(d1));
                fb(now[4], f2, sizeof(f2));
                disp(now[5], d2, sizeof(d2));
                const uint64_t pm = now[0];
                std::fprintf(stderr,
                             "[crtc] PMODE EN1 %u EN2 %u MMOD %u AMOD %u SLBG %u ALP %u | SMODE2 INT %u FFMD %u | field %u\n"
                             "[crtc]   1: %s | %s\n[crtc]   2: %s | %s | BG %06llx\n",
                             unsigned(pm & 1), unsigned((pm >> 1) & 1), unsigned((pm >> 5) & 1), unsigned((pm >> 6) & 1),
                             unsigned((pm >> 7) & 1), unsigned((pm >> 8) & 0xFF), unsigned(now[1] & 1), unsigned((now[1] >> 1) & 1),
                             field, f1, d1, f2, d2, static_cast<unsigned long long>(now[6] & 0xFFFFFF));
            }
        }
        // Scan-out options (PS2X_PGS_PROGRESSIVE / PS2X_PGS_ANTI_BLUR = 0 or 1) for comparing looks.
        static const int progressiveOverride = []()
        {
            const char *env = std::getenv("PS2X_PGS_PROGRESSIVE");
            return env && *env ? (*env != '0' ? 1 : 0) : -1;
        }();
        static const int antiBlurOverride = []()
        {
            const char *env = std::getenv("PS2X_PGS_ANTI_BLUR");
            return env && *env ? (*env != '0' ? 1 : 0) : -1;
        }();
        VSyncInfo info = {};
        info.phase = field;
        info.dst_layout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        info.dst_stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        info.dst_access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        info.force_progressive = progressiveOverride != 0;
        info.anti_blur = antiBlurOverride != 0;
        info.adapt_to_internal_horizontal_resolution = true;
        info.high_resolution_scanout = s.superSampling > 1u;
        const ScanoutResult result = s.iface.vsync(info);
        ps2perf::count(ps2perf::GpuFrames);
        if (ps2perf::enabled())
        {
            static uint32_t budgetTick = 0;
            if ((budgetTick++ % 120u) == 0u)
            {
                HeapBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
                s.device.get_memory_budget(budgets);
                const VkPhysicalDeviceMemoryProperties &memory = s.device.get_memory_properties();
                uint64_t used = 0, budget = 0;
                for (uint32_t i = 0; i < memory.memoryHeapCount; ++i)
                {
                    if ((memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0u)
                        continue;
                    used += budgets[i].device_usage;
                    budget += budgets[i].budget_size;
                }
                ps2perf::setGauge(ps2perf::GpuMemoryUsedBytes, used);
                ps2perf::setGauge(ps2perf::GpuMemoryBudgetBytes, budget);
            }
        }
        static const bool debugFrames = std::getenv("PS2X_GPU_DEBUG") != nullptr;
        if (debugFrames)
        {
            static uint32_t n = 0;
            if ((++n % 61u) == 0u)
                std::fprintf(stderr, "[gpu-debug] vsync %u frame-context %u/%u image %ux%u\n", n, s.device.get_current_frame_context(),
                             s.device.get_num_frame_contexts(), result.image ? result.image->get_width() : 0u,
                             result.image ? result.image->get_height() : 0u);
        }

        if (ps2perf::enabled())
        {
            const FlushStats stats = s.iface.consume_flush_stats();
            ps2perf::count(ps2perf::GpuPrimitives, stats.num_primitives);
            ps2perf::count(ps2perf::GpuRenderPasses, stats.num_render_passes);
            ps2perf::count(ps2perf::GpuCopies, stats.num_copies);
            ps2perf::count(ps2perf::GpuPaletteUpdates, stats.num_palette_updates);
            double gpuSeconds = 0.0;
            for (int t = 0; t < static_cast<int>(TimestampType::Count); ++t)
                gpuSeconds += s.iface.get_accumulated_timestamps(static_cast<TimestampType>(t));
            if (gpuSeconds > s.gpuSecondsSeen)
            {
                ps2perf::count(ps2perf::GpuBusyNs, static_cast<uint64_t>((gpuSeconds - s.gpuSecondsSeen) * 1e9));
                s.gpuSecondsSeen = gpuSeconds;
            }
        }

        collectReadbacksLocked(s);
        static const bool noReadback = std::getenv("PS2X_GPU_NO_READBACK") != nullptr;
        if (result.image && !noReadback)
            queueReadbackLocked(s, result, displayFbp);
    }
    }

    bool takeFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, uint32_t &displayFbp)
    {
        State &s = st();
        std::lock_guard<std::mutex> lock(s.frameMutex);
        if (!s.frameFresh)
            return false;
        rgba.swap(s.frame);
        width = s.frameWidth;
        height = s.frameHeight;
        displayFbp = s.frameFbp;
        s.frameFresh = false;
        return true;
    }

    void requestSuperSampling(uint32_t rate) noexcept
    {
        g_requestedSuperSampling.store(rate == 0u ? 1u : rate);
    }

    uint32_t currentSuperSampling() noexcept
    {
        return active() ? g_currentSuperSampling.load() : 1u;
    }

    uint32_t readLocalToHost(uint8_t *dst, uint32_t bytes)
    {
        if (!dst || bytes < 16u || !active())
            return 0u;
        ps2perf::Scope scope(ps2perf::GpuSync); // downloads are counted where they start (VIF1_STAT.FDR)
        drainFeed(); // the transfer set-up may still be on its way to paraLLEl-GS
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        registerThreadLocked(s);
        const uint32_t words = bytes / 16u;
        s.iface.read_transfer_fifo(dst, words);
        return words * 16u;
    }

    void shutdown()
    {
        if (!active())
            return;
        stopFeed();
        State &s = st();
        std::lock_guard<std::mutex> lock(s.mutex);
        registerThreadLocked(s);
        g_active.store(false, std::memory_order_release);
        s.device.wait_idle();
    }
}

#else // !PS2X_HAVE_PGS

namespace ps2gpu
{
    bool compiledIn() noexcept { return false; }
    bool init(GSRegisters *, const Options &, Description &, std::string &outError)
    {
        outError = "this build has no Vulkan GS renderer";
        return false;
    }
    bool active() noexcept { return false; }
    void gifTransfer(uint32_t, const uint8_t *, size_t) {}
    void writeRegister(uint8_t, uint64_t) {}
    Scanout captureScanout(const GSRegisters &) { return Scanout{}; }
    void vsync(const Scanout &) {}
    bool takeFrame(std::vector<uint8_t> &, uint32_t &, uint32_t &, uint32_t &) { return false; }
    uint32_t readLocalToHost(uint8_t *, uint32_t) { return 0u; }
    void drainFeed() {}
    void requestSuperSampling(uint32_t) noexcept {}
    uint32_t currentSuperSampling() noexcept { return 1u; }
    void shutdown() {}
}

#endif
