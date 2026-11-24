#include <deque>
#include <emmintrin.h>
#include <chrono>
#include <filesystem>
#include <condition_variable>
#include "runtime/ps2_fast_sync.h"
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
#include "runtime/ps2_host_wait.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_texture_replace.h"

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

namespace ps2launcher
{
    std::string executableDirectory(); // ps2_file_dialog.cpp
}

namespace
{
    // fopen for a UTF-8 path (Windows needs the wide-character name).
    FILE *ps2gpuOpenFile(const std::string &path, const char *mode)
    {
#if defined(_WIN32)
        const std::wstring wide = std::filesystem::u8path(path).wstring();
        std::wstring wmode(mode, mode + std::strlen(mode));
        return _wfopen(wide.c_str(), wmode.c_str());
#else
        return std::fopen(path.c_str(), mode);
#endif
    }
}

namespace ps2gpu
{
    std::atomic<uint32_t> g_requestedSuperSampling{0u};
    std::atomic<uint32_t> g_currentSuperSampling{1u};
    std::atomic<bool> g_rawBgraFrames{false}; // the window can draw raw BGRA pictures
    std::atomic<uint64_t> g_syncWaitNs{0};    // time the game spent in GS downloads (waiting for the GPU)

    // How much drawing paraLLEl-GS collects before handing it to the GPU. Large batches cost the
    // CPU the least, but a game that reads the picture back every frame (this one does) then
    // waits while the GPU draws the whole frame from scratch. Small batches let the GPU draw
    // while the frame is still being built, so a download only waits for the last bit.
    constexpr uint32_t kLargeBatch = 65536u;
    constexpr uint32_t kSmallBatch = 4096u;
    struct BatchPolicy
    {
        uint32_t current = 0u;
        double waitEmaMs = 0.0;         // download wait per frame (smoothed)
        double smallUntil = 0.0;        // keep small batches until this time
        double holdSeconds = 30.0;      // how long small batches are kept after waits are seen
    } g_batch;
    namespace
    {
        void startFeed();
        void stopFeed();
    }

    namespace
    {
        using namespace Vulkan;
        using namespace ParallelGS;

        // shadercache.bin: a Vulkan pipeline cache with Granite's header (GPU UUID + hash), so a
        // cache from another graphics card or driver is ignored and rebuilt.
        size_t g_shaderCacheLoaded = 0; // bytes read at start: the game only writes the file back when it grew
        std::atomic<uint32_t> g_stalledCompiles{0}; // shaders compiled while the game waited this session

        bool loadShaderCache(Device &device)
        {
            std::vector<uint8_t> data;
            if (FILE *f = ps2gpuOpenFile(shaderCachePath(), "rb"))
            {
                std::fseek(f, 0, SEEK_END);
                const long size = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                if (size > 0)
                {
                    data.resize(static_cast<size_t>(size));
                    if (std::fread(data.data(), 1, data.size(), f) != data.size())
                        data.clear();
                }
                std::fclose(f);
            }
            const bool ok = device.init_pipeline_cache(data.empty() ? nullptr : data.data(), data.size());
            g_shaderCacheLoaded = ok ? data.size() : 0u;
            ps2perf::graphicsNote(" Shader cache: %s (%.1f MB)", data.empty() ? "none yet, shaders are compiled as needed" : "loaded",
                                  static_cast<double>(data.size()) / (1024.0 * 1024.0));
            return ok;
        }

