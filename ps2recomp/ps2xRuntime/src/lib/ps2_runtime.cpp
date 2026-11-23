#include <vector>
#include "ps2x/iop/iop_audio.h"
#include <cstdlib>
#include <cstdio>
#include "ps2_runtime.h"
#include "runtime/ps2_perf.h"
#include "ps2_gs_download.h"
#include "ps2_capture.h"
#include "runtime/ps2_mtvu.h"
#include "runtime/ps2_settings.h"
#include "runtime/gs/gs_gpu_renderer.h"
#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int period);
#endif
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"

#include <iostream>
#include <stdexcept>
#include <fstream>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>

namespace
{
    // raylib's own messages (window, OpenGL device, textures) go to stdout as before and to
    // Graphicslogs.txt; the OpenGL vendor/renderer/version lines describe the presenter.
    std::string g_glVendor, g_glRenderer, g_glVersion;

    void rayLogToGraphics(int logLevel, const char *text, va_list args)
    {
        char message[1024];
        std::vsnprintf(message, sizeof(message), text, args);
        const char *level = logLevel >= LOG_ERROR ? "ERROR" : logLevel >= LOG_WARNING ? "WARNING" : logLevel >= LOG_INFO ? "INFO" : "DEBUG";
        if (logLevel < LOG_INFO)
            return;
        std::fprintf(stdout, "%s: %s\n", level, message);
        ps2perf::graphicsNote("raylib %s: %s", level, message);
        auto take = [&](const char *prefix, std::string &out)
        {
            const char *found = std::strstr(message, prefix);
            if (found)
                out = found + std::strlen(prefix);
        };
        take("> Vendor:   ", g_glVendor);
        take("> Renderer: ", g_glRenderer);
        take("> Version:  ", g_glVersion);
    }

    unsigned softwareRasterThreads()
    {
        if (const char *env = std::getenv("PS2X_GS_THREADS"); env && *env)
            return static_cast<unsigned>(std::clamp(std::atoi(env), 1, 32));
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        return std::clamp(hw > 2u ? hw - 2u : 1u, 1u, 8u);
    }

    void describePresenter()
    {
        const int monitor = GetCurrentMonitor();
        char text[768];
        std::snprintf(text, sizeof(text), "OpenGL %s - %s / %s | monitor %dx%d @ %d Hz | window %dx%d",
                      g_glVersion.empty() ? "?" : g_glVersion.c_str(), g_glVendor.empty() ? "?" : g_glVendor.c_str(),
                      g_glRenderer.empty() ? "?" : g_glRenderer.c_str(), GetMonitorWidth(monitor), GetMonitorHeight(monitor),
                      GetMonitorRefreshRate(monitor), GetScreenWidth(), GetScreenHeight());
        ps2perf::setPresenter(text);
    }
}

namespace ps2_stubs
{
    void resetSifState();
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01F00000u;

    // Guest stacks for interrupt handlers, alarms and other callbacks the runtime runs on the game's
    // behalf. On a PS2 these run on the kernel's own stacks, inside the first 512 KB of RAM that the
    // EE kernel owns, so games treat the rest of RAM, up to its last byte, as theirs. (Twisted Metal:
    // Head-On builds its pause menu at the very top of RAM. With these stacks at the top, the VBlank
    // handler wrote over the menu's widgets 60 times a second and the menu never appeared.)
    // The kernel area holds nothing else here except the syscall-table mirror around 0x11F80 and a
    // probe word at 0x2F0; 0x70000-0x80000 is left alone for SDKs that patch kernel code there.
    constexpr uint32_t kKernelStackFloor = 0x00020000u;
    constexpr uint32_t kKernelStackTop = 0x00070000u;
    // If the kernel area runs out (or an executable loads into it), stacks come from the top of RAM
    // as before.
    uint32_t g_highCallbackStackTop = PS2_RAM_SIZE;
    uint32_t g_highCallbackStackFloor = kGuestHeapHardLimit;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    // Only the game thread dispatches guest code. Not thread_local: on mingw every thread_local
    // access is a function call (emulated TLS), and this runs on every guest dispatch.
    DispatchHistory g_dispatchHistory;

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }
}

// GPU renderer: the picture arrives already scanned out (and read back) at the size the GPU
// produced; the texture follows that size.
static void UploadGpuFrame(Texture2D &tex, uint32_t &outWidth, uint32_t &outHeight)
{
    static std::vector<uint8_t> s_frame;
    static uint32_t s_width = 0u, s_height = 0u, s_fbp = 0xFFFFFFFFu;
    ps2perf::Scope latchScope(ps2perf::MainLatch);
    uint32_t width = 0u, height = 0u, fbp = 0u;
    if (ps2gpu::takeFrame(s_frame, width, height, fbp) && width != 0u && height != 0u &&
        s_frame.size() >= static_cast<size_t>(width) * height * 4u)
    {
        s_fbp = fbp; // new pictures are counted at the game's VBlank (EeScheduler)
        ps2perf::Scope uploadScope(ps2perf::MainUpload);
        if (static_cast<uint32_t>(tex.width) != width || static_cast<uint32_t>(tex.height) != height)
        {
            UnloadTexture(tex);
            Image image{};
            image.data = s_frame.data();
            image.width = static_cast<int>(width);
            image.height = static_cast<int>(height);
            image.mipmaps = 1;
            image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            tex = LoadTextureFromImage(image);
            SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
        }
        else
        {
            UpdateTexture(tex, s_frame.data());
        }
        s_width = width;
        s_height = height;
    }
    outWidth = s_width != 0u ? s_width : FB_WIDTH;
    outHeight = s_height != 0u ? s_height : DEFAULT_DISPLAY_HEIGHT;
}

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    if (ps2gpu::active())
    {
        UploadGpuFrame(tex, outWidth, outHeight);
        return;
    }
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
    ps2perf::Scope latchScope(ps2perf::MainLatch);
    if (needsLatch)
    {
        // The game thread latches each frame at VBlank; PS2X_LATCH_ON_WINDOW_THREAD=1 latches here.
        static const bool latchHere = []()
        {
            const char *env = std::getenv("PS2X_LATCH_ON_WINDOW_THREAD");
            return env && *env && *env != '0';
        }();
        if (latchHere)
            rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp; // new pictures are counted at the game's VBlank (EeScheduler)
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    {
        ps2perf::Scope uploadScope(ps2perf::MainUpload);
        UpdateTexture(tex, s_uploadBuffer.data());
    }
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

// Chooses the GS renderer: PS2X_RENDERER=vulkan|software, else settings.ini (renderer=0 GPU,
// 1 software), else the GPU. A GPU that cannot start falls back to the software renderer.
static void startGsRenderer(GSRegisters &privRegs)
{
    const std::string iniPath = std::string(GetApplicationDirectory()) + "settings.ini";
    ps2settings::loadEarly(iniPath.c_str());
    bool wantGpu = ps2settings::get().renderer == 0;
    if (const char *env = std::getenv("PS2X_RENDERER"); env && *env)
    {
        const std::string choice(env);
        wantGpu = !(choice == "software" || choice == "cpu" || choice == "sw" || choice == "0");
    }
    ps2gpu::Description description;
    std::string error;
    bool started = false;
    if (wantGpu)
    {
        ps2gpu::Options options;
        options.superSampling = ps2settings::superSamplingFor(ps2settings::get().internalScale, 448); // Auto settles each frame
        if (const char *env = std::getenv("PS2X_GPU_SSAA"); env && *env)
            options.superSampling = static_cast<uint32_t>(std::clamp(std::atoi(env), 1, 16));
        started = ps2gpu::init(&privRegs, options, description, error);
        if (started)
        {
            ps2perf::RendererInfo info;
            info.name = "Vulkan GPU (paraLLEl-GS)";
            info.device = description.device;
            info.details = description.details;
            info.gpu = true;
            info.superSampling = options.superSampling;
            ps2perf::setRenderer(info);
            std::fprintf(stderr, "[gpu] GS renderer: Vulkan on %s (%s), %ux super-sampling\n", description.device.c_str(),
                         description.details.c_str(), options.superSampling);
            // VIF1 and VU1 microcode on their own CPU core (settings: vu1_thread=0 or PS2X_MTVU=0
            // keeps them on the game thread); the GS packets go on to a thread of their own.
            if (ps2settings::get().vu1Thread && ps2mtvu::start())
            {
                std::fprintf(stderr, "[mtvu] VU1 thread on\n");
                ps2perf::performanceNote("           VU1 thread: on - VIF1 and VU1 microcode run on their own CPU core, GS packets on a GS feed thread");
            }
            else
                ps2perf::performanceNote("           VU1 thread: off - VIF1 and VU1 run on the game thread");
        }
        else
        {
            std::fprintf(stderr, "[gpu] Vulkan GS renderer unavailable: %s - using the software renderer\n", error.c_str());
            ps2perf::graphicsNote(" Vulkan GPU renderer could not start: %s. Falling back to the software (CPU) renderer.", error.c_str());
        }
    }
    else
    {
        ps2perf::graphicsNote(" Software (CPU) renderer selected (settings.ini renderer=1 or PS2X_RENDERER).");
    }
    if (!started)
    {
        // Software renderer: the VU1 thread also feeds the CPU rasterizer.
        if (ps2settings::get().vu1Thread && ps2mtvu::start())
        {
            std::fprintf(stderr, "[mtvu] VU1 thread on\n");
            ps2perf::performanceNote("           VU1 thread: on - VIF1, VU1 microcode and the GS feed run on their own CPU core");
        }
        else
            ps2perf::performanceNote("           VU1 thread: off - VIF1 and VU1 run on the game thread");
    }
    ps2settings::setRendererStatus(started, ps2gpu::compiledIn(), started ? description.device.c_str() : "");
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);

    m_eeScheduler = std::make_unique<EeScheduler>(*this);

    // Assign rather than memset: R5900Context's constructor zeroes itself and
    // then applies the COP0 reset values, which a memset here would discard.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = kKernelStackFloor;
    m_asyncCallbackStackTop = kKernelStackTop;
    g_highCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    g_highCallbackStackTop = PS2_RAM_SIZE;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            CloseAudioDevice();
            m_audioBackend.setAudioReady(false);
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModule(std::string_view path, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModule(path, arguments, argumentSize);
}

ps2x::iop::ModuleLoadResult PS2Runtime::loadIopModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->loadModuleBuffer(guestAddress, arguments, argumentSize);
}