        bool saveShaderCache(Device &device, bool onlyIfGrown)
        {
            const size_t size = device.get_pipeline_cache_size();
            if (size == 0)
                return false;
            // Shaders compiled during play go into the file even when the driver's cache data
            // did not grow in size, so the next start has them.
            const uint32_t compiled = g_stalledCompiles.load(std::memory_order_relaxed);
            if (onlyIfGrown && size <= g_shaderCacheLoaded && compiled == 0u)
                return true; // no new shaders this run
            if (onlyIfGrown)
                std::fprintf(stderr, "[gpu] shader cache: %.1f MB written at quit (was %.1f MB; %u shaders compiled during play)\n",
                             static_cast<double>(size) / (1024.0 * 1024.0), static_cast<double>(g_shaderCacheLoaded) / (1024.0 * 1024.0),
                             compiled);
            std::vector<uint8_t> data(size);
            if (!device.get_pipeline_cache_data(data.data(), size))
                return false;
            const std::string path = shaderCachePath();
            const std::string temp = path + ".tmp";
            FILE *f = ps2gpuOpenFile(temp, "wb");
            if (!f)
                return false;
            const bool written = std::fwrite(data.data(), 1, size, f) == size;
            std::fclose(f);
            std::error_code ec;
            if (written)
                std::filesystem::rename(std::filesystem::u8path(temp), std::filesystem::u8path(path), ec);
            else
                std::filesystem::remove(std::filesystem::u8path(temp), ec);
            return written && !ec;
        }

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
                if (std::strstr(text, "Stalled compile"))
                    g_stalledCompiles.fetch_add(1u, std::memory_order_relaxed);
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
            uint64_t tick = 0;
            uint32_t phase = 0; // the interlace field it was scanned out as
            bool pending = false;
        };

        struct State
        {
            ps2sync::Mutex mutex; // everything that touches Vulkan / the GS emulator
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

            ps2sync::Mutex frameMutex; // the newest picture, handed to the window thread
            std::vector<uint8_t> frame;
            uint32_t frameWidth = 0;
            uint32_t frameHeight = 0;
            uint32_t frameFbp = 0;
            uint64_t frameTick = 0;
            bool frameFresh = false;
            bool frameRawBgra = false; // frame holds the GPU's BGRA bytes as they came back
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

        // Diagnostics: PS2X_DUMP_SCANOUT=<folder> writes the top-left quarter of the scanned-out
        // pictures of VBlanks PS2X_DUMP_SCANOUT_FROM .. +PS2X_DUMP_SCANOUT_COUNT (default 120) as
        // PPM files, with their VBlank, field and buffer in scanout.txt.
        void dumpScanoutLocked(State &s, const Readback &slot, bool rawBgra)
        {
            static const char *dir = std::getenv("PS2X_DUMP_SCANOUT");
            if (!dir || !*dir)
                return;
            static const uint64_t from = []()
            {
                const char *v = std::getenv("PS2X_DUMP_SCANOUT_FROM");
                return v ? std::strtoull(v, nullptr, 10) : 0ull;
            }();
            static const uint64_t count = []()
            {
                const char *v = std::getenv("PS2X_DUMP_SCANOUT_COUNT");
                return v ? std::strtoull(v, nullptr, 10) : 120ull;
            }();
            if (slot.tick < from || slot.tick >= from + count || s.frame.empty())
                return;
            const uint32_t w = slot.width / 2u, h = slot.height / 2u;
            char path[512];
            std::snprintf(path, sizeof(path), "%s/scanout_%06llu.ppm", dir, static_cast<unsigned long long>(slot.tick));
            if (std::FILE *f = std::fopen(path, "wb"))
            {
                std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                std::vector<uint8_t> row(static_cast<size_t>(w) * 3u);
                for (uint32_t y = 0; y < h; ++y)
                {
                    const uint8_t *src = s.frame.data() + static_cast<size_t>(y) * slot.width * 4u;
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        row[x * 3u + 0u] = src[x * 4u + (rawBgra ? 2u : 0u)];
                        row[x * 3u + 1u] = src[x * 4u + 1u];
                        row[x * 3u + 2u] = src[x * 4u + (rawBgra ? 0u : 2u)];
                    }
                    std::fwrite(row.data(), 1, row.size(), f);
                }
                std::fclose(f);
            }
            std::snprintf(path, sizeof(path), "%s/scanout.txt", dir);
            if (std::FILE *f = std::fopen(path, "ab"))
            {
                std::fprintf(f, "tick %llu phase %u fbp %u size %ux%u\n", static_cast<unsigned long long>(slot.tick), slot.phase,
                             slot.displayFbp, slot.width, slot.height);
                std::fclose(f);
            }
        }