bool PS2Runtime::stopIopModule(int32_t moduleId, int32_t *result)
{
    auto scope = m_iopHost->enterCall(nullptr, m_memory.getRDRAM());
    return m_iopSubsystem->stopModule(moduleId, result);
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

bool PS2Runtime::canBindIopRpc(uint32_t sid) const noexcept
{
    return m_iopSubsystem->canBindRpc(sid);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::advanceIopEeCycles(uint64_t eeCycles) noexcept
{
    // The scheduler reports time in small slices and the IOP batches ~2048 EE cycles anyway;
    // batching here keeps the timing scope off the per-slice path (game thread only).
    static uint64_t s_pendingEeCycles = 0;
    s_pendingEeCycles += eeCycles;
    if (s_pendingEeCycles < 2048u)
        return;
    const uint64_t cycles = s_pendingEeCycles;
    s_pendingEeCycles = 0;
    ps2perf::Scope perfScope(ps2perf::Iop);
    m_iopSubsystem->runEeCycles(cycles);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

uint32_t PS2Runtime::allocateIopMemory(uint32_t size, uint32_t alignment)
{
    return m_iopSubsystem ? m_iopSubsystem->allocateMemory(size, alignment) : 0u;
}

bool PS2Runtime::freeIopMemory(uint32_t address)
{
    return m_iopSubsystem && m_iopSubsystem->freeMemory(address);
}

bool PS2Runtime::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->readMemory(address, destination, size);
}

bool PS2Runtime::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->writeMemory(address, source, size);
}

bool PS2Runtime::zeroIopMemory(uint32_t address, size_t size)
{
    return m_iopSubsystem && m_iopSubsystem->zeroMemory(address, size);
}

bool PS2Runtime::isIopMemoryRange(uint32_t address, size_t size) const
{
    return m_iopSubsystem && m_iopSubsystem->isMemoryRange(address, size);
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gifArbiter.setProcessPathPacketFn([this](GifPathId path, const uint8_t *data, uint32_t size)
                                        {
                                            if (ps2gpu::active())
                                                ps2gpu::gifTransfer(static_cast<uint32_t>(path), data, size);
                                            else
                                                m_gs.processGIFPacket(data, size, static_cast<uint32_t>(path));
                                        });
    m_memory.setGifArbiter(&m_gifArbiter);
    // GS downloads (sceGsExecStoreImage and friends) read from whichever GS draws.
    ps2SetGsDownloadSource([this](uint8_t *dst, uint32_t bytes) -> uint32_t
                           { return ps2gpu::active() ? ps2gpu::readLocalToHost(dst, bytes)
                                                     : m_gs.consumeLocalToHostBytes(dst, bytes); });
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     // On the VU1 thread the EE's context is in use elsewhere: D/T-bit stops
                                     // (a debugging feature) are left off there.
                                     R5900Context *cpuContext = nullptr;
                                     if (!ps2mtvu::onWorker())
                                     {
                                         cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                         if (!cpuContext)
                                             cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled = cpuContext && (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled = cpuContext && (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 65536);
                                     // VPU_STAT D/T stop bits; written only when they change (this can run
                                     // on the VU1 thread while the EE uses the context).
                                     const uint32_t stopBits = (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                                               (m_vu1.state().stoppedByT ? 0x0400u : 0u);
                                     if (cpuContext && (cpuContext->vu0_vpu_stat & 0x0600u) != stopBits)
                                         cpuContext->vu0_vpu_stat = (cpuContext->vu0_vpu_stat & ~0x0600u) | stopBits; });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     // On the VU1 thread the EE's context is in use elsewhere: D/T-bit stops
                                     // (a debugging feature) are left off there.
                                     R5900Context *cpuContext = nullptr;
                                     if (!ps2mtvu::onWorker())
                                     {
                                         cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                         if (!cpuContext)
                                             cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled = cpuContext && (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled = cpuContext && (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, 65536);
                                     const uint32_t stopBits = (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                                               (m_vu1.state().stoppedByT ? 0x0400u : 0u);
                                     if (cpuContext && (cpuContext->vu0_vpu_stat & 0x0600u) != stopBits)
                                         cpuContext->vu0_vpu_stat = (cpuContext->vu0_vpu_stat & ~0x0600u) | stopBits; });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if !defined(PLATFORM_VITA)
        startGsRenderer(m_memory.gs());
#endif
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
#if defined(_WIN32)
        // 1 ms system timer: the scheduler's timed waits and the frame limiter wake on time.
        timeBeginPeriod(1u);
#endif
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        SetTraceLogCallback(rayLogToGraphics);
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        describePresenter();
        if (!ps2gpu::active())
        {
            ps2perf::RendererInfo info;
            info.name = "Software (CPU)";
            info.device = "CPU - " + std::to_string(softwareRasterThreads()) + " raster threads";
            info.details = "the graphics card is only used to show the finished picture";
            info.gpu = false;
            ps2perf::setRenderer(info);
        }
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
        // Esc belongs to the game and the F2 settings drawer, not to closing the window.
        SetExitKey(KEY_NULL);
        // F2 settings drawer: loads settings.ini and applies window mode, V-Sync and volume.
        ps2settings::initialize((std::string(GetApplicationDirectory()) + "settings.ini").c_str());
        // SPU2 output: a 48 kHz stereo stream pulled from the IOP's sound processor.
        if (IsAudioDeviceReady() && !std::getenv("PS2X_NO_AUDIO"))
        {
            SetAudioStreamBufferSizeDefault(1024);
            static AudioStream spu2Stream = LoadAudioStream(48000, 16, 2);
            SetAudioStreamCallback(spu2Stream, [](void *buffer, unsigned int frames)
                                   { (void)ps2x::iop::pullAudioFrames(static_cast<int16_t *>(buffer), frames); });
            PlayAudioStream(spu2Stream);
        }
#endif
        SetTargetFPS(60);
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t minLoadedRdram = std::numeric_limits<uint32_t>::max();
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
            minLoadedRdram = std::min(minLoadedRdram, physAddr);
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        g_highCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        g_highCallbackStackTop = PS2_RAM_SIZE;
        const bool kernelAreaFree = minLoadedRdram >= kKernelStackTop;
        m_asyncCallbackStackFloor = kernelAreaFree ? kKernelStackFloor : 0u;
        m_asyncCallbackStackTop = kernelAreaFree ? kKernelStackTop : 0u;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string romError;
    if (!m_romDevice.configure(identity, &romError))
    {
        std::cerr << "[ROM0] failed to configure profile: " << romError << std::endl;
        return false;
    }

    m_iopSubsystem->reset();

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    // A memory card folder the host chose before loading (e.g. "saves" beside the executable)
    // stays: loading resets the IOP, which sets up both cards right away. Only the default one
    // (<program folder>/mc0) follows the program.
    const bool defaultMcRoot = paths.mcRoot.empty() ||
                               normalizeAbsolutePath(paths.mcRoot) == normalizeAbsolutePath(paths.elfDirectory / "mc0");
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        if (defaultMcRoot)
            paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void ps2xDumpBranchTrace(const char *why);

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    // PS2X_CRASH_DUMP=<path>: save EE RAM when the first bad branch happens (for post-mortem analysis).
    if (firstReport)
    {
        ps2xDumpBranchTrace("missing function");
    }
    if (firstReport && rdram)
    {
        if (const char *dumpPath = std::getenv("PS2X_CRASH_DUMP"); dumpPath && *dumpPath)
        {
            if (FILE *f = std::fopen(dumpPath, "wb"))
            {
                std::fwrite(rdram, 1, PS2_RAM_SIZE, f);
                std::fclose(f);
                std::fprintf(stderr, "[crash-dump] wrote %s\n", dumpPath);
            }
        }
    }

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

namespace
{
    // Last inter-function transfers, printed when a thread ends by reaching address 0.
    struct BranchTraceEntry
    {
        uint32_t source;
        uint32_t target;
        uint32_t ra;
        uint32_t kind;
    };
    constexpr uint32_t kBranchTraceSize = 48u;
    BranchTraceEntry g_branchTrace[kBranchTraceSize]{};
    uint32_t g_branchTracePos = 0u;
}

void ps2xRecordBranch(uint32_t source, uint32_t target, uint32_t ra, uint32_t kind)
{
    g_branchTrace[g_branchTracePos++ % kBranchTraceSize] = {source, target, ra, kind};
}

void ps2xDumpBranchTrace(const char *why)
{
    std::fprintf(stderr, "[branch-trace] %s (oldest first)\n", why);
    for (uint32_t i = 0; i < kBranchTraceSize; ++i)
    {
        const BranchTraceEntry &e = g_branchTrace[(g_branchTracePos + i) % kBranchTraceSize];
        if (e.source == 0u && e.target == 0u)
            continue;
        std::fprintf(stderr, "[branch-trace]   %08x -> %08x kind=%u ra=%08x\n", e.source, e.target, e.kind, e.ra);
    }
}

#if !defined(_WIN32)
#include <csignal>
static volatile std::sig_atomic_t g_texDumpRequested = 0;
bool ps2xTexDumpRequested()
{
    if (!g_texDumpRequested)
        return false;
    g_texDumpRequested = 0;
    return true;
}
namespace
{
    // kill -USR1 <pid> prints the recent guest call trail (bring-up aid on Linux).
    struct BranchTraceSignalInstaller
    {
        BranchTraceSignalInstaller()
        {
            std::signal(SIGUSR1, [](int) { ps2xDumpBranchTrace("SIGUSR1"); });
            std::signal(SIGUSR2, [](int) { g_texDumpRequested = 1; });
        }
    } g_branchTraceSignalInstaller;
}
#else
bool ps2xTexDumpRequested() { return false; }
#endif

// PS2X_TRACE_CALL_HIST=1: every 4 s print the 40 most-dispatched guest functions (call/jump targets).
static void ps2xCallHistogram(uint32_t target)
{
    static const bool enabled = []() { const char *e = std::getenv("PS2X_TRACE_CALL_HIST"); return e && *e && *e != '0'; }();
    if (!enabled)
        return;
    static std::unordered_map<uint32_t, uint64_t> counts;
    static auto last = std::chrono::steady_clock::now();
    ++counts[target];
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(4))
        return;
    last = now;
    std::vector<std::pair<uint32_t, uint64_t>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    std::fprintf(stderr, "[call-hist] %zu functions:", sorted.size());
    for (size_t i = 0; i < sorted.size() && i < 40; ++i)
        std::fprintf(stderr, " %x:%llu", sorted[i].first, (unsigned long long)sorted[i].second);
    std::fprintf(stderr, "\n");
    if (const char *watch = std::getenv("PS2X_TRACE_CALL_WATCH"))
    {
        std::fprintf(stderr, "[call-watch]");
        for (const char *p = watch; *p;)
        {
            char *end = nullptr;
            const uint32_t addr = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (end == p) break;
            std::fprintf(stderr, " %x:%llu", addr, (unsigned long long)(counts.count(addr) ? counts[addr] : 0ull));
            p = (*end == ',') ? end + 1 : end;
        }
        std::fprintf(stderr, "\n");
    }
    counts.clear();
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    g_branchTrace[g_branchTracePos++ % kBranchTraceSize] = {sourcePc, targetPc, getRegU32(ctx, 31), static_cast<uint32_t>(kind)};
    ps2xCallHistogram(targetPc);
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);

    // Report calls through bad function pointers (null or low addresses) with the registers that led there.
    if (isCall && targetPc < 0x00100000u)
    {
        static int reports = 0;
        if (reports < 8)
        {
            ++reports;
            auto word = [&](uint32_t addr) -> uint32_t {
                const uint32_t phys = addr & 0x1FFFFFFFu;
                if (phys + 4u > PS2_RAM_SIZE) return 0xDEADBEEFu;
                uint32_t v; std::memcpy(&v, rdram + phys, 4); return v;
            };
            std::fprintf(stderr, "[bad-call] src=%08x target=%08x ra=%08x sp=%08x a0=%08x a1=%08x a2=%08x a3=%08x v0=%08x v1=%08x\n",
                         sourcePc, targetPc, getRegU32(ctx, 31), getRegU32(ctx, 29), getRegU32(ctx, 4), getRegU32(ctx, 5),
                         getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 2), getRegU32(ctx, 3));
            std::fprintf(stderr, "[bad-call]   s0=%08x s1=%08x s2=%08x s3=%08x s4=%08x s5=%08x s6=%08x s7=%08x\n",
                         getRegU32(ctx, 16), getRegU32(ctx, 17), getRegU32(ctx, 18), getRegU32(ctx, 19),
                         getRegU32(ctx, 20), getRegU32(ctx, 21), getRegU32(ctx, 22), getRegU32(ctx, 23));
            const uint32_t a0 = getRegU32(ctx, 4);
            std::fprintf(stderr, "[bad-call]   [a0] %08x %08x %08x %08x %08x %08x %08x %08x\n", word(a0), word(a0 + 4), word(a0 + 8),
                         word(a0 + 12), word(a0 + 16), word(a0 + 20), word(a0 + 24), word(a0 + 28));
            const uint32_t s1 = getRegU32(ctx, 17);
            std::fprintf(stderr, "[bad-call]   [s1] %08x %08x %08x %08x %08x %08x %08x %08x\n", word(s1), word(s1 + 4), word(s1 + 8),
                         word(s1 + 12), word(s1 + 16), word(s1 + 20), word(s1 + 24), word(s1 + 28));
            ps2xDumpBranchTrace("bad call");
            if (const char *dumpPath = std::getenv("PS2X_CRASH_DUMP"); dumpPath && *dumpPath && reports == 1)
            {
                if (FILE *f = std::fopen(dumpPath, "wb")) { std::fwrite(rdram, 1, PS2_RAM_SIZE, f); std::fclose(f); }
            }
        }
    }

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        return false;
    }

    if (!isCall)
    {
        if (!hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            // if you need the app to keep open to open debug pannel change this to false
            return false;
        }

        return false;
    }

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    targetFn(rdram, ctx, this);
    if (ctx->pc != fallthroughPc)
    {
        // Unusual return: the callee left a pc other than the call's fall-through.
        g_branchTrace[g_branchTracePos++ % kBranchTraceSize] = {targetPc, ctx->pc, getRegU32(ctx, 31), 99u};
    }

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    // A callee that leaves pc at its own entry is an HLE stub that "returned" without touching pc.
    // The exception is an unwind for a scheduler checkpoint: a recursive call preempted at its own
    // call site also leaves pc == entry, and must resume there rather than fall through.
    if (ctx->pc == entryPc && !(m_eeScheduler && m_eeScheduler->unwindingForCheckpoint()))
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    // VU0 runs a microprogram to its E-bit before the EE continues. A fixed 4096-cycle budget cut
    // longer programs short and handed the EE half-finished results.
    constexpr uint32_t kVu0RunBudget = 4u * 1024u * 1024u;
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, kVu0RunBudget);
    {
        static const bool trace = []() { const char *e = std::getenv("PS2X_TRACE_VU0_RUNS"); return e && *e && *e != '0'; }();
        static uint32_t longRunsReported = 0;
        const uint64_t used = m_vu0.state().cycles;
        if ((trace || !m_vu0.lastRunEnded()) && longRunsReported < 16u && (used > 4096u || !m_vu0.lastRunEnded()))
        {
            ++longRunsReported;
            std::fprintf(stderr, "[vu0] microprogram at 0x%03x ran %llu cycles%s\n", startPC,
                         static_cast<unsigned long long>(used), m_vu0.lastRunEnded() ? "" : " and did not finish");
        }
    }
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    // PS2X_TRACE_SYSCALLS=<n> prints the first n EE syscalls with arguments and result.
    static const uint64_t traceLimit = []() -> uint64_t
    {
        const char *env = std::getenv("PS2X_TRACE_SYSCALLS");
        return (env && *env) ? std::strtoull(env, nullptr, 0) : 0u;
    }();
    static std::atomic<uint64_t> traceCount{0};
    const bool trace = traceLimit != 0u && traceCount.fetch_add(1) < traceLimit;
    const uint32_t tracePc = ctx->pc;
    const uint32_t traceArgs[4] = {getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7)};

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        if (trace)
        {
            std::fprintf(stderr, "[syscall] %d pc=%08x ra=%08x a0=%08x a1=%08x a2=%08x a3=%08x -> %08x\n",
                         static_cast<int32_t>(syscallId), tracePc, getRegU32(ctx, 31),
                         traceArgs[0], traceArgs[1], traceArgs[2], traceArgs[3], getRegU32(ctx, 2));
        }
        return;
    }
    if (trace)
    {
        std::fprintf(stderr, "[syscall] %d pc=%08x UNHANDLED\n", static_cast<int32_t>(syscallId), tracePc);
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (uint32_t cause : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, cause);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    const uint32_t address = allocateGuestBlockLocked(size, alignment);
    // PS2X_TRACE_GUEST_MALLOC=1 lists every runtime-side guest allocation.
    static const bool trace = []() { const char *e = std::getenv("PS2X_TRACE_GUEST_MALLOC"); return e && *e && *e != '0'; }();
    if (trace)
        std::fprintf(stderr, "[guest-malloc] addr=0x%08x size=0x%x heap=[0x%08x,0x%08x)\n", address, size, m_guestHeapBase, m_guestHeapLimit);
    return address;
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    // Carves a stack downwards from top, never below floor. Returns its initial stack pointer.
    auto carve = [&](uint32_t &top, uint32_t floor) -> uint32_t
    {
        uint32_t current = std::min(top, PS2_RAM_SIZE) & ~(kGuestHeapDefaultAlignment - 1u);
        if (current <= allocSize)
            return 0u;
        uint32_t base = (current - allocSize) & ~(normalizedAlignment - 1u);
        if (base < floor || base >= current)
            return 0u;
        top = base;
        return current - 0x10u;
    };

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    if (const uint32_t sp = carve(m_asyncCallbackStackTop, m_asyncCallbackStackFloor))
        return sp;
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        std::fprintf(stderr, "[ee] callback stacks: the kernel area is full, using the top of RAM\n");
    }
    return carve(g_highCallbackStackTop, g_highCallbackStackFloor);
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    // The native fast paths write straight into the software renderer; the GPU renderer takes
    // the regular DMA path so every packet reaches it.
    if (!ps2gpu::active() && !ps2mtvu::active() && m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (!ps2gpu::active() && !ps2mtvu::active() && m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    return m_eeScheduler->checkpointDue(cycles);
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    resetIop();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        try
        {
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    ps2perf::markMainThread();
    ps2perf::registerThread("window (present + input)");
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        PS2_IF_AGRESSIVE_LOGS({
            tick++;
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);

            }
        });
        // PS2X_DUMP_TEX="psm,tbp,tbw,w,h": on SIGUSR2 write that texture's raw texels to
        // /tmp/ps2x_tex.pgm (bring-up of texture upload paths).
        if (ps2xTexDumpRequested())
        {
            if (const char *spec = std::getenv("PS2X_DUMP_TEX"))
            {
                unsigned psm = 0, tbp = 0, tbw = 0, w = 0, h = 0;
                if (std::sscanf(spec, "%i,%i,%i,%i,%i", &psm, &tbp, &tbw, &w, &h) == 5 && w && h)
                {
                    ps2mtvu::sync();
                    if (FILE *f = std::fopen("/tmp/ps2x_tex.pgm", "wb"))
                    {
                        std::fprintf(f, "P5 %u %u 255\n", w, h);
                        for (unsigned y = 0; y < h; ++y)
                            for (unsigned x = 0; x < w; ++x)
                            {
                                const uint32_t v = m_gs.ReadVram(psm, tbp, tbw, x, y);
                                const uint8_t b = (psm == 0x14 || psm == 0x24 || psm == 0x2c) ? static_cast<uint8_t>(v * 17u) : static_cast<uint8_t>(v);
                                std::fputc(b, f);
                            }
                        std::fclose(f);
                        std::fprintf(stderr, "[tex-dump] wrote /tmp/ps2x_tex.pgm\n");
                    }
                }
            }
        }

        // PS2X_STATE_DUMP=<frames>: print the EE threads and semaphores every <frames> host frames.
        static const uint64_t stateDumpEvery = []() -> uint64_t
        {
            const char *env = std::getenv("PS2X_STATE_DUMP");
            return (env && *env) ? std::strtoull(env, nullptr, 0) : 0u;
        }();
        if (stateDumpEvery != 0u && (++tick % stateDumpEvery) == 0u)
        {
            const auto ee = m_eeScheduler->snapshot();
            std::fprintf(stderr, "[state] frame=%llu eeCycle=%llu running=%d dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                         static_cast<unsigned long long>(tick), static_cast<unsigned long long>(ee.eeCycle),
                         ee.runningThreadId, static_cast<unsigned long long>(m_memory.dmaStartCount()),
                         static_cast<unsigned long long>(m_memory.gifCopyCount()),
                         static_cast<unsigned long long>(m_memory.gsWriteCount()),
                         static_cast<unsigned long long>(m_memory.vifWriteCount()));
            {
                const GSRegisters &gsRegs = m_memory.gs();
                std::fprintf(stderr, "[state]   gs pmode=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx smode2=%llx\n",
                             (unsigned long long)gsRegs.pmode, (unsigned long long)gsRegs.dispfb1,
                             (unsigned long long)gsRegs.display1, (unsigned long long)gsRegs.dispfb2,
                             (unsigned long long)gsRegs.display2, (unsigned long long)gsRegs.smode2);
            }
            for (const auto &t : ee.threads)
            {
                std::fprintf(stderr, "[state]   thread %d status=%d wait=%d/%d prio=%d pc=%08x ra=%08x sp=%08x entry=%08x\n",
                             t.id, static_cast<int>(t.status), static_cast<int>(t.waitReason), t.waitId,
                             t.currentPriority, t.pc, t.ra, t.sp, t.entry);
            }
            for (const auto &sema : ee.semaphores)
            {
                std::fprintf(stderr, "[state]   sema %d count=%d max=%d waiters=%u\n",
                             sema.id, sema.count, sema.maxCount, sema.waiters);
            }
            for (const std::string &line : iopDebugSnapshot().diagnostics)
                if (line.rfind("iop-thread", 0) == 0)
                    std::fprintf(stderr, "[state]   %s\n", line.c_str());
            if (std::getenv("PS2X_IOP_RAM_DUMP"))
            {
                std::vector<uint8_t> iopRam(0x200000u);
                if (readIopMemory(0u, iopRam.data(), iopRam.size()))
                {
                    if (FILE *f = std::fopen("/tmp/ps2x_iop_ram.bin", "wb"))
                    {
                        std::fwrite(iopRam.data(), 1, iopRam.size(), f);
                        std::fclose(f);
                    }
                }
            }
        }
        // PS2X_RAM_DUMP=<frame>: write EE RAM to /tmp/ps2x_rdram_<frame>.bin at that host frame.
        {
            static const uint64_t ramDumpAt = []() -> uint64_t
            {
                const char *env = std::getenv("PS2X_RAM_DUMP");
                return (env && *env) ? std::strtoull(env, nullptr, 0) : 0u;
            }();
            static uint64_t ramDumpFrame = 0u;
            if (ramDumpAt != 0u && ++ramDumpFrame == ramDumpAt)
            {
                char path[64];
                std::snprintf(path, sizeof(path), "/tmp/ps2x_rdram_%llu.bin", static_cast<unsigned long long>(ramDumpAt));
                if (FILE *f = std::fopen(path, "wb"))
                {
                    std::fwrite(m_memory.getRDRAM(), 1, PS2_RAM_SIZE, f);
                    std::fclose(f);
                    std::fprintf(stderr, "[ram] dumped %s\n", path);
                }
            }
        }
        // PS2X_RAM_DUMP_TIMES=<s1,s2,...>: write EE RAM to /tmp/ps2x_rdram_t<s>.bin at those seconds after start.
        {
            static std::vector<double> dumpTimes = []()
            {
                std::vector<double> times;
                if (const char *env = std::getenv("PS2X_RAM_DUMP_TIMES"))
                {
                    for (const char *p = env; *p;)
                    {
                        char *end = nullptr;
                        const double t = std::strtod(p, &end);
                        if (end == p) break;
                        times.push_back(t);
                        p = (*end == ',') ? end + 1 : end;
                    }
                }
                std::sort(times.begin(), times.end());
                return times;
            }();
            static const auto dumpEpoch = std::chrono::steady_clock::now();
            if (!dumpTimes.empty())
            {
                const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - dumpEpoch).count();
                if (now >= dumpTimes.front())
                {
                    char path[80];
                    std::snprintf(path, sizeof(path), "/tmp/ps2x_rdram_t%.0f.bin", dumpTimes.front());
                    dumpTimes.erase(dumpTimes.begin());
                    if (FILE *f = std::fopen(path, "wb"))
                    {
                        std::fwrite(m_memory.getRDRAM(), 1, PS2_RAM_SIZE, f);
                        std::fclose(f);
                        std::fprintf(stderr, "[ram] dumped %s\n", path);
                    }
                }
            }
        }
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        UploadFrame(frameTex, this, presentWidth, presentHeight);
        // F2 drawer: toggling, menu input, window mode / V-Sync / scaling filter / volume.
        ps2settings::update(&frameTex);

        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        // Borderless with a chosen resolution: the picture is drawn at that size first, then
        // scaled to the screen (F2 > Video > Resolution).
        int targetWidth = 0, targetHeight = 0;
        ps2settings::pictureTargetSize(GetScreenWidth(), GetScreenHeight(), targetWidth, targetHeight);
        static RenderTexture2D pictureTarget{};
        const bool useTarget = targetWidth > 0 && targetHeight > 0 && (targetWidth != GetScreenWidth() || targetHeight != GetScreenHeight());
        if (useTarget)
        {
            if (pictureTarget.id == 0 || pictureTarget.texture.width != targetWidth || pictureTarget.texture.height != targetHeight)
            {
                if (pictureTarget.id != 0)
                    UnloadRenderTexture(pictureTarget);
                pictureTarget = LoadRenderTexture(targetWidth, targetHeight);
                SetTextureFilter(pictureTarget.texture, TEXTURE_FILTER_BILINEAR);
            }
            BeginTextureMode(pictureTarget);
            ClearBackground(BLACK);
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
            ps2settings::presentRect(srcWidth, srcHeight, static_cast<float>(targetWidth), static_cast<float>(targetHeight), x, y, w, h);
            DrawTexturePro(frameTex, srcRect, Rectangle{x, y, w, h}, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            EndTextureMode();
        }
        else if (pictureTarget.id != 0)
        {
            UnloadRenderTexture(pictureTarget);
            pictureTarget = RenderTexture2D{};
        }

        BeginDrawing();
        ClearBackground(BLACK);
        if (useTarget)
        {
            // Keep the chosen resolution's shape on the screen.
            const float scale = std::min(screenWidth / static_cast<float>(targetWidth), screenHeight / static_cast<float>(targetHeight));
            const float w = std::floor(static_cast<float>(targetWidth) * scale);
            const float h = std::floor(static_cast<float>(targetHeight) * scale);
            const Rectangle from{0.0f, 0.0f, static_cast<float>(targetWidth), -static_cast<float>(targetHeight)}; // render targets are upside down
            DrawTexturePro(pictureTarget.texture, from, Rectangle{std::floor((screenWidth - w) * 0.5f), std::floor((screenHeight - h) * 0.5f), w, h},
                           Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        }
        else
        {
            float dstX = 0.0f, dstY = 0.0f, dstWidth = 0.0f, dstHeight = 0.0f;
            ps2settings::presentRect(srcWidth, srcHeight, screenWidth, screenHeight, dstX, dstY, dstWidth, dstHeight);
            const Rectangle dstRect{dstX, dstY, dstWidth, dstHeight};
            DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        }
        ps2settings::draw(ps2perf::lastSpeedPercent());
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        {
            static const bool recordEnv = []() { const char *e = std::getenv("PS2X_RECORD_FRAMES"); return e && *e && *e != '0'; }();
            ps2capture::afterDraw(recordEnv || ps2settings::get().recordFrames);
        }
        {
            ps2perf::Scope endScope(ps2perf::MainEndDrawing);
            EndDrawing();
        }
        ps2perf::setOutputSize(presentWidth, presentHeight, static_cast<uint32_t>(GetScreenWidth()), static_cast<uint32_t>(GetScreenHeight()));
        ps2perf::mainThreadTick(m_eeScheduler->eeCycleForDiagnostics(), m_memory.getRDRAM());

        // PS2X_SHOT_TIMES=<s1,s2,...>: save the presented frame as shot_<s>.png (in PS2X_SHOT_DIR,
        // default the working directory) at those seconds after start.
        // PS2X_EXIT_AFTER=<s>: close the game after that many seconds. Both are for unattended
        // diagnostic runs.
        {
            static const auto loopEpoch = std::chrono::steady_clock::now();
            static std::vector<double> shotTimes = []()
            {
                std::vector<double> times;
                if (const char *env = std::getenv("PS2X_SHOT_TIMES"))
                {
                    for (const char *p = env; *p;)
                    {
                        char *end = nullptr;
                        const double t = std::strtod(p, &end);
                        if (end == p)
                            break;
                        times.push_back(t);
                        p = (*end == ',') ? end + 1 : end;
                    }
                }
                std::sort(times.begin(), times.end());
                return times;
            }();
            static const double exitAfter = []()
            {
                const char *env = std::getenv("PS2X_EXIT_AFTER");
                return (env && *env) ? std::atof(env) : 0.0;
            }();
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - loopEpoch).count();
            if (!shotTimes.empty() && elapsed >= shotTimes.front())
            {
                const double when = shotTimes.front();
                shotTimes.erase(shotTimes.begin());
                Image shot = LoadImageFromTexture(frameTex);
                if (shot.data != nullptr)
                {
                    ImageCrop(&shot, Rectangle{0.0f, 0.0f, static_cast<float>(std::min<uint32_t>(presentWidth, FB_WIDTH)),
                                               static_cast<float>(std::min<uint32_t>(presentHeight, FB_HEIGHT))});
                    ImageFormat(&shot, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
                    const char *dir = std::getenv("PS2X_SHOT_DIR");
                    char path[512];
                    std::snprintf(path, sizeof(path), "%s%sshot_%.0f.png", (dir && *dir) ? dir : "", (dir && *dir) ? "/" : "", when);
                    ExportImage(shot, path);
                    UnloadImage(shot);
                    std::fprintf(stderr, "[shot] %s\n", path);
                }
            }
            if (exitAfter > 0.0 && elapsed >= exitAfter)
            {
                std::fprintf(stderr, "[run] PS2X_EXIT_AFTER reached (%.0f s)\n", exitAfter);
                requestStop();
                break;
            }
        }

        if (WindowShouldClose() || ps2settings::quitRequested())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    ps2perf::heartbeat(ps2perf::Beat::Closing);
    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }
    ps2mtvu::stop(); // finishes what the game thread handed over
    ps2gpu::shutdown();

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    ps2capture::shutdown();
    ps2perf::shutdown();
    UnloadTexture(frameTex);
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");
}