        // Moves a finished copy into the window thread's frame.
        void publishLocked(State &s, Readback &slot)
        {
            const auto *src = static_cast<const uint32_t *>(s.device.map_host_buffer(*slot.buffer, MEMORY_ACCESS_READ_BIT));
            const size_t pixels = static_cast<size_t>(slot.width) * slot.height;
            {
                std::lock_guard<ps2sync::Mutex> lock(s.frameMutex);
                s.frame.resize(pixels * 4u);
                uint32_t *dst = reinterpret_cast<uint32_t *>(s.frame.data());
                const bool bgra = slot.format == VK_FORMAT_B8G8R8A8_UNORM || slot.format == VK_FORMAT_B8G8R8A8_SRGB;
                const bool raw = bgra && g_rawBgraFrames.load(std::memory_order_relaxed);
                if (src)
                {
                    if (raw)
                        std::memcpy(dst, src, pixels * 4u); // the window's shader swaps red and blue
                    else if (bgra)
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
                s.frameTick = slot.tick;
                s.frameRawBgra = raw;
                s.frameFresh = true;
                dumpScanoutLocked(s, slot, raw);
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

        void queueReadbackLocked(State &s, const ScanoutResult &result, uint32_t displayFbp, uint64_t tick)
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
            slot.tick = tick;
            slot.phase = result.interlace_phase;

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

        // Once a frame: picks the batch size (see BatchPolicy). Above native resolution the GPU has
        // real work every frame, so small batches always pay; at native resolution they are used
        // while downloads are seen waiting, then large batches are tried again.
        void updateBatchPolicyLocked(State &s)
        {
            const int fixed = ps2settings::get().gpuFlushPrimitives;
            const double waitMs = static_cast<double>(g_syncWaitNs.exchange(0u, std::memory_order_relaxed)) / 1e6;
            BatchPolicy &b = g_batch;
            b.waitEmaMs += (waitMs - b.waitEmaMs) * 0.1;
            const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            uint32_t wanted = kLargeBatch;
            if (fixed > 0)
                wanted = static_cast<uint32_t>(fixed);
            else if (s.superSampling > 1u)
                wanted = kSmallBatch;
            else
            {
                if (b.waitEmaMs > 1.0)
                {
                    if (b.current == kLargeBatch && b.smallUntil > 0.0)
                        b.holdSeconds = std::min(b.holdSeconds * 2.0, 600.0); // large batches stalled again
                    b.smallUntil = nowSeconds + b.holdSeconds;
                }
                wanted = nowSeconds < b.smallUntil ? kSmallBatch : kLargeBatch;
            }
            if (wanted != b.current)
            {
                if (b.current != 0u)
                    ps2perf::graphicsNote("           GPU work is now handed over every %u primitives (downloads waited %.1f ms a frame)", wanted,
                                          b.waitEmaMs);
                b.current = wanted;
                ParallelGS::set_minimum_primitives_for_flush(wanted);
            }
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
        std::lock_guard<ps2sync::Mutex> lock(s.mutex);
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
        loadShaderCache(s.device);

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
        if (ps2perf::enabled() && !std::getenv("PS2X_GPU_NO_TIMESTAMPS") && ps2settings::get().gpuTimestamps != 0)
        {
            DebugMode debug = {};
            debug.timestamps = true; // GPU time per second in Graphicslogs.txt
            s.iface.set_debug_mode(debug);
        }
        // How often queued drawing goes to the GPU: chosen once a frame by updateBatchPolicyLocked
        // (settings.ini gpu_flush_primitives fixes it).
        g_batch = BatchPolicy{};
        updateBatchPolicyLocked(s);
        setFeedBatching(ps2settings::get().gpuFeedBatch != 0);
        // Replacement textures (textures\font_atlas.png): the renderer samples them instead of the
        // game's own, picking the mip level that fits each letter's size on screen.
        {
            std::vector<std::string> notes;
            std::vector<ps2texrep::Replacement> replacements = ps2texrep::loadAll(notes);
            for (const std::string &note : notes)
                ps2perf::graphicsNote(" Replacement textures: %s", note.c_str());
            for (ps2texrep::Replacement &r : replacements)
            {
                ParallelGS::GSInterface::TextureReplacement t;
                t.tbp0 = r.tbp0;
                t.tbw = r.tbw;
                t.psm = r.psm;
                t.tw = r.tw;
                t.th = r.th;
                t.cbp = r.cbp;
                t.width = r.width;
                t.height = r.height;
                const size_t levels = r.levels.size();
                t.levels = std::move(r.levels);
                const bool ok = s.iface.set_texture_replacement(t);
                ps2perf::graphicsNote(" Replacement texture \"%s\": %s (%u x %u, %zu mip levels) %s", r.name.c_str(), r.path.c_str(), r.width, r.height,
                                      levels, ok ? "in use" : "could not be used");
            }
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
            std::function<void()> task; // runs in order instead (an asynchronous GS download)
        };

        constexpr size_t kFeedMaxBytes = 32u << 20; // the producer waits beyond this much data
        constexpr uint32_t kFeedMaxFrames = 1u;     // ... or when a whole frame is still waiting

        ps2sync::Mutex g_feedMutex;
        ps2sync::CondVar g_feedWorkCv;
        ps2sync::CondVar g_feedDoneCv;
        std::deque<FeedItem> g_feed;
        size_t g_feedBytes = 0;
        uint32_t g_feedFrames = 0;
        bool g_feedBusy = false;
        bool g_feedStop = false;
        bool g_feedSleeping = false;           // under g_feedMutex: the feed thread waits for work
        uint32_t g_feedWaiters = 0;            // under g_feedMutex: threads waiting on g_feedDoneCv
        std::vector<std::vector<uint8_t>> g_feedPool; // under g_feedMutex: emptied packet buffers to reuse
        std::thread g_feedThread;
        std::atomic<bool> g_feedOn{false};
        thread_local bool t_onFeedThread = false;
        std::atomic<uint64_t> g_feedItemCount{0}, g_feedSleepCount{0}; // packets handed over, times the feed thread slept
        // Packets come in about 10 microseconds apart in a big battle (90,000+ a second). Waking the
        // feed thread for each one cost more kernel time (on both threads) than the packets
        // themselves, so a sleeping feed thread is woken once a batch has built up, at every
        // VBlank, and whenever someone needs what is queued (a GS register read, a drain).
        constexpr size_t kFeedWakeItems = 24u;
        constexpr size_t kFeedWakeBytes = 96u << 10;
        std::atomic<bool> g_feedBatching{true};
        size_t g_feedPendingItems = 0; // under g_feedMutex: queued since the feed thread last took the queue

        void gifTransferNow(uint32_t path, const uint8_t *data, size_t size);
        void vsyncNow(const Scanout &scanout);

        void feedMain()
        {
            t_onFeedThread = true;
            ps2perf::registerThread("GS feed thread");
            ps2host::raiseThisThread();
            std::deque<FeedItem> batch;
            for (;;)
            {
                {
                    std::unique_lock<ps2sync::Mutex> lock(g_feedMutex);
                    if (g_feed.empty() && !g_feedStop)
                    {
                        g_feedSleepCount.fetch_add(1u, std::memory_order_relaxed);
                        g_feedSleeping = true;
                        g_feedWorkCv.wait(lock, [] { return g_feedStop || !g_feed.empty(); });
                        g_feedSleeping = false;
                    }
                    if (g_feed.empty())
                        return; // stopping, and everything handed over has been fed
                    batch.swap(g_feed); // everything queued so far, in order
                    g_feedPendingItems = 0;
                    g_feedBusy = true;
                }
                for (FeedItem &item : batch)
                {
                    if (item.task)
                    {
                        item.task();
                        item.task = nullptr;
                    }
                    else if (item.vsync)
                        vsyncNow(item.scanout);
                    else
                        gifTransferNow(item.path, item.bytes.data(), item.bytes.size());
                    // Give this item's share of the budget back right away, so a waiting producer
                    // does not wait for the rest of the batch.
                    bool wake = false;
                    {
                        std::lock_guard<ps2sync::Mutex> lock(g_feedMutex);
                        g_feedBytes -= item.bytes.size();
                        if (item.vsync)
                            --g_feedFrames;
                        if (item.bytes.capacity() != 0u && g_feedPool.size() < 64u)
                        {
                            item.bytes.clear();
                            g_feedPool.push_back(std::move(item.bytes));
                        }
                        wake = g_feedWaiters != 0u;
                    }
                    if (wake)
                        g_feedDoneCv.notify_all();
                }
                batch.clear();
                bool wake = false;
                {
                    std::lock_guard<ps2sync::Mutex> lock(g_feedMutex);
                    g_feedBusy = false;
                    wake = g_feedWaiters != 0u;
                }
                if (wake)
                    g_feedDoneCv.notify_all();
            }
        }

        bool feedQueues()
        {
            return g_feedOn.load(std::memory_order_acquire) && !t_onFeedThread;
        }

        void enqueueFeed(FeedItem &&item)
        {
            std::unique_lock<ps2sync::Mutex> lock(g_feedMutex);
            auto roomFor = [&item]
            { return g_feedStop || (g_feedBytes < kFeedMaxBytes && (!item.vsync || g_feedFrames < kFeedMaxFrames)); };
            if (!roomFor())
            {
                ++g_feedWaiters;
                g_feedDoneCv.wait(lock, roomFor);
                --g_feedWaiters;
            }
            g_feedBytes += item.bytes.size();
            if (item.vsync)
                ++g_feedFrames;
            const bool vsync = item.vsync;
            g_feed.push_back(std::move(item));
            ++g_feedPendingItems;
            const bool wake = g_feedSleeping &&
                              (vsync || !g_feedBatching.load(std::memory_order_relaxed) || g_feedPendingItems >= kFeedWakeItems ||
                               g_feedBytes >= kFeedWakeBytes);
            lock.unlock();
            g_feedItemCount.fetch_add(1u, std::memory_order_relaxed);
            if (wake)
                g_feedWorkCv.notify_one();
        }

        // A packet buffer for the feed: an emptied one when there is one (keeps its memory).
        std::vector<uint8_t> takeFeedBuffer()
        {
            std::lock_guard<ps2sync::Mutex> lock(g_feedMutex);
            if (g_feedPool.empty())
                return {};
            std::vector<uint8_t> buffer = std::move(g_feedPool.back());
            g_feedPool.pop_back();
            return buffer;
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
                std::lock_guard<ps2sync::Mutex> lock(g_feedMutex);
                g_feedStop = true;
            }
            g_feedWorkCv.notify_all();
            g_feedDoneCv.notify_all();
            if (g_feedThread.joinable())
                g_feedThread.join();
        }
    }

    void kickFeed() noexcept
    {
        if (!g_feedOn.load(std::memory_order_relaxed) || t_onFeedThread)
            return;
        bool wake = false;
        {
            std::lock_guard<ps2sync::Mutex> lock(g_feedMutex);
            wake = g_feedSleeping && !g_feed.empty();
        }
        if (wake)
            g_feedWorkCv.notify_one();
    }

    void setFeedBatching(bool enabled) noexcept
    {
        g_feedBatching.store(enabled, std::memory_order_relaxed);
    }

    void drainFeed()
    {
        if (!g_feedOn.load(std::memory_order_acquire) || t_onFeedThread)
            return;
        std::unique_lock<ps2sync::Mutex> lock(g_feedMutex);
        auto drained = [] { return g_feedStop || (g_feed.empty() && !g_feedBusy); };
        if (g_feedSleeping && !g_feed.empty())
            g_feedWorkCv.notify_one();
        if (!drained())
        {
            ++g_feedWaiters;
            g_feedDoneCv.wait(lock, drained);
            --g_feedWaiters;
        }
    }

    void gifTransfer(uint32_t path, const uint8_t *data, size_t size)
    {
        if (!data || size < 16u)
            return;
        if (feedQueues())
        {
            FeedItem item;
            item.path = path;
            item.bytes = takeFeedBuffer();
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
        std::lock_guard<ps2sync::Mutex> lock(s.mutex);
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
            std::lock_guard<ps2sync::Mutex> lock(s.mutex);
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
        std::lock_guard<ps2sync::Mutex> lock(s.mutex);
        registerThreadLocked(s);

        if (const uint32_t wanted = g_requestedSuperSampling.exchange(0u); wanted != 0u && wanted != s.superSampling)
        {
            s.iface.flush();
            s.iface.set_super_sampling_rate(toSuperSampling(wanted), true, false);
            s.superSampling = static_cast<uint32_t>(toSuperSampling(wanted));
            g_currentSuperSampling.store(s.superSampling);
            std::fprintf(stderr, "[gpu] internal resolution: %ux super-sampling\n", s.superSampling);
        }

        updateBatchPolicyLocked(s);
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
        // Field-rendered games: scan a buffer out as the field it was drawn for, so a frame the game
        // shows late or twice does not jump a line (PS2X_PGS_FIELD_FOLLOW=0 goes by the CRTC field).
        static const bool followDrawnField = []()
        {
            const char *env = std::getenv("PS2X_PGS_FIELD_FOLLOW");
            return !(env && *env == '0');
        }();
        info.follow_drawn_field = followDrawnField;
        // Tests: PS2X_TEST_FIELD_GLITCH=N scans every Nth VBlank out as the other field, as when the
        // game shows a frame late.
        static const uint64_t glitchEvery = []()
        {
            const char *env = std::getenv("PS2X_TEST_FIELD_GLITCH");
            return env ? std::strtoull(env, nullptr, 10) : 0ull;
        }();
        if (glitchEvery != 0u && scanout.tick % glitchEvery == 0u)
            info.phase ^= 1u;
        const ScanoutResult result = s.iface.vsync(info);
        ps2perf::count(ps2perf::GpuFrames);
        {
            static uint64_t lastFollows = 0;
            const uint64_t follows = s.iface.get_field_follow_count();
            if (follows != lastFollows)
            {
                ps2perf::count(ps2perf::GpuFieldFollows, follows - lastFollows);
                lastFollows = follows;
            }
        }
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
            double byStage[static_cast<int>(TimestampType::Count)] = {};
            for (int t = 0; t < static_cast<int>(TimestampType::Count); ++t)
            {
                byStage[t] = s.iface.get_accumulated_timestamps(static_cast<TimestampType>(t));
                gpuSeconds += byStage[t];
            }
            if (gpuSeconds > s.gpuSecondsSeen)
            {
                ps2perf::count(ps2perf::GpuBusyNs, static_cast<uint64_t>((gpuSeconds - s.gpuSecondsSeen) * 1e9));
                s.gpuSecondsSeen = gpuSeconds;
            }
            // Every ten seconds: which GPU stage the time went to.
            static double stageSeen[static_cast<int>(TimestampType::Count)] = {};
            static auto stageSince = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const double span = std::chrono::duration<double>(now - stageSince).count();
            if (span >= 10.0)
            {
                double ms[static_cast<int>(TimestampType::Count)];
                for (int t = 0; t < static_cast<int>(TimestampType::Count); ++t)
                {
                    ms[t] = (byStage[t] - stageSeen[t]) * 1000.0 / span;
                    stageSeen[t] = byStage[t];
                }
                stageSince = now;
                ps2perf::graphicsNote("           GPU time by stage (ms/s): sync to VRAM %.1f, VRAM copies %.1f, palettes %.1f, texture upload %.1f, "
                                      "triangle setup %.1f, binning %.1f, shading %.1f, readback %.1f, vsync/scanout %.1f",
                                      ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[6], ms[7], ms[8]);
                ps2perf::graphicsNote("           GS feed thread: %.0f packets/s handed over, slept %.0f times/s",
                                      static_cast<double>(g_feedItemCount.exchange(0)) / span, static_cast<double>(g_feedSleepCount.exchange(0)) / span);
                uint64_t fc[ParallelGS::FlushCounterCount] = {};
                ParallelGS::take_flush_counters(fc, ParallelGS::FlushCounterCount);
                auto per = [span](uint64_t v) { return static_cast<double>(v) / span; };
                const int rb = ParallelGS::FlushCounterReasonBase;
                if (fc[ParallelGS::FlushCounterReplacedDraws] != 0)
                    ps2perf::graphicsNote("           replacement textures: %.0f draws/s", per(fc[ParallelGS::FlushCounterReplacedDraws]));
                ps2perf::graphicsNote("           paraLLEl-GS: %.0f primitives/s, %.0f flushes/s -> %.0f GPU queue submits/s | work-queued flushes %.0f/s (primitives %.0f, passes %.0f, images %.0f, other %.0f) | "
                                      "render passes ended by: FB change %.0f, overflow %.0f, texture hazard %.0f, copy hazard %.0f, submission %.0f, pressure %.0f, CPU access %.0f /s",
                                      per(fc[ParallelGS::FlushCounterPrimitives]), per(fc[ParallelGS::FlushCounterSubmitCalls]), per(fc[ParallelGS::FlushCounterQueueSubmits]), per(fc[ParallelGS::FlushCounterPressure]),
                                      per(fc[ParallelGS::FlushCounterPressurePrimitives]), per(fc[ParallelGS::FlushCounterPressurePasses]),
                                      per(fc[ParallelGS::FlushCounterPressureImages]), per(fc[ParallelGS::FlushCounterPressureOther]),
                                      per(fc[rb + 0]), per(fc[rb + 1]), per(fc[rb + 2]), per(fc[rb + 3]), per(fc[rb + 4]), per(fc[rb + 5]), per(fc[rb + 6]));
            }
        }

        collectReadbacksLocked(s);
        static const bool noReadback = std::getenv("PS2X_GPU_NO_READBACK") != nullptr;
        if (result.image && !noReadback)
            queueReadbackLocked(s, result, displayFbp, scanout.tick);
    }
    }

    bool takeFrame(std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height, uint32_t &displayFbp, bool &rawBgra,
                   uint64_t &tick)
    {
        State &s = st();
        std::lock_guard<ps2sync::Mutex> lock(s.frameMutex);
        if (!s.frameFresh)
            return false;
        pixels.swap(s.frame);
        width = s.frameWidth;
        height = s.frameHeight;
        displayFbp = s.frameFbp;
        tick = s.frameTick;
        rawBgra = s.frameRawBgra;
        s.frameFresh = false;
        return true;
    }

    void setRawBgraFrames(bool enabled) noexcept
    {
        g_rawBgraFrames.store(enabled, std::memory_order_relaxed);
    }

    void requestSuperSampling(uint32_t rate) noexcept
    {
        g_requestedSuperSampling.store(rate == 0u ? 1u : rate);
    }

    uint32_t currentSuperSampling() noexcept
    {
        return active() ? g_currentSuperSampling.load() : 1u;
    }

    bool readLocalToHostAsync(uint32_t bytes, std::function<void(const uint8_t *, uint32_t)> done)
    {
        if (bytes < 16u || bytes > 4096u || !active() || !feedQueues())
            return false;
        FeedItem item;
        item.task = [bytes, done = std::move(done)]()
        {
            uint8_t data[4096];
            uint32_t got = 0;
            {
                State &s = st();
                std::lock_guard<ps2sync::Mutex> lock(s.mutex);
                registerThreadLocked(s);
                const uint32_t words = bytes / 16u;
                s.iface.read_transfer_fifo(data, words);
                got = words * 16u;
            }
            done(data, got);
        };
        enqueueFeed(std::move(item));
        return true;
    }

    uint32_t readLocalToHost(uint8_t *dst, uint32_t bytes)
    {
        if (!dst || bytes < 16u || !active())
            return 0u;
        ps2perf::Scope scope(ps2perf::GpuSync); // downloads are counted where they start (VIF1_STAT.FDR)
        const auto start = std::chrono::steady_clock::now();
        drainFeed(); // the transfer set-up may still be on its way to paraLLEl-GS
        State &s = st();
        std::lock_guard<ps2sync::Mutex> lock(s.mutex);
        registerThreadLocked(s);
        const uint32_t words = bytes / 16u;
        s.iface.read_transfer_fifo(dst, words);
        g_syncWaitNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - start).count()),
                               std::memory_order_relaxed);
        return words * 16u;
    }

    void shutdown()
    {
        if (!active())
            return;
        stopFeed();
        State &s = st();
        std::lock_guard<ps2sync::Mutex> lock(s.mutex);
        registerThreadLocked(s);
        g_active.store(false, std::memory_order_release);
        s.device.wait_idle();
        saveShaderCache(s.device, true);
    }

    std::string shaderCachePath()
    {
        return ps2launcher::executableDirectory() + "shadercache.bin";
    }

    uint64_t shaderCacheBytes()
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(std::filesystem::u8path(shaderCachePath()), ec);
        return ec ? 0u : static_cast<uint64_t>(size);
    }

    bool buildShaderCache(ShaderBuildProgress &progress, std::string &gpuName, std::string &error)
    {
        progress.done = 0;
        progress.total = 0;
        progress.threads = (std::thread::hardware_concurrency() + 1u) / 2u;
        Util::register_thread_index(0);
        if (!Context::init_loader(nullptr))
        {
            error = "No Vulkan driver was found.";
            return false;
        }
        Context context;
        context.set_num_thread_indices(1);
        ContextCreationFlags flags = CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT;
        if (!context.init_instance_and_device(nullptr, 0, nullptr, 0, flags))
        {
            error = "The Vulkan device could not be created.";
            return false;
        }
        Device device;
        device.set_context(context);
        device.init_frame_contexts(4);
        gpuName = device.get_gpu_properties().deviceName;
        loadShaderCache(device);
        {
            GSInterface iface;
            GSOptions options = {};
            options.vram_size = 4u * 1024u * 1024u;
            options.super_sampling = SuperSampling::X1;
            options.dynamic_super_sampling = true;
            options.super_sampled_textures = false;
            if (!iface.init(&device, options))
            {
                error = "This graphics card or driver lacks a Vulkan feature paraLLEl-GS needs.";
                return false;
            }
            // paraLLEl-GS starts compiling its shader variants on its own threads in init().
            for (;;)
            {
                uint32_t d = 0, t = 0;
                iface.get_shader_compilation_progress(d, t);
                progress.total = t;
                progress.done = std::min(d, t);
                if (iface.shader_compilation_finished() || progress.cancel.load())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            device.wait_idle();
        } // GSInterface goes away here: a cancelled build stops after the variants in flight

        if (!saveShaderCache(device, false))
        {
            error = "The shader cache could not be saved beside TMHO.exe.";
            return false;
        }
        return true;
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
    bool takeFrame(std::vector<uint8_t> &, uint32_t &, uint32_t &, uint32_t &, bool &, uint64_t &) { return false; }
    void setRawBgraFrames(bool) noexcept {}
    void kickFeed() noexcept {}
    void setFeedBatching(bool) noexcept {}
    uint32_t readLocalToHost(uint8_t *, uint32_t) { return 0u; }
    bool readLocalToHostAsync(uint32_t, std::function<void(const uint8_t *, uint32_t)>) { return false; }
    void drainFeed() {}
    void requestSuperSampling(uint32_t) noexcept {}
    uint32_t currentSuperSampling() noexcept { return 1u; }
    void shutdown() {}
    std::string shaderCachePath() { return {}; }
    uint64_t shaderCacheBytes() { return 0; }
    bool buildShaderCache(ShaderBuildProgress &, std::string &, std::string &error)
    {
        error = "this build has no Vulkan GS renderer";
        return false;
    }
}

#endif
