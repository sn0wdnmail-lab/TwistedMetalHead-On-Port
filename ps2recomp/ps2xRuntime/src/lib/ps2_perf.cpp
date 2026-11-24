#include "runtime/ps2_perf.h"
#include "runtime/ps2_mtvu.h"
#include "runtime/ps2_present.h"
#include "ps2x/iop/iop_audio.h"

namespace ps2settings
{
    // F2 drawer open with "Pause in this menu" (ps2_settings.cpp; its header pulls in raylib,
    // whose names clash with <windows.h> used here).
    bool emulationPaused() noexcept;
}

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace ps2perf
{
    namespace
    {
        std::atomic<uint64_t> g_buckets[BucketCount];
        std::atomic<uint64_t> g_counters[CounterCount];
        std::atomic<uint64_t> g_gauges[GaugeCount];
        thread_local uint64_t t_childNs = 0;
        thread_local bool t_mainThread = false;
        thread_local bool t_vuThread = false; // the VU1 thread (ps2_mtvu)
        thread_local bool t_feedThread = false; // the GS feed thread (gs_gpu_renderer)
        // The GS feed thread's time, kept apart from the game thread's: packet parse, frame
        // hand-off to the GPU, anything else.
        std::atomic<uint64_t> g_feedNs[3];
        std::atomic<bool> g_feedSeen{false};
        std::atomic<int64_t> g_lastSpeedTenths{-1}; // last reported speed, in tenths of a percent

        Bucket vuThreadBucket(Bucket bucket)
        {
            switch (bucket)
            {
            case Vu1: return VuThreadVu1;
            case Vif1: return VuThreadVif1;
            case GsPacket: return VuThreadGs;
            case GpuFrame:
            case GpuSync: return VuThreadGpu;
            case IdleWait: return VuThreadIdle;
            default: return VuThreadOther;
            }
        }

        const auto g_epoch = std::chrono::steady_clock::now();

        constexpr uint64_t kReportIntervalNs = 2000000000ull;
        constexpr double kEeHz = 294912000.0;

        // ---------------------------------------------------------------- settings
        int perfMode()
        {
            // 0 = off, 1 = files (default), 2 = files + stderr
            static const int mode = []()
            {
                const char *env = std::getenv("PS2X_PERF");
                if (!env || !*env)
                    return 1;
                if (*env == '0')
                    return 0;
                return 2;
            }();
            return mode;
        }

        uint32_t tickAddress()
        {
            static const uint32_t address = []() -> uint32_t
            {
                const char *env = std::getenv("PS2X_PERF_TICK_ADDR");
                if (env && *env)
                    return static_cast<uint32_t>(std::strtoul(env, nullptr, 0));
                return 0x2f4548u; // Head-On's game logic tick counter
            }();
            return address;
        }

        // ---------------------------------------------------------------- log files
        std::mutex g_fileMutex;
        bool g_filesTried = false;
        FILE *g_perfFile = nullptr;
        FILE *g_gfxFile = nullptr;
        std::string g_logDir;
        std::string g_exePath;

#if defined(_WIN32)
        std::wstring widen(const std::string &text)
        {
            if (text.empty())
                return std::wstring();
            const int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring out(static_cast<size_t>(count), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), count);
            return out;
        }

        std::string narrow(const std::wstring &text)
        {
            if (text.empty())
                return std::string();
            const int count = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<size_t>(count), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), count, nullptr, nullptr);
            return out;
        }
#endif

        std::string parentOf(const std::string &path)
        {
            const size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
        }

        std::string leafOf(const std::string &path)
        {
            const size_t slash = path.find_last_of("/\\");
            return slash == std::string::npos ? path : path.substr(slash + 1);
        }

        bool iequals(const std::string &a, const char *b)
        {
            const size_t n = std::strlen(b);
            if (a.size() != n)
                return false;
            for (size_t i = 0; i < n; ++i)
            {
                char x = a[i], y = b[i];
                if (x >= 'A' && x <= 'Z')
                    x = static_cast<char>(x - 'A' + 'a');
                if (y >= 'A' && y <= 'Z')
                    y = static_cast<char>(y - 'A' + 'a');
                if (x != y)
                    return false;
            }
            return true;
        }

        std::string executablePath()
        {
#if defined(_WIN32)
            std::wstring buffer(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            buffer.resize(length);
            return narrow(buffer);
#else
            char buffer[4096];
            const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
            if (length <= 0)
                return std::string("./ps2EntryRunner");
            buffer[length] = '\0';
            return std::string(buffer);
#endif
        }

        void makeDirectory(const std::string &path)
        {
#if defined(_WIN32)
            CreateDirectoryW(widen(path).c_str(), nullptr);
#else
            mkdir(path.c_str(), 0755);
#endif
        }

        void moveFile(const std::string &from, const std::string &to)
        {
#if defined(_WIN32)
            MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING);
#else
            std::rename(from.c_str(), to.c_str());
#endif
        }

        FILE *openFile(const std::string &path)
        {
#if defined(_WIN32)
            return _wfopen(widen(path).c_str(), L"wb");
#else
            return std::fopen(path.c_str(), "wb");
#endif
        }

        std::string localTimeText()
        {
            const std::time_t now = std::time(nullptr);
            std::tm parts{};
#if defined(_WIN32)
            localtime_s(&parts, &now);
#else
            localtime_r(&now, &parts);
#endif
            char text[64];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &parts);
            return text;
        }

        // ---------------------------------------------------------------- system description
        std::string cpuName()
        {
#if defined(__x86_64__) || defined(__i386__)
            unsigned int regs[12] = {};
            unsigned int maxLeaf = __get_cpuid_max(0x80000000u, nullptr);
            if (maxLeaf >= 0x80000004u)
            {
                __get_cpuid(0x80000002u, &regs[0], &regs[1], &regs[2], &regs[3]);
                __get_cpuid(0x80000003u, &regs[4], &regs[5], &regs[6], &regs[7]);
                __get_cpuid(0x80000004u, &regs[8], &regs[9], &regs[10], &regs[11]);
                char brand[49] = {};
                std::memcpy(brand, regs, 48);
                std::string text(brand);
                const size_t first = text.find_first_not_of(' ');
                const size_t last = text.find_last_not_of(' ');
                if (first != std::string::npos)
                    return text.substr(first, last - first + 1);
            }
#endif
            return std::string("unknown CPU");
        }

        double memoryGb()
        {
#if defined(_WIN32)
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof(status);
            if (GlobalMemoryStatusEx(&status))
                return static_cast<double>(status.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
            return 0.0;
#else
            const long pages = sysconf(_SC_PHYS_PAGES);
            const long pageSize = sysconf(_SC_PAGE_SIZE);
            return (pages > 0 && pageSize > 0) ? static_cast<double>(pages) * static_cast<double>(pageSize) / (1024.0 * 1024.0 * 1024.0) : 0.0;
#endif
        }

        std::string osName()
        {
#if defined(_WIN32)
            using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
            if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
            {
                auto fn = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void *>(GetProcAddress(ntdll, "RtlGetVersion")));
                OSVERSIONINFOW info{};
                info.dwOSVersionInfoSize = sizeof(info);
                if (fn && fn(&info) == 0)
                {
                    char text[96];
                    std::snprintf(text, sizeof(text), "Windows %s (version %lu.%lu, build %lu)",
                                  info.dwBuildNumber >= 22000u ? "11" : "10",
                                  static_cast<unsigned long>(info.dwMajorVersion), static_cast<unsigned long>(info.dwMinorVersion),
                                  static_cast<unsigned long>(info.dwBuildNumber));
                    return text;
                }
            }
            return std::string("Windows");
#else
            utsname name{};
            if (uname(&name) == 0)
                return std::string(name.sysname) + " " + name.release + " (" + name.machine + ")";
            return std::string("unknown OS");
#endif
        }

        // ---------------------------------------------------------------- CPU time per thread
        struct ThreadEntry
        {
            std::string name;
#if defined(_WIN32)
            HANDLE handle = nullptr;
#else
            clockid_t clock{};
#endif
            uint64_t lastNs = 0;
            uint64_t intervalNs = 0;
        };

        std::mutex g_threadMutex;
        std::vector<ThreadEntry> g_threads;

        uint64_t threadCpuNs(const ThreadEntry &entry)
        {
#if defined(_WIN32)
            FILETIME created, exited, kernel, user;
            if (!GetThreadTimes(entry.handle, &created, &exited, &kernel, &user))
                return entry.lastNs;
            const uint64_t k = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
            const uint64_t u = (static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
            return (k + u) * 100ull;
#else
            timespec ts{};
            if (clock_gettime(entry.clock, &ts) != 0)
                return entry.lastNs;
            return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
        }

        uint64_t processCpuNs()
        {
#if defined(_WIN32)
            FILETIME created, exited, kernel, user;
            if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
                return 0;
            const uint64_t k = (static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
            const uint64_t u = (static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
            return (k + u) * 100ull;
#else
            timespec ts{};
            if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
                return 0;
            return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
        }

        // ---------------------------------------------------------------- every thread of the process
        // Finds which threads use the CPU, including ones the game did not create itself (graphics
        // driver, audio, shader compilers): name (thread description) or the module it started in.
        struct CensusEntry
        {
            uint64_t lastNs = 0;
            uint64_t seenTick = 0;
            std::string label;
        };
        std::unordered_map<uint64_t, CensusEntry> g_census;
        std::atomic<unsigned long> g_otherThreadId{0}; // the busiest thread that is not one of ours (sampled too; a Windows thread id)
        uint64_t g_censusTick = 0;

#if defined(_WIN32)
        std::string threadLabel(DWORD tid)
        {
            std::string label;
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (!h)
                h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (!h)
                return "thread " + std::to_string(tid);
            using GetDescFn = HRESULT(WINAPI *)(HANDLE, PWSTR *);
            static const GetDescFn getDesc = []() -> GetDescFn
            {
                HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
                return kernel ? reinterpret_cast<GetDescFn>(reinterpret_cast<void *>(GetProcAddress(kernel, "GetThreadDescription"))) : nullptr;
            }();
            if (getDesc)
            {
                PWSTR desc = nullptr;
                if (SUCCEEDED(getDesc(h, &desc)) && desc)
                {
                    label = narrow(desc);
                    LocalFree(desc);
                }
            }
            if (label.empty())
            {
                // Start address -> module ("nvoglv64.dll", "TMHO.exe", ...).
                using QueryFn = LONG(WINAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
                static const QueryFn query = []() -> QueryFn
                {
                    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
                    return ntdll ? reinterpret_cast<QueryFn>(reinterpret_cast<void *>(GetProcAddress(ntdll, "NtQueryInformationThread"))) : nullptr;
                }();
                void *start = nullptr;
                if (query && query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr) == 0 && start)
                {
                    HMODULE module = nullptr;
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           static_cast<LPCWSTR>(start), &module) && module)
                    {
                        wchar_t path[MAX_PATH] = {};
                        GetModuleFileNameW(module, path, MAX_PATH);
                        label = leafOf(narrow(path)) + " thread";
                    }
                }
            }
            CloseHandle(h);
            if (label.empty())
                label = "thread " + std::to_string(tid);
            return label;
        }

        // Returns "label=cores" for the busiest threads over the interval.
        std::string busiestThreads(uint64_t intervalNs)
        {
            ++g_censusTick;
            const DWORD pid = GetCurrentProcessId();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return std::string();
            struct Busy
            {
                double cores;
                std::string label;
            };
            std::vector<Busy> busy;
            unsigned threadCount = 0;
            double otherCores = 0.0;
            DWORD otherId = 0;
            THREADENTRY32 entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
            {
                if (entry.th32OwnerProcessID != pid)
                    continue;
                ++threadCount;
                HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                if (!h)
                    continue;
                FILETIME created, exited, kernel, user;
                uint64_t cpu = 0;
                if (GetThreadTimes(h, &created, &exited, &kernel, &user))
                    cpu = (((static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                           ((static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime)) * 100ull;
                CloseHandle(h);
                auto it = g_census.find(entry.th32ThreadID);
                if (it == g_census.end())
                {
                    CensusEntry fresh;
                    fresh.lastNs = 0; // CPU since the thread started counts toward its first interval
                    fresh.label = threadLabel(entry.th32ThreadID);
                    it = g_census.emplace(entry.th32ThreadID, std::move(fresh)).first;
                }
                const uint64_t delta = cpu > it->second.lastNs ? cpu - it->second.lastNs : 0u;
                it->second.lastNs = cpu;
                it->second.seenTick = g_censusTick;
                const double cores = static_cast<double>(delta) / static_cast<double>(intervalNs);
                if (cores >= 0.05)
                    busy.push_back({cores, it->second.label});
                {
                    const std::string &label = it->second.label;
                    const bool ours = label == "GameThread" || label.rfind("VU1 thread", 0) == 0 || label.rfind("GS feed", 0) == 0 ||
                                      label.rfind("window", 0) == 0 || label == "perf sampler" || label == "perf report";
                    if (!ours && cores >= 0.03 && cores > otherCores)
                    {
                        otherCores = cores;
                        otherId = entry.th32ThreadID;
                    }
                }
            }
            CloseHandle(snapshot);
            g_otherThreadId.store(otherId, std::memory_order_relaxed);
            for (auto it = g_census.begin(); it != g_census.end();)
                it = (it->second.seenTick != g_censusTick) ? g_census.erase(it) : std::next(it);

            // Same-named threads (8 shader compilers, raster workers) are summed into one entry.
            std::vector<Busy> merged;
            for (const Busy &b : busy)
            {
                auto found = std::find_if(merged.begin(), merged.end(), [&](const Busy &m)
                                          { return m.label == b.label || m.label.rfind(b.label + " x", 0) == 0; });
                if (found == merged.end())
                    merged.push_back(b);
                else
                    found->cores += b.cores;
            }
            std::sort(merged.begin(), merged.end(), [](const Busy &a, const Busy &b)
                      { return a.cores > b.cores; });
            std::string text = std::to_string(threadCount) + " threads; busiest:";
            for (size_t i = 0; i < merged.size() && i < 6u; ++i)
            {
                char part[160];
                std::snprintf(part, sizeof(part), " %s %.2f%s", merged[i].label.c_str(), merged[i].cores, i + 1 < merged.size() && i < 5u ? "," : "");
                text += part;
            }
            if (merged.empty())
                text += " none above 0.05 cores";
            return text;
        }
#else
        std::string busiestThreads(uint64_t intervalNs)
        {
            ++g_censusTick;
            DIR *dir = opendir("/proc/self/task");
            if (!dir)
                return std::string();
            struct Busy
            {
                double cores;
                std::string label;
            };
            std::vector<Busy> busy;
            unsigned threadCount = 0;
            const double tickNs = 1e9 / static_cast<double>(sysconf(_SC_CLK_TCK));
            while (dirent *d = readdir(dir))
            {
                if (d->d_name[0] < '0' || d->d_name[0] > '9')
                    continue;
                ++threadCount;
                const uint64_t tid = std::strtoull(d->d_name, nullptr, 10);
                char path[96];
                std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", d->d_name);
                FILE *f = std::fopen(path, "r");
                if (!f)
                    continue;
                char buffer[1024] = {};
                const size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, f);
                std::fclose(f);
                buffer[got] = '\0';
                const char *open = std::strchr(buffer, '(');
                const char *close = std::strrchr(buffer, ')');
                if (!open || !close)
                    continue;
                std::string comm(open + 1, close);
                unsigned long long utime = 0, stime = 0;
                // fields after ")": state(3) ... utime(14) stime(15)
                const char *p = close + 2;
                int field = 3;
                while (*p && field < 14)
                {
                    if (*p == ' ')
                        ++field;
                    ++p;
                }
                std::sscanf(p, "%llu %llu", &utime, &stime);
                const uint64_t cpu = static_cast<uint64_t>(static_cast<double>(utime + stime) * tickNs);
                auto it = g_census.find(tid);
                if (it == g_census.end())
                {
                    CensusEntry fresh;
                    fresh.label = comm;
                    it = g_census.emplace(tid, std::move(fresh)).first;
                }
                const uint64_t delta = cpu > it->second.lastNs ? cpu - it->second.lastNs : 0u;
                it->second.lastNs = cpu;
                it->second.seenTick = g_censusTick;
                const double cores = static_cast<double>(delta) / static_cast<double>(intervalNs);
                if (cores >= 0.05)
                    busy.push_back({cores, it->second.label});
            }
            closedir(dir);
            for (auto it = g_census.begin(); it != g_census.end();)
                it = (it->second.seenTick != g_censusTick) ? g_census.erase(it) : std::next(it);
            std::vector<Busy> merged;
            for (const Busy &b : busy)
            {
                auto found = std::find_if(merged.begin(), merged.end(), [&](const Busy &m)
                                          { return m.label == b.label; });
                if (found == merged.end())
                    merged.push_back(b);
                else
                    found->cores += b.cores;
            }
            std::sort(merged.begin(), merged.end(), [](const Busy &a, const Busy &b)
                      { return a.cores > b.cores; });
            std::string text = std::to_string(threadCount) + " threads; busiest:";
            for (size_t i = 0; i < merged.size() && i < 6u; ++i)
            {
                char part[160];
                std::snprintf(part, sizeof(part), " %s %.2f%s", merged[i].label.c_str(), merged[i].cores, i + 1 < merged.size() && i < 5u ? "," : "");
                text += part;
            }
            if (merged.empty())
                text += " none above 0.05 cores";
            return text;
        }
#endif

        void writeLine(FILE *file, const char *text); // defined with the report code below

        // ---------------------------------------------------------------- sampling profiler
        // Every 2 ms the game thread is interrupted and its instruction pointer is recorded; every
        // 10 s the busiest functions are written to performancelogs.txt. Names come from the
        // symbol map next to the executable (<exe name>.sym: "rva name" lines, written at build
        // time); without it, addresses are shown. PS2X_PROFILE=0 turns it off.
        struct Symbol
        {
            uint64_t rva;
            std::string name;
        };
        std::vector<Symbol> g_symbols;
        void loadSymbolsOnce();
        std::mutex g_profileMutex;
        // Samples per sampled thread: 0 game thread, 1 VU1 thread, 2 GS feed thread (the last two on
        // Windows): label -> samples.
        constexpr int kSampledThreads = 4;
        const char *const kSampledNames[kSampledThreads] = {"game thread", "VU1 thread", "GS feed thread", "busiest other thread"};
        std::unordered_map<std::string, uint64_t> g_profileCounts[kSampledThreads];
        uint64_t g_profileSamples[kSampledThreads] = {};
        std::atomic<bool> g_samplerRunning{false};
        uint64_t g_imageBase = 0;
        uint64_t g_imageSize = 0;

        bool profilerEnabled()
        {
            const char *env = std::getenv("PS2X_PROFILE");
            return !(env && *env == '0');
        }

        void loadSymbols()
        {
            static std::once_flag once;
            std::call_once(once, []() { loadSymbolsOnce(); });
        }

        void loadSymbolsOnce()
        {
            std::string path = executablePath();
            const size_t dot = path.find_last_of('.');
            const size_t slash = path.find_last_of("/\\");
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
                path = path.substr(0, dot);
            path += ".sym";
#if defined(_WIN32)
            FILE *f = _wfopen(widen(path).c_str(), L"rb");
#else
            FILE *f = std::fopen(path.c_str(), "rb");
#endif
            if (!f)
                return;
            char line[4096];
            while (std::fgets(line, sizeof(line), f))
            {
                char *end = nullptr;
                const uint64_t rva = std::strtoull(line, &end, 16);
                if (!end || *end != ' ')
                    continue;
                std::string name(end + 1);
                while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
                    name.pop_back();
                g_symbols.push_back({rva, std::move(name)});
            }
            std::fclose(f);
            std::sort(g_symbols.begin(), g_symbols.end(), [](const Symbol &a, const Symbol &b) { return a.rva < b.rva; });
        }

        std::string labelForRva(uint64_t rva)
        {
            auto it = std::upper_bound(g_symbols.begin(), g_symbols.end(), rva, [](uint64_t v, const Symbol &s) { return v < s.rva; });
            if (it == g_symbols.begin())
            {
                char text[48];
                std::snprintf(text, sizeof(text), "exe+0x%llx", static_cast<unsigned long long>(rva));
                return text;
            }
            --it;
            std::string name = it->name;
            // Group the generated code: compiled VU1 blocks, recompiled game functions stay named.
            if (name.rfind("vu1aot_", 0) == 0)
                return "compiled VU microprograms";
            // Drop the parameter list (the last balanced "(...)", plus a trailing " const"), keeping
            // template arguments such as <(vufast::UpKind)3>.
            if (name.size() > 6 && name.compare(name.size() - 6, 6, " const") == 0)
                name.resize(name.size() - 6);
            if (!name.empty() && name.back() == ')')
            {
                int depth = 0;
                for (size_t i = name.size(); i-- > 0;)
                {
                    if (name[i] == ')')
                        ++depth;
                    else if (name[i] == '(' && --depth == 0)
                    {
                        name.resize(i);
                        break;
                    }
                }
            }
            if (name.rfind("void ", 0) == 0)
                name.erase(0, 5);
            return name;
        }

        // Broad groups for the profile summary line.
        const char *categoryOf(const std::string &label)
        {
            if (label == "compiled VU microprograms")
                return "compiled VU code";
            if (label.find("VU1Interpreter") != std::string::npos || label.find("VU0") != std::string::npos ||
                label.find("Vu1Aot") != std::string::npos || label.find("vufast") != std::string::npos || label.find("Vu0") != std::string::npos)
                return "VU interpreter";
            if (label.find("Vif") != std::string::npos || label.find("VIF") != std::string::npos || label.find("vif") != std::string::npos)
                return "VIF";
            if (label.find("GS") != std::string::npos || label.find("Gs") != std::string::npos || label.find("ps2gpu") != std::string::npos ||
                label.find("Vulkan") != std::string::npos || label.find("Granite") != std::string::npos || label.find("ParallelGS") != std::string::npos ||
                label.find("Gif") != std::string::npos || label.find("GIF") != std::string::npos)
                return "GS/GIF";
            if (label.find("Iop") != std::string::npos || label.find("IOP") != std::string::npos || label.find("iop") != std::string::npos ||
                label.find("Spu") != std::string::npos || label.find("SPU") != std::string::npos)
                return "IOP/sound";
            if (!label.empty() && label[0] == '[')
                return "system libraries";
            const size_t mark = label.rfind("_0x");
            if (mark != std::string::npos && mark + 3 < label.size() &&
                label.find_first_not_of("0123456789abcdef", mark + 3) == std::string::npos)
                return "game code (recompiled EE)";
            if (label.find("Memory") != std::string::npos || label.find("Ps2Fast") != std::string::npos || label.find("mem") == 0)
                return "memory access";
            return "other runtime";
        }

        void recordSample(const std::string &label, int thread = 0)
        {
            std::lock_guard<std::mutex> lock(g_profileMutex);
            ++g_profileCounts[thread][label];
            ++g_profileSamples[thread];
        }

#if defined(_WIN32)
        HANDLE g_sampledThread = nullptr;
        HANDLE vuThreadHandle(); // the VU1 thread, once it has registered (below, with the watchdog)
        HANDLE g_feedThreadHandle = nullptr; // the GS feed thread, once it has registered

        // Samples outside TMHO.exe: "[module] export" when the address sits right after an exported
        // function's start (the syscall stubs in ntdll.dll show what a waiting thread waits on),
        // otherwise just "[module]".
        struct ModuleSymbols
        {
            std::string label;
            std::vector<std::pair<uint32_t, std::string>> exports; // rva, name (sorted)
        };

        std::string moduleLabel(uint64_t address)
        {
            static std::mutex cacheMutex;
            std::lock_guard<std::mutex> cacheLock(cacheMutex);
            static std::unordered_map<uint64_t, ModuleSymbols> cache;
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(address), &module) || !module)
                return "[unknown code]";
            const uint64_t base = reinterpret_cast<uint64_t>(module);
            auto it = cache.find(base);
            if (it == cache.end())
            {
                ModuleSymbols symbols;
                wchar_t path[MAX_PATH] = {};
                GetModuleFileNameW(module, path, MAX_PATH);
                symbols.label = "[" + leafOf(narrow(path)) + "]";
                const auto *bytes = reinterpret_cast<const uint8_t *>(module);
                const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(bytes);
                const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(bytes + dos->e_lfanew);
                const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
                if (dir.VirtualAddress != 0 && dir.Size != 0)
                {
                    const auto *exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(bytes + dir.VirtualAddress);
                    const auto *names = reinterpret_cast<const DWORD *>(bytes + exports->AddressOfNames);
                    const auto *ordinals = reinterpret_cast<const WORD *>(bytes + exports->AddressOfNameOrdinals);
                    const auto *functions = reinterpret_cast<const DWORD *>(bytes + exports->AddressOfFunctions);
                    for (DWORD i = 0; i < exports->NumberOfNames; ++i)
                    {
                        if (ordinals[i] >= exports->NumberOfFunctions)
                            continue;
                        const DWORD rva = functions[ordinals[i]];
                        if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size)
                            continue; // forwarded to another module
                        symbols.exports.emplace_back(rva, reinterpret_cast<const char *>(bytes + names[i]));
                    }
                    std::sort(symbols.exports.begin(), symbols.exports.end());
                }
                it = cache.emplace(base, std::move(symbols)).first;
            }
            const ModuleSymbols &symbols = it->second;
            const uint64_t rva = address - base;
            auto exp = std::upper_bound(symbols.exports.begin(), symbols.exports.end(), rva,
                                        [](uint64_t v, const std::pair<uint32_t, std::string> &e) { return v < e.first; });
            if (exp != symbols.exports.begin())
            {
                --exp;
                if (rva - exp->first < 0x400u)
                    return symbols.label + " " + exp->second;
            }
            return symbols.label;
        }

        // Where a thread is right now; false when it could not be paused.
        bool sampleThread(HANDLE thread, uint64_t &rip)
        {
            if (!thread || SuspendThread(thread) == static_cast<DWORD>(-1))
                return false;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            const BOOL ok = GetThreadContext(thread, &context);
            ResumeThread(thread);
            rip = context.Rip;
            return ok != FALSE;
        }

        std::string labelForAddress(uint64_t rip)
        {
            if (rip >= g_imageBase && rip < g_imageBase + g_imageSize)
                return labelForRva(rip - g_imageBase);
            return moduleLabel(rip);
        }

        void samplerMain()
        {
            registerThread("perf sampler");
            DWORD otherId = 0;
            HANDLE otherHandle = nullptr;
            while (g_samplerRunning.load(std::memory_order_relaxed))
            {
                Sleep(2);
                uint64_t rip = 0;
                if (sampleThread(g_sampledThread, rip))
                    recordSample(labelForAddress(rip));
                // The VU1 thread too (VIF1, the VU1 microprograms, PATH1 packets) and the GS feed thread.
                if (sampleThread(vuThreadHandle(), rip))
                    recordSample(labelForAddress(rip), 1);
                if (sampleThread(g_feedThreadHandle, rip))
                    recordSample(labelForAddress(rip), 2);
                // And whichever other thread used the most CPU (driver, audio, ...), to name it.
                const DWORD wanted = static_cast<DWORD>(g_otherThreadId.load(std::memory_order_relaxed));
                if (wanted != otherId)
                {
                    if (otherHandle)
                        CloseHandle(otherHandle);
                    otherHandle = wanted ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, wanted) : nullptr;
                    otherId = wanted;
                }
                if (otherHandle && sampleThread(otherHandle, rip))
                    recordSample(labelForAddress(rip), 3);
            }
            if (otherHandle)
                CloseHandle(otherHandle);
        }
#else
        pthread_t g_sampledThread{};
        std::atomic<uint64_t> g_lastSampleRip{0};
        std::atomic<bool> g_sampleReady{false};

        void onProfileSignal(int, siginfo_t *, void *contextPtr)
        {
            const ucontext_t *context = static_cast<const ucontext_t *>(contextPtr);
#if defined(__x86_64__)
            g_lastSampleRip.store(static_cast<uint64_t>(context->uc_mcontext.gregs[REG_RIP]), std::memory_order_relaxed);
#endif
            g_sampleReady.store(true, std::memory_order_release);
        }

        void samplerMain()
        {
            while (g_samplerRunning.load(std::memory_order_relaxed))
            {
                timespec delay{0, 2000000};
                nanosleep(&delay, nullptr);
                g_sampleReady.store(false, std::memory_order_relaxed);
                if (pthread_kill(g_sampledThread, SIGPROF) != 0)
                    continue;
                for (int spin = 0; spin < 2000 && !g_sampleReady.load(std::memory_order_acquire); ++spin)
                    std::this_thread::yield();
                if (!g_sampleReady.load(std::memory_order_acquire))
                    continue;
                const uint64_t rip = g_lastSampleRip.load(std::memory_order_relaxed);
                if (rip >= g_imageBase && rip < g_imageBase + g_imageSize)
                    recordSample(labelForRva(rip - g_imageBase));
                else
                {
                    Dl_info info{};
                    if (dladdr(reinterpret_cast<void *>(rip), &info) && info.dli_fname)
                    {
                        std::string label = "[" + leafOf(info.dli_fname) + "]";
                        if (info.dli_sname && info.dli_saddr && rip - reinterpret_cast<uint64_t>(info.dli_saddr) < 0x400u)
                            label += std::string(" ") + info.dli_sname;
                        recordSample(label);
                    }
                    else
                        recordSample("[outside the executable]");
                }
            }
        }
#endif

        void startSampler()
        {
            if (g_samplerRunning.exchange(true))
                return;
            loadSymbols();
#if defined(_WIN32)
            g_imageBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
            const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(g_imageBase);
            const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(g_imageBase + dos->e_lfanew);
            g_imageSize = nt->OptionalHeader.SizeOfImage;
            if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_sampledThread,
                                 THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
            {
                g_samplerRunning = false;
                return;
            }
#else
            g_sampledThread = pthread_self();
            dl_iterate_phdr([](dl_phdr_info *info, size_t, void *) -> int
                            {
                                // the first object is the executable: load bias + highest segment end
                                g_imageBase = info->dlpi_addr;
                                uint64_t end = 0;
                                for (int i = 0; i < info->dlpi_phnum; ++i)
                                    if (info->dlpi_phdr[i].p_type == PT_LOAD)
                                        end = std::max<uint64_t>(end, info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz);
                                g_imageSize = end;
                                return 1; },
                            nullptr);
            struct sigaction action{};
            action.sa_sigaction = onProfileSignal;
            action.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset(&action.sa_mask);
            sigaction(SIGPROF, &action, nullptr);
#endif
            std::thread(samplerMain).detach();
        }

        // ---------------------------------------------------------------- VU1 compiled-code misses
        struct Vu1MissImage
        {
            std::atomic<uint64_t> hash{0};
            std::atomic<uint64_t> stretches[2048];
            std::atomic<uint64_t> pairs[2048];
        };
        Vu1MissImage g_vu1Miss[8]; // VU1 and VU0 code images

        void writeVu1MissesLocked(FILE *file)
        {
            struct Row
            {
                uint64_t hash;
                uint32_t pc;
                uint64_t stretches;
                uint64_t pairs;
            };
            std::vector<Row> rows;
            uint64_t totalPairs = 0;
            for (Vu1MissImage &image : g_vu1Miss)
            {
                const uint64_t hash = image.hash.load(std::memory_order_relaxed);
                if (hash == 0)
                    continue;
                for (uint32_t i = 0; i < 2048u; ++i)
                {
                    const uint64_t pairs = image.pairs[i].exchange(0, std::memory_order_relaxed);
                    const uint64_t stretches = image.stretches[i].exchange(0, std::memory_order_relaxed);
                    if (pairs == 0)
                        continue;
                    rows.push_back({hash, i * 8u, stretches, pairs});
                    totalPairs += pairs;
                }
            }
            if (!file || rows.empty())
                return;
            std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.pairs > b.pairs; });
            char line[256];
            std::snprintf(line, sizeof(line), "           VU pairs interpreted inside compiled images (VU1 and VU0): %llu - entry points the compiled code lacks:",
                          static_cast<unsigned long long>(totalPairs));
            writeLine(file, line);
            for (size_t i = 0; i < rows.size() && i < 16u; ++i)
            {
                std::snprintf(line, sizeof(line), "           vu1-miss %016llx %04x stretches %llu pairs %llu",
                              static_cast<unsigned long long>(rows[i].hash), rows[i].pc,
                              static_cast<unsigned long long>(rows[i].stretches), static_cast<unsigned long long>(rows[i].pairs));
                writeLine(file, line);
            }
        }

        // Every 10 s: the busiest places on the game thread (and on Windows the VU1 thread).
        void writeProfileTableLocked(FILE *file, double seconds, int thread)
        {
            const bool vuThread = thread != 0;
            std::vector<std::pair<std::string, uint64_t>> rows;
            uint64_t total = 0;
            {
                std::lock_guard<std::mutex> lock(g_profileMutex);
                auto &counts = g_profileCounts[thread];
                uint64_t &samples = g_profileSamples[thread];
                rows.assign(counts.begin(), counts.end());
                total = samples;
                counts.clear();
                samples = 0;
            }
            if (!file || total == 0)
                return;
            std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            char line[1024];
            std::snprintf(line, sizeof(line), "           ---- where the %s spent the last %.0f s (%llu samples%s) ----",
                          kSampledNames[thread], seconds,
                          static_cast<unsigned long long>(total), g_symbols.empty() ? ", no symbol map" : "");
            writeLine(file, line);
            {
                std::vector<std::pair<std::string, uint64_t>> groups;
                for (const auto &row : rows)
                {
                    const char *group = categoryOf(row.first);
                    auto found = std::find_if(groups.begin(), groups.end(), [&](const auto &g) { return g.first == group; });
                    if (found == groups.end())
                        groups.emplace_back(group, row.second);
                    else
                        found->second += row.second;
                }
                std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                std::string summary = "           groups:";
                for (const auto &g : groups)
                {
                    char part[128];
                    std::snprintf(part, sizeof(part), " %s %.1f%%,", g.first.c_str(), 100.0 * static_cast<double>(g.second) / static_cast<double>(total));
                    summary += part;
                }
                if (summary.back() == ',')
                    summary.pop_back();
                writeLine(file, summary.c_str());
            }
            for (size_t i = 0; i < rows.size() && i < (vuThread ? 15u : 25u); ++i)
            {
                std::snprintf(line, sizeof(line), "           %5.1f%%  %s", 100.0 * static_cast<double>(rows[i].second) / static_cast<double>(total),
                              rows[i].first.c_str());
                writeLine(file, line);
            }
        }

        void writeProfileLocked(FILE *file, double seconds)
        {
            for (int thread = 0; thread < kSampledThreads; ++thread)
                writeProfileTableLocked(file, seconds, thread);
        }

        // Resident memory of the whole process (RAM the game occupies right now).
        uint64_t processMemoryBytes()
        {
#if defined(_WIN32)
            struct Counters
            {
                DWORD cb;
                DWORD PageFaultCount;
                SIZE_T PeakWorkingSetSize;
                SIZE_T WorkingSetSize;
                SIZE_T QuotaPeakPagedPoolUsage;
                SIZE_T QuotaPagedPoolUsage;
                SIZE_T QuotaPeakNonPagedPoolUsage;
                SIZE_T QuotaNonPagedPoolUsage;
                SIZE_T PagefileUsage;
                SIZE_T PeakPagefileUsage;
            };
            using Fn = BOOL(WINAPI *)(HANDLE, Counters *, DWORD);
            static const Fn fn = []() -> Fn
            {
                HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
                return kernel ? reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(kernel, "K32GetProcessMemoryInfo"))) : nullptr;
            }();
            Counters counters{};
            counters.cb = sizeof(counters);
            if (fn && fn(GetCurrentProcess(), &counters, sizeof(counters)))
                return static_cast<uint64_t>(counters.WorkingSetSize);
            return 0;
#else
            FILE *file = std::fopen("/proc/self/statm", "r");
            if (!file)
                return 0;
            unsigned long long size = 0, resident = 0;
            const int got = std::fscanf(file, "%llu %llu", &size, &resident);
            std::fclose(file);
            return got == 2 ? static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGE_SIZE)) : 0u;
#endif
        }

        // ---------------------------------------------------------------- graphics description
        std::mutex g_infoMutex;
        RendererInfo g_renderer{"Software (CPU)", "CPU", "", false, 1u};
        bool g_rendererSet = false;
        std::string g_presenter;
        uint32_t g_gameW = 0, g_gameH = 0, g_windowW = 0, g_windowH = 0;

        // ---------------------------------------------------------------- session totals
        struct Session
        {
            uint64_t intervals = 0;
            uint64_t fullSpeedIntervals = 0;
            double speedSum = 0.0;
            double slowestSpeed = 1e9;
            double slowestAt = 0.0;
            double bucketMsSum[BucketCount] = {};
            double processCoresSum = 0.0;
            double windowFpsSum = 0.0;
            uint64_t slowFrames = 0;
            double startSeconds = 0.0;
            uint64_t pausedIntervals = 0; // F2 drawer paused the game: left out of the averages
        };
        Session g_session;
        std::atomic<bool> g_shutdown{false};

        // ---------------------------------------------------------------- watchdog
        // A freeze on someone's PC is found from their logs: when the game, VU1 or window thread
        // makes no progress for 5 s, write where each of them is.
        std::atomic<uint64_t> g_beatGame{0};    // ns of the game thread's last VBlank
        std::atomic<uint64_t> g_beatWindow{0};  // ns of the window thread's last frame
        std::atomic<uint64_t> g_vuBusySince{0}; // ns the VU1 thread started its current batch, 0 = idle
        std::atomic<uint64_t> g_closingSince{0}; // ns the window closed, 0 = still running

        // FPS counter: a snapshot every 250 ms, rates over the last four (one second).
        struct LiveSnapshot
        {
            uint64_t ns = 0, windowFrames = 0, gameFrames = 0, eeCycle = 0;
        };
        LiveSnapshot g_liveRing[5];
        int g_liveCount = 0;
        uint64_t g_liveWindowFrames = 0;
        std::atomic<uint64_t> g_liveGameFrames{0}; // game thread adds, window thread reads
        LiveStats g_liveStats;

        void updateLiveStats(uint64_t now, uint64_t eeCycle)
        {
            ++g_liveWindowFrames;
            const LiveSnapshot &newest = g_liveRing[(g_liveCount + 4) % 5];
            if (g_liveCount != 0 && now < newest.ns + 250000000ull)
                return;
            LiveSnapshot &slot = g_liveRing[g_liveCount % 5];
            const uint64_t gameFrames = g_liveGameFrames.load(std::memory_order_relaxed);
            slot = {now, g_liveWindowFrames, gameFrames, eeCycle};
            ++g_liveCount;
            if (g_liveCount < 2)
                return;
            const LiveSnapshot &oldest = g_liveRing[g_liveCount >= 5 ? g_liveCount % 5 : 0];
            const double seconds = static_cast<double>(now - oldest.ns) / 1e9;
            if (seconds <= 0.0)
                return;
            g_liveStats.windowFps = static_cast<double>(g_liveWindowFrames - oldest.windowFrames) / seconds;
            g_liveStats.gameFps = static_cast<double>(gameFrames - oldest.gameFrames) / seconds;
            constexpr double kEeHz = 294912000.0;
            g_liveStats.speedPercent = eeCycle >= oldest.eeCycle
                                           ? 100.0 * static_cast<double>(eeCycle - oldest.eeCycle) / kEeHz / seconds
                                           : -1.0;
            g_liveStats.paused = ps2settings::emulationPaused();
        }
        std::atomic<bool> g_watchdogStarted{false};
        constexpr uint64_t kStallNs = 5000000000ull;
        double secondsOf(uint64_t ns) { return static_cast<double>(ns) / 1e9; }
#if defined(_WIN32)
        HANDLE g_watchHandles[3] = {};   // game, VU1, window: suspend + context rights
        uint64_t g_watchStackTop[3] = {}; // their stack bases (highest address)
        const NT_TIB *g_watchTib[3] = {}; // their thread blocks (StackLimit = lowest committed stack byte)
        HANDLE vuThreadHandle() { return g_watchHandles[1]; }

        // Call stack of a thread, symbolized: our functions from TMHO.sym, others as [module] export.
        // While the thread is paused only its registers and the top 256 KB of its stack are copied;
        // the copy is unwound after it runs again, so nothing here can wait on a lock it holds.
        std::string stackOf(int index)
        {
            HANDLE thread = g_watchHandles[index];
            const uint64_t top = g_watchStackTop[index];
            constexpr size_t kCopyBytes = 256u * 1024u;
            static std::vector<uint8_t> copy(kCopyBytes + 64u * 1024u); // slack for a bad unwind reading past the end
            if (!thread || top == 0 || !g_watchTib[index] || SuspendThread(thread) == static_cast<DWORD>(-1))
                return "(could not pause it)";
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            const BOOL gotContext = GetThreadContext(thread, &context);
            const uint64_t stackLow = context.Rsp;
            // Only the committed part of its stack: below StackLimit is the guard page, and touching
            // another thread's guard page faults in this thread.
            const uint64_t committedLow = reinterpret_cast<uint64_t>(g_watchTib[index]->StackLimit);
            size_t copied = 0;
            if (gotContext && stackLow >= committedLow && stackLow < top)
            {
                copied = static_cast<size_t>(std::min<uint64_t>(top - stackLow, kCopyBytes));
                std::memcpy(copy.data(), reinterpret_cast<const void *>(stackLow), copied);
            }
            ResumeThread(thread);
            if (!gotContext)
                return "(could not read its registers)";
            if (copied == 0)
                return "(its stack pointer is outside its stack right now)";

            const uint64_t copyLow = reinterpret_cast<uint64_t>(copy.data());
            const uint64_t delta = copyLow - stackLow;
            // Point every register that points into the original stack at the copy instead.
            auto relocate = [&](CONTEXT &c) {
                DWORD64 *regs[] = {&c.Rax, &c.Rcx, &c.Rdx, &c.Rbx, &c.Rsp, &c.Rbp, &c.Rsi, &c.Rdi,
                                   &c.R8, &c.R9, &c.R10, &c.R11, &c.R12, &c.R13, &c.R14, &c.R15};
                for (DWORD64 *r : regs)
                    if (*r >= stackLow && *r < stackLow + copied)
                        *r += delta;
            };
            relocate(context);
            uint64_t frames[24];
            int count = 0;
            while (count < 24 && context.Rip != 0)
            {
                frames[count++] = context.Rip;
                if (context.Rsp < copyLow || context.Rsp + 8u > copyLow + copied)
                    break; // left the copied part of the stack
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (!function)
                {
                    // a leaf function: its return address is on top of the stack
                    std::memcpy(&context.Rip, reinterpret_cast<const void *>(context.Rsp), 8);
                    context.Rsp += 8;
                    continue;
                }
                void *handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                 &establisher, nullptr);
                relocate(context); // registers restored from the stack hold original addresses
            }
            std::string text;
            std::string previous;
            for (int i = 0; i < count; ++i)
            {
                const uint64_t pc = frames[i];
                std::string label = (pc >= g_imageBase && pc < g_imageBase + g_imageSize) ? labelForRva(pc - g_imageBase)
                                                                                           : moduleLabel(pc);
                if (label == previous)
                    continue;
                previous = label;
                if (!text.empty())
                    text += "  <-  ";
                text += label;
            }
            return text.empty() ? "(no frames)" : text;
        }
#endif

        void watchdogLine(const std::string &line)
        {
            std::fprintf(stderr, "%s\n", line.c_str());
            std::fflush(stderr);
            std::unique_lock<std::mutex> lock(g_fileMutex, std::try_to_lock);
            if (lock.owns_lock() && g_perfFile)
            {
                writeLine(g_perfFile, line.c_str());
                std::fflush(g_perfFile);
            }
        }

        void watchdogMain()
        {
            uint64_t lastReport = 0;
            int reports = 0;
            uint64_t firstGameBeat = 0;
            bool selfChecked = false;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (g_shutdown)
                    return;
                const uint64_t now = nowNs();
                if (!selfChecked)
                {
                    if (firstGameBeat == 0)
                        firstGameBeat = g_beatGame.load(std::memory_order_relaxed);
                    else if (now > firstGameBeat + 20000000000ull)
                    {
                        selfChecked = true;
#if defined(_WIN32)
                        watchdogLine("[watchdog] watching the game, VU1 and window threads for freezes (self-check: the game thread is in " +
                                     stackOf(0) + ")");
#else
                        watchdogLine("[watchdog] watching the game, VU1 and window threads for freezes");
#endif
                    }
                }
                const uint64_t closing = g_closingSince.load(std::memory_order_relaxed);
                if (closing != 0)
                {
                    if (now < closing + 10000000000ull)
                        continue; // stopping the threads normally takes well under a second
                    char line[256];
                    std::snprintf(line, sizeof(line), "[watchdog] the game has not finished closing %.0f s after its window closed; "
                                                      "ending the process. Where its threads are:", secondsOf(now - closing));
                    watchdogLine(line);
                    const uint64_t vuSince = g_vuBusySince.load(std::memory_order_relaxed);
                    if (vuSince != 0)
                        std::snprintf(line, sizeof(line), "[watchdog]   VU1 thread: inside one batch of work for %.1f s", secondsOf(now - vuSince));
                    else
                        std::snprintf(line, sizeof(line), "[watchdog]   VU1 thread: idle (waiting for work)");
                    watchdogLine(line);
#if defined(_WIN32)
                    const char *const names[3] = {"game", "VU1", "window"};
                    for (int i = 0; i < 3; ++i)
                        if (g_watchHandles[i])
                            watchdogLine(std::string("[watchdog]   ") + names[i] + " thread is in: " + stackOf(i));
#endif
                    ps2perf::shutdown(); // the session summary, then close the logs
                    std::fflush(nullptr);
#if defined(_WIN32)
                    TerminateProcess(GetCurrentProcess(), 3);
#endif
                    _exit(3);
                }
                if (ps2settings::emulationPaused() && g_beatGame.load(std::memory_order_relaxed) != 0)
                    g_beatGame.store(now, std::memory_order_relaxed); // paused on purpose
                const uint64_t game = g_beatGame.load(std::memory_order_relaxed);
                const uint64_t window = g_beatWindow.load(std::memory_order_relaxed);
                const uint64_t vuSince = g_vuBusySince.load(std::memory_order_relaxed);
                const bool gameStuck = game != 0 && !ps2settings::emulationPaused() && now > game + kStallNs;
                const bool windowStuck = window != 0 && now > window + kStallNs;
                const bool vuStuck = vuSince != 0 && now > vuSince + kStallNs;
                if (!gameStuck && !windowStuck && !vuStuck)
                {
                    lastReport = 0;
                    continue;
                }
                if ((lastReport != 0 && now < lastReport + 30000000000ull) || reports >= 6)
                    continue;
                lastReport = now;
                ++reports;
                auto ago = [now](uint64_t t) { return t == 0 ? -1.0 : secondsOf(now > t ? now - t : 0); };
                char line[512];
                std::snprintf(line, sizeof(line), "[watchdog] %.1f s into the session something stopped making progress:",
                              secondsOf(now) - g_session.startSeconds);
                watchdogLine(line);
                std::snprintf(line, sizeof(line), "[watchdog]   game thread: last VBlank %.1f s ago%s", ago(game), gameStuck ? "  <-- STUCK" : "");
                watchdogLine(line);
                if (vuSince != 0)
                    std::snprintf(line, sizeof(line), "[watchdog]   VU1 thread: inside one batch of work for %.1f s%s", ago(vuSince), vuStuck ? "  <-- STUCK" : "");
                else
                    std::snprintf(line, sizeof(line), "[watchdog]   VU1 thread: idle (waiting for work)");
                watchdogLine(line);
                std::snprintf(line, sizeof(line), "[watchdog]   window thread: last frame %.1f s ago%s", ago(window),
                              windowStuck ? "  <-- STUCK (normal while the window is minimized)" : "");
                watchdogLine(line);
#if defined(_WIN32)
                const char *const names[3] = {"game", "VU1", "window"};
                for (int i = 0; i < 3; ++i)
                {
                    if (!g_watchHandles[i])
                        continue;
                    watchdogLine(std::string("[watchdog]   ") + names[i] + " thread is in: " + stackOf(i));
                }
#endif
            }
        }

        void startWatchdog()
        {
            if (g_watchdogStarted.exchange(true))
                return;
            loadSymbols();
#if defined(_WIN32)
            if (g_imageBase == 0)
            {
                g_imageBase = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
                const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(g_imageBase);
                const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(g_imageBase + dos->e_lfanew);
                g_imageSize = nt->OptionalHeader.SizeOfImage;
            }
#endif
            std::thread(watchdogMain).detach();
        }

        void writeLine(FILE *file, const char *text)
        {
            if (!file)
                return;
            std::fputs(text, file);
            std::fputc('\n', file);
        }

        void writeHeaders()
        {
            const std::string started = localTimeText();
            const unsigned logical = std::max(1u, std::thread::hardware_concurrency());
            char line[1024];

            if (g_perfFile)
            {
                writeLine(g_perfFile, "================================================================================");
                writeLine(g_perfFile, " Twisted Metal: Head-On - PC port  |  PERFORMANCE LOG");
                writeLine(g_perfFile, "================================================================================");
                std::snprintf(line, sizeof(line), " Session started : %s", started.c_str());
                writeLine(g_perfFile, line);
                std::snprintf(line, sizeof(line), " Executable      : %s", g_exePath.c_str());
                writeLine(g_perfFile, line);
                std::snprintf(line, sizeof(line), " CPU             : %s (%u logical processors)", cpuName().c_str(), logical);
                writeLine(g_perfFile, line);
                std::snprintf(line, sizeof(line), " Memory          : %.1f GB", memoryGb());
                writeLine(g_perfFile, line);
                std::snprintf(line, sizeof(line), " OS              : %s", osName().c_str());
                writeLine(g_perfFile, line);
                writeLine(g_perfFile, "");
                writeLine(g_perfFile, " One report every 2 seconds. \"ms/s\" = milliseconds of work per second of real time;");
                writeLine(g_perfFile, " 1000 ms/s means one CPU core is completely busy with that job.");
                writeLine(g_perfFile, "   speed      emulated PS2 time vs real time (100% = full speed)");
                writeLine(g_perfFile, "   logic      game logic updates per second (60 = smooth)");
                writeLine(g_perfFile, "   window     frames the PC window shows per second, and its slowest frame");
                writeLine(g_perfFile, "   game thr.  the thread that runs the game: EE code, VU0/VU1 microcode, VIF, IOP,");
                writeLine(g_perfFile, "              and with the software renderer also the GS drawing. If speed is below");
                writeLine(g_perfFile, "              100%, the biggest number on that line is what holds the game back.");
                writeLine(g_perfFile, "   CPU        CPU cores the whole game uses, and each of its busy threads");
                writeLine(g_perfFile, "--------------------------------------------------------------------------------");
                std::fflush(g_perfFile);
            }
            if (g_gfxFile)
            {
                writeLine(g_gfxFile, "================================================================================");
                writeLine(g_gfxFile, " Twisted Metal: Head-On - PC port  |  GRAPHICS LOG");
                writeLine(g_gfxFile, "================================================================================");
                std::snprintf(line, sizeof(line), " Session started : %s", started.c_str());
                writeLine(g_gfxFile, line);
                std::snprintf(line, sizeof(line), " CPU             : %s (%u logical processors)", cpuName().c_str(), logical);
                writeLine(g_gfxFile, line);
                writeLine(g_gfxFile, "");
                writeLine(g_gfxFile, " The PS2's graphics chip (the GS) is emulated by a \"GS renderer\": either the CPU");
                writeLine(g_gfxFile, " (software) or the graphics card (Vulkan GPU). The window itself is always shown");
                writeLine(g_gfxFile, " with OpenGL on the graphics card. Each report says who did the drawing work.");
                writeLine(g_gfxFile, "--------------------------------------------------------------------------------");
                std::fflush(g_gfxFile);
            }
        }

        // Caller holds g_fileMutex.
        void ensureFilesLocked()
        {
            if (g_filesTried)
                return;
            g_filesTried = true;
            if (perfMode() == 0)
                return;

            g_exePath = executablePath();
            if (const char *env = std::getenv("PS2X_LOG_DIR"); env && *env)
            {
                g_logDir = env;
            }
            else
            {
                std::string base = parentOf(g_exePath);
                if (iequals(leafOf(base), "bin"))
                    base = parentOf(base);
                g_logDir = base + "/Logs";
#if defined(_WIN32)
                for (char &c : g_logDir)
                    if (c == '/')
                        c = '\\';
#endif
            }
            makeDirectory(g_logDir);
#if defined(_WIN32)
            const char sep = '\\';
#else
            const char sep = '/';
#endif
            const std::string perfPath = g_logDir + sep + "performancelogs.txt";
            const std::string gfxPath = g_logDir + sep + "Graphicslogs.txt";
            // Keep the previous session for comparison.
            moveFile(perfPath, g_logDir + sep + "performancelogs (previous run).txt");
            moveFile(gfxPath, g_logDir + sep + "Graphicslogs (previous run).txt");
            g_perfFile = openFile(perfPath);
            g_gfxFile = openFile(gfxPath);
            if (!g_perfFile || !g_gfxFile)
                std::fprintf(stderr, "[perf] could not open log files in %s\n", g_logDir.c_str());
            writeHeaders();
        }

        void noteLocked(FILE *file, const char *text)
        {
            if (!file)
                return;
            writeLine(file, text);
            std::fflush(file);
        }

        double nsToSeconds(uint64_t ns) { return static_cast<double>(ns) / 1e9; }

        const char *bucketLabel(uint32_t bucket)
        {
            switch (bucket)
            {
            case Vu1: return "VU1 microcode (3D transforms)";
            case Vu0: return "VU0 microcode";
            case Vif1: return "VIF1 data unpacking";
            case GsPacket: return "GS packet parsing";
            case GsRaster: return "GS drawing on the CPU";
            case GpuFrame: return "GPU frame hand-off";
            case GpuSync: return "waiting for the GPU";
            case Iop: return "IOP (sound/disc/pad processor)";
            default: return "?";
            }
        }

        std::string formatCount(double value)
        {
            char text[32];
            if (value >= 1e6)
                std::snprintf(text, sizeof(text), "%.2fM", value / 1e6);
            else if (value >= 1e4)
                std::snprintf(text, sizeof(text), "%.1fk", value / 1e3);
            else
                std::snprintf(text, sizeof(text), "%.0f", value);
            return text;
        }
    }

    bool enabled() noexcept
    {
        return perfMode() != 0;
    }

    void setGauge(Gauge gauge, uint64_t value) noexcept
    {
        g_gauges[gauge].store(value, std::memory_order_relaxed);
    }

    void markMainThread() noexcept
    {
        t_mainThread = true;
    }

    void heartbeat(Beat beat) noexcept
    {
        switch (beat)
        {
        case Beat::Game:
            g_beatGame.store(nowNs(), std::memory_order_relaxed);
            break;
        case Beat::VuBegin:
            g_vuBusySince.store(nowNs(), std::memory_order_relaxed);
            break;
        case Beat::VuEnd:
            g_vuBusySince.store(0, std::memory_order_relaxed);
            break;
        case Beat::Closing:
            g_closingSince.store(nowNs(), std::memory_order_relaxed);
            break;
        }
    }

    uint64_t nowNs() noexcept
    {
#if defined(_WIN32)
        // QueryPerformanceCounter directly: steady_clock::now() goes through winpthreads'
        // clock_gettime, which costs several times more, and every perf scope calls this twice
        // (tens of thousands of VU0 microprograms a second in a big battle).
        struct Clock
        {
            int64_t start;
            double nsPerTick;
        };
        static const Clock clock = []()
        {
            LARGE_INTEGER frequency{}, start{};
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&start);
            return Clock{start.QuadPart, 1e9 / static_cast<double>(frequency.QuadPart ? frequency.QuadPart : 1)};
        }();
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<uint64_t>(static_cast<double>(now.QuadPart - clock.start) * clock.nsPerTick);
#else
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - g_epoch)
                                         .count());
#endif
    }

    void add(Bucket bucket, uint64_t ns) noexcept
    {
        if (t_feedThread)
        {
            const int slot = bucket == GsPacket ? 0 : (bucket == GpuFrame || bucket == GpuSync) ? 1 : 2;
            g_feedNs[slot].fetch_add(ns, std::memory_order_relaxed);
            return;
        }
        if (t_vuThread)
            bucket = vuThreadBucket(bucket);
        g_buckets[bucket].fetch_add(ns, std::memory_order_relaxed);
    }

    void count(Counter counter, uint64_t value) noexcept
    {
        g_counters[counter].fetch_add(value, std::memory_order_relaxed);
    }

    void countMax(Counter counter, uint64_t value) noexcept
    {
        uint64_t current = g_counters[counter].load(std::memory_order_relaxed);
        while (value > current &&
               !g_counters[counter].compare_exchange_weak(current, value, std::memory_order_relaxed))
        {
        }
    }

    void registerThread(const char *name) noexcept
    {
#if defined(_WIN32)
        // Give the thread a name Windows shows (and the thread list in this log uses), unless it
        // already has one.
        if (name && *name)
        {
            using SetDescription = HRESULT(WINAPI *)(HANDLE, PCWSTR);
            using GetDescription = HRESULT(WINAPI *)(HANDLE, PWSTR *);
            static const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            static const auto setFn = kernel ? reinterpret_cast<SetDescription>(reinterpret_cast<void *>(GetProcAddress(kernel, "SetThreadDescription"))) : nullptr;
            static const auto getFn = kernel ? reinterpret_cast<GetDescription>(reinterpret_cast<void *>(GetProcAddress(kernel, "GetThreadDescription"))) : nullptr;
            bool named = false;
            PWSTR current = nullptr;
            if (getFn && SUCCEEDED(getFn(GetCurrentThread(), &current)) && current)
            {
                named = current[0] != L'\0';
                LocalFree(current);
            }
            if (setFn && !named)
            {
                wchar_t wide[64] = {};
                for (size_t i = 0; i + 1 < 64 && name[i] && name[i] != '('; ++i)
                    wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
                for (size_t n = wcslen(wide); n > 0 && wide[n - 1] == L' '; --n)
                    wide[n - 1] = L'\0';
                setFn(GetCurrentThread(), wide);
            }
        }
#endif
        if (name && std::strncmp(name, "VU1 thread", 10) == 0)
            t_vuThread = true;
        if (name && std::strncmp(name, "GS feed", 7) == 0)
        {
            t_feedThread = true;
            g_feedSeen.store(true, std::memory_order_relaxed);
#if defined(_WIN32)
            if (!g_feedThreadHandle)
                DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_feedThreadHandle,
                                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
#endif
        }
        if (name)
        {
            const int watch = std::strncmp(name, "game", 4) == 0 ? 0 : std::strncmp(name, "VU1 thread", 10) == 0 ? 1
                                                                   : std::strncmp(name, "window", 6) == 0     ? 2
                                                                                                              : -1;
#if defined(_WIN32)
            if (watch >= 0 && !g_watchHandles[watch] &&
                DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_watchHandles[watch],
                                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
            {
                g_watchTib[watch] = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
                g_watchStackTop[watch] = reinterpret_cast<uint64_t>(g_watchTib[watch]->StackBase);
            }
#endif
            if (watch >= 0)
                startWatchdog();
        }
        if (!enabled())
            return;
        try
        {
            ThreadEntry entry;
            entry.name = name ? name : "thread";
#if defined(_WIN32)
            if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &entry.handle,
                                 THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
                return;
#else
            if (pthread_getcpuclockid(pthread_self(), &entry.clock) != 0)
                return;
#endif
            entry.lastNs = threadCpuNs(entry);
            const bool isGame = entry.name.rfind("game", 0) == 0;

            {
                std::lock_guard<std::mutex> lock(g_threadMutex);
                if (g_threads.size() < 128u)
                    g_threads.push_back(std::move(entry));
            }
            if (isGame && profilerEnabled())
                startSampler(); // samples the calling thread
        }
        catch (...)
        {
        }
    }

    std::string logDirectory()
    {
        std::lock_guard<std::mutex> lock(g_fileMutex);
        if (!g_filesTried)
        {
            // Resolve the folder even when the performance logs are off.
            const int saved = perfMode();
            (void)saved;
            ensureFilesLocked();
        }
        if (g_logDir.empty())
        {
            g_exePath = executablePath();
            std::string base = parentOf(g_exePath);
            if (iequals(leafOf(base), "bin"))
                base = parentOf(base);
            g_logDir = base + "/Logs";
            makeDirectory(g_logDir);
        }
        return g_logDir;
    }

    void setRenderer(const RendererInfo &info) noexcept
    {
        try
        {
            {
                std::lock_guard<std::mutex> lock(g_infoMutex);
                g_renderer = info;
                g_rendererSet = true;
            }
            char line[1024];
            std::lock_guard<std::mutex> fileLock(g_fileMutex);
            ensureFilesLocked();
            std::snprintf(line, sizeof(line), " GS renderer     : %s   <- this draws the PS2 graphics", info.name.c_str());
            noteLocked(g_gfxFile, line);
            std::snprintf(line, sizeof(line), "   device        : %s", info.device.c_str());
            noteLocked(g_gfxFile, line);
            if (!info.details.empty())
            {
                std::snprintf(line, sizeof(line), "   details       : %s", info.details.c_str());
                noteLocked(g_gfxFile, line);
            }
            if (info.gpu)
            {
                std::snprintf(line, sizeof(line), "   internal res. : %ux super-sampling%s", info.superSampling,
                              info.superSampling <= 1u ? " (native PS2 resolution)" : "");
                noteLocked(g_gfxFile, line);
            }
            std::snprintf(line, sizeof(line), " Renderer        : %s on %s", info.name.c_str(), info.device.c_str());
            noteLocked(g_perfFile, line);
        }
        catch (...)
        {
        }
    }

    void setPresenter(const std::string &description) noexcept
    {
        try
        {
            {
                std::lock_guard<std::mutex> lock(g_infoMutex);
                g_presenter = description;
            }
            std::lock_guard<std::mutex> fileLock(g_fileMutex);
            ensureFilesLocked();
            const std::string line = " Window / screen : " + description;
            noteLocked(g_gfxFile, line.c_str());
        }
        catch (...)
        {
        }
    }

    void setOutputSize(uint32_t gameWidth, uint32_t gameHeight, uint32_t windowWidth, uint32_t windowHeight) noexcept
    {
        std::lock_guard<std::mutex> lock(g_infoMutex);
        g_gameW = gameWidth;
        g_gameH = gameHeight;
        g_windowW = windowWidth;
        g_windowH = windowHeight;
    }

    void graphicsNote(const char *fmt, ...) noexcept
    {
        if (!enabled())
            return;
        char text[2048];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        std::lock_guard<std::mutex> lock(g_fileMutex);
        ensureFilesLocked();
        noteLocked(g_gfxFile, text);
    }

    LiveStats liveStats() noexcept
    {
        return g_liveStats;
    }

    void noteGameFrame() noexcept
    {
        g_liveGameFrames.fetch_add(1, std::memory_order_relaxed);
        if (enabled())
            count(DisplayFlips);
    }

    double lastSpeedPercent() noexcept
    {
        const int64_t tenths = g_lastSpeedTenths.load(std::memory_order_relaxed);
        return tenths < 0 ? -1.0 : static_cast<double>(tenths) / 10.0;
    }

    void vu1CompiledMiss(uint64_t imageHash, uint32_t pc, uint32_t interpretedPairs) noexcept
    {
        if (imageHash == 0 || pc >= 16384u)
            return;
        for (Vu1MissImage &image : g_vu1Miss)
        {
            uint64_t current = image.hash.load(std::memory_order_relaxed);
            if (current == 0 && image.hash.compare_exchange_strong(current, imageHash))
                current = imageHash;
            if (current == imageHash)
            {
                image.stretches[pc / 8u].fetch_add(1, std::memory_order_relaxed);
                image.pairs[pc / 8u].fetch_add(interpretedPairs, std::memory_order_relaxed);
                return;
            }
        }
    }

    // Testing only (settings.ini cpu_limit_percent): the whole game may use at most this share of
    // all CPU cores together, enforced by Windows (a job object with a hard CPU-rate cap).
    void applyCpuLimit(int percent) noexcept
    {
        if (percent <= 0 || percent >= 100)
            return;
#if defined(_WIN32)
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job)
            return;
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION rate{};
        rate.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        rate.CpuRate = static_cast<DWORD>(percent) * 100u; // in 1/100 of a percent
        const bool ok = SetInformationJobObject(job, JobObjectCpuRateControlInformation, &rate, sizeof(rate)) &&
                        AssignProcessToJobObject(job, GetCurrentProcess());
        performanceNote("           CPU limit (testing): %s - the game may use %d%% of all CPU cores together", ok ? "on" : "could not be set", percent);
        std::fprintf(stderr, "[perf] CPU limit %d%%: %s\n", percent, ok ? "on" : "failed");
#else
        performanceNote("           CPU limit (testing): %d%% asked for, only available on Windows", percent);
#endif
    }

    void performanceNote(const char *fmt, ...) noexcept
    {
        if (!enabled())
            return;
        char text[2048];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        std::lock_guard<std::mutex> lock(g_fileMutex);
        ensureFilesLocked();
        noteLocked(g_perfFile, text);
    }

    Scope::Scope(Bucket bucket) noexcept
        : m_bucket(bucket), m_active(enabled())
    {
        if (!m_active)
            return;
        if (t_mainThread && bucket == GsRaster)
            m_bucket = MainRaster;
        m_start = nowNs();
        m_savedChild = t_childNs;
        t_childNs = 0;
    }

    Scope::~Scope()
    {
        if (!m_active)
            return;
        const uint64_t elapsed = nowNs() - m_start;
        const uint64_t exclusive = elapsed > t_childNs ? elapsed - t_childNs : 0u;
        add(m_bucket, exclusive);
        t_childNs = m_savedChild + elapsed;
    }

    void recordWait(bool pacing, uint64_t requestedNs, uint64_t actualNs, bool timedOut) noexcept
    {
        if (!enabled())
            return;
        count(pacing ? PaceWaits : IdleWaits);
        count(pacing ? PaceRequestedNs : IdleRequestedNs, requestedNs);
        if (timedOut && actualNs > requestedNs)
        {
            const uint64_t over = actualNs - requestedNs;
            count(pacing ? PaceOvershootNs : IdleOvershootNs, over);
            countMax(pacing ? PaceOvershootMaxNs : IdleOvershootMaxNs, over);
        }
    }

    namespace
    {
        // What the window thread hands the report thread each frame.
        std::atomic<uint64_t> g_tickEeCycle{0};
        std::atomic<const uint8_t *> g_tickRdram{nullptr};
        std::atomic<uint64_t> g_framesThisInterval{0};
        std::atomic<uint64_t> g_pausedFrames{0};
        std::atomic<bool> g_reportThreadStarted{false};
        std::atomic<bool> g_reportThreadRunning{false};

        void writeReport(uint64_t now, uint64_t eeCycle, const uint8_t *rdram) noexcept;

        // The 2-second report (thread census, profile, file writes) takes milliseconds; on the
        // window thread it showed up as a hitch every report. It has its own thread instead.
        void reportMain()
        {
            registerThread("perf report");
            uint64_t next = nowNs() + kReportIntervalNs;
            struct Running
            {
                ~Running() { g_reportThreadRunning.store(false, std::memory_order_release); }
            } running;
            while (!g_shutdown.load(std::memory_order_relaxed))
            {
                const uint64_t now = nowNs();
                if (now < next)
                {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<uint64_t>(next - now, 100000000ull)));
                    continue;
                }
                next = now + kReportIntervalNs;
                writeReport(now, g_tickEeCycle.load(std::memory_order_relaxed), g_tickRdram.load(std::memory_order_relaxed));
            }
        }
    }

    void mainThreadTick(uint64_t eeCycle, const uint8_t *rdram) noexcept
    {
        const uint64_t now = nowNs();
        g_beatWindow.store(now, std::memory_order_relaxed);
        updateLiveStats(now, eeCycle);
        if (!enabled() || g_shutdown)
            return;

        static uint64_t lastFrameNs = 0;
        if (lastFrameNs != 0u)
        {
            const uint64_t frameNs = now - lastFrameNs;
            countMax(MaxFrameNs, frameNs);
            if (frameNs > 25000000ull)
                count(SlowFrames);
        }
        lastFrameNs = now;
        count(MainFrames);
        // Frames shown while the F2 drawer had the game paused (this report interval).
        g_framesThisInterval.fetch_add(1u, std::memory_order_relaxed);
        if (ps2settings::emulationPaused())
            g_pausedFrames.fetch_add(1u, std::memory_order_relaxed);
        g_tickEeCycle.store(eeCycle, std::memory_order_relaxed);
        g_tickRdram.store(rdram, std::memory_order_relaxed);
        if (!g_reportThreadStarted.exchange(true, std::memory_order_relaxed))
        {
            try
            {
                g_reportThreadRunning.store(true, std::memory_order_release);
                std::thread(reportMain).detach();
            }
            catch (...)
            {
                g_reportThreadRunning.store(false, std::memory_order_release);
            }
        }
    }

    namespace
    {
    void writeReport(uint64_t now, uint64_t eeCycle, const uint8_t *rdram) noexcept
    {
        static uint64_t lastNs = now - kReportIntervalNs;
        static uint64_t lastCycle = eeCycle;
        static uint64_t lastBuckets[BucketCount] = {};
        static uint64_t lastCounters[CounterCount] = {};
        static uint64_t lastProcessNs = processCpuNs();
        static uint32_t lastTick = 0;
        static bool haveTick = false;
        static bool first = true;
        if (first)
        {
            first = false;
            std::lock_guard<std::mutex> lock(g_fileMutex);
            g_session.startSeconds = nsToSeconds(now) - nsToSeconds(kReportIntervalNs);
        }

        const uint64_t interval = now - lastNs;
        if (interval == 0u)
            return;
        const uint64_t framesThisInterval = g_framesThisInterval.exchange(0u, std::memory_order_relaxed);
        const uint64_t pausedFrames = g_pausedFrames.exchange(0u, std::memory_order_relaxed);

        try
        {
            uint64_t buckets[BucketCount];
            uint64_t counters[CounterCount];
            for (uint32_t i = 0; i < BucketCount; ++i)
            {
                const uint64_t value = g_buckets[i].load(std::memory_order_relaxed);
                buckets[i] = value - lastBuckets[i];
                lastBuckets[i] = value;
            }
            for (uint32_t i = 0; i < CounterCount; ++i)
            {
                if (i == IdleOvershootMaxNs || i == PaceOvershootMaxNs || i == MaxFrameNs)
                {
                    counters[i] = g_counters[i].exchange(0u, std::memory_order_relaxed); // maxima are per interval
                    continue;
                }
                const uint64_t value = g_counters[i].load(std::memory_order_relaxed);
                counters[i] = value - lastCounters[i];
                lastCounters[i] = value;
            }
            static uint64_t lastFeed[3] = {};
            uint64_t feed[3];
            for (int i = 0; i < 3; ++i)
            {
                const uint64_t value = g_feedNs[i].load(std::memory_order_relaxed);
                feed[i] = value - lastFeed[i];
                lastFeed[i] = value;
            }

            const double seconds = static_cast<double>(interval) / 1e9;
            const double perSecond = 1.0 / seconds;
            auto msPerSecond = [&](uint64_t ns)
            { return static_cast<double>(ns) / 1e6 * perSecond; };
            auto rate = [&](Counter c)
            { return static_cast<double>(counters[c]) * perSecond; };

            const uint64_t cycles = eeCycle - lastCycle;
            const double speed = static_cast<double>(cycles) / kEeHz / seconds * 100.0;
            g_lastSpeedTenths.store(static_cast<int64_t>(speed * 10.0 + 0.5), std::memory_order_relaxed);

            double gameThreadMs = 0.0;
            for (uint32_t i = Vu1; i <= MtvuWait; ++i)
                gameThreadMs += msPerSecond(buckets[i]);
            // VU1 thread (ps2_mtvu): busy = everything but waiting for work
            const bool vuThread = ps2mtvu::active();
            const double vuThreadBusyMs = msPerSecond(buckets[VuThreadVu1]) + msPerSecond(buckets[VuThreadVif1]) +
                                          msPerSecond(buckets[VuThreadGs]) + msPerSecond(buckets[VuThreadGpu]) +
                                          msPerSecond(buckets[VuThreadOther]);
            const ps2mtvu::Stats mtvu = ps2mtvu::takeStats();
            const double eeCodeMs = std::max(0.0, 1000.0 - gameThreadMs);

            double logicRate = -1.0;
            if (tickAddress() != 0u && rdram != nullptr)
            {
                uint32_t tick = 0;
                std::memcpy(&tick, rdram + (tickAddress() & 0x01FFFFFCu), sizeof(tick));
                if (haveTick)
                    logicRate = static_cast<double>(tick - lastTick) * perSecond;
                lastTick = tick;
                haveTick = true;
            }

            // CPU time per registered thread
            const uint64_t processNs = processCpuNs();
            const double processCores = static_cast<double>(processNs - lastProcessNs) / static_cast<double>(interval);
            lastProcessNs = processNs;
            double gameCores = 0.0, windowCores = 0.0, rasterCores = 0.0, otherCores = 0.0;
            unsigned rasterThreads = 0;
            {
                std::lock_guard<std::mutex> lock(g_threadMutex);
                for (ThreadEntry &entry : g_threads)
                {
                    const uint64_t cpu = threadCpuNs(entry);
                    entry.intervalNs = cpu - entry.lastNs;
                    entry.lastNs = cpu;
                    const double cores = static_cast<double>(entry.intervalNs) / static_cast<double>(interval);
                    if (entry.name.rfind("game", 0) == 0)
                        gameCores += cores;
                    else if (entry.name.rfind("window", 0) == 0)
                        windowCores += cores;
                    else if (entry.name.rfind("raster", 0) == 0)
                    {
                        rasterCores += cores;
                        ++rasterThreads;
                    }
                    else
                        otherCores += cores;
                }
            }
            const double unaccountedCores = std::max(0.0, processCores - gameCores - windowCores - rasterCores - otherCores);

            RendererInfo renderer;
            uint32_t gameW, gameH, windowW, windowH;
            {
                std::lock_guard<std::mutex> lock(g_infoMutex);
                renderer = g_renderer;
                gameW = g_gameW;
                gameH = g_gameH;
                windowW = g_windowW;
                windowH = g_windowH;
            }

            const double tSeconds = nsToSeconds(now) - g_session.startSeconds;
            const double windowFps = rate(MainFrames);
            const double worstFrameMs = static_cast<double>(counters[MaxFrameNs]) / 1e6;

            // ---- verdict: what limits speed
            struct Cost
            {
                uint32_t bucket;
                double ms;
            };
            Cost costs[] = {{Vu1, msPerSecond(buckets[Vu1])}, {Vu0, msPerSecond(buckets[Vu0])}, {Vif1, msPerSecond(buckets[Vif1])},
                            {GsPacket, msPerSecond(buckets[GsPacket])}, {GsRaster, msPerSecond(buckets[GsRaster])},
                            {GpuFrame, msPerSecond(buckets[GpuFrame])}, {GpuSync, msPerSecond(buckets[GpuSync])}, {Iop, msPerSecond(buckets[Iop])},
                            {BucketCount, eeCodeMs}};
            std::sort(std::begin(costs), std::end(costs), [](const Cost &a, const Cost &b)
                      { return a.ms > b.ms; });
            auto costLabel = [](const Cost &c)
            { return c.bucket == BucketCount ? "EE game code + scheduling" : bucketLabel(c.bucket); };
            const double waitMs = msPerSecond(buckets[IdleWait]) + msPerSecond(buckets[PaceWait]);
            const double mtvuWaitMs = msPerSecond(buckets[MtvuWait]);
            // Paused by the F2 drawer for any part of it: the game stood still on purpose, so this
            // interval says so and stays out of the session averages (its speed would look low).
            const bool pausedInterval = pausedFrames != 0u;
            char verdict[512];
            if (pausedInterval)
            {
                std::snprintf(verdict, sizeof(verdict), pausedFrames * 10u >= framesThisInterval * 9u
                                                            ? "PAUSED - the F2 settings menu is open or the window is in the background, so the game is stopped (not counted in the averages)"
                                                            : "PAUSED part of the time - the F2 settings menu was open or the window was in the background (not counted in the averages)");
            }
            else if (speed < 97.0 && vuThread && mtvuWaitMs > 150.0)
            {
                // The game thread keeps waiting for the VU1 thread: that thread is the limit.
                std::snprintf(verdict, sizeof(verdict),
                              "SLOW (%.0f%%) - held back by the VU1 thread (%.0f ms/s busy: VU1 %.0f, VIF1 %.0f, GS %.0f); the game thread waited %.0f ms/s for it",
                              speed, vuThreadBusyMs, msPerSecond(buckets[VuThreadVu1]), msPerSecond(buckets[VuThreadVif1]),
                              msPerSecond(buckets[VuThreadGs]), mtvuWaitMs);
            }
            else if (speed >= 97.0)
            {
                std::snprintf(verdict, sizeof(verdict), "OK - full speed. Busiest job: %s (%.0f ms/s)", costLabel(costs[0]), costs[0].ms);
            }
            else if (waitMs > 250.0)
            {
                std::snprintf(verdict, sizeof(verdict),
                              "SLOW (%.0f%%) but the game thread sat idle %.0f ms/s - limited by timing/waiting, not by CPU power",
                              speed, waitMs);
            }
            else
            {
                std::snprintf(verdict, sizeof(verdict), "SLOW (%.0f%%) - held back by %s: %.0f ms/s of the game thread; next: %s %.0f ms/s",
                              speed, costLabel(costs[0]), costs[0].ms, costLabel(costs[1]), costs[1].ms);
            }

            // ---- performance report
            char logicText[48] = "";
            if (logicRate >= 0.0)
                std::snprintf(logicText, sizeof(logicText), " | logic %.1f/s", logicRate);
            char l1[768], l2[768], l3[768], l4[768], l5[640];
            std::snprintf(l1, sizeof(l1),
                          "[%7.1fs] speed %3.0f%%%s | window %.1f fps, slowest frame %.1f ms, %llu frames >25 ms | PS2 flips %.1f/s",
                          tSeconds, speed, logicText, windowFps, worstFrameMs,
                          static_cast<unsigned long long>(counters[SlowFrames]), rate(DisplayFlips));
            std::snprintf(l2, sizeof(l2),
                          "           game thread ms/s: vu1 %.0f | vu0 %.0f | vif1 %.0f | gs-parse %.0f | gs-draw %.0f | gpu-handoff %.0f | gpu-wait %.0f | iop %.0f | ee-code+other %.0f | idle %.0f | pacing %.0f%s",
                          msPerSecond(buckets[Vu1]), msPerSecond(buckets[Vu0]), msPerSecond(buckets[Vif1]),
                          msPerSecond(buckets[GsPacket]), msPerSecond(buckets[GsRaster]), msPerSecond(buckets[GpuFrame]),
                          msPerSecond(buckets[GpuSync]), msPerSecond(buckets[Iop]), eeCodeMs,
                          msPerSecond(buckets[IdleWait]), msPerSecond(buckets[PaceWait]),
                          vuThread ? (std::string(" | waiting for VU1 thread ") + std::to_string(static_cast<int>(mtvuWaitMs + 0.5))).c_str() : "");
            char l7[640] = "";
            if (vuThread)
            {
                const double perSecondStats = perSecond;
                std::snprintf(l7, sizeof(l7),
                              "           VU1 thread ms/s: vu1 %.0f | vif1 %.0f | gs-feed %.0f | gpu-handoff %.0f | other %.0f | idle %.0f | handed over %.0f tasks/s (%.1f MB/s), woke %.0f/s | game thread waits: catch-up %.0f/s (VU1 memory %.0f, GIF_STAT %.0f, other %.0f), frame-ahead %.0f ms/s, queue-full %.0f ms/s",
                              msPerSecond(buckets[VuThreadVu1]), msPerSecond(buckets[VuThreadVif1]), msPerSecond(buckets[VuThreadGs]),
                              msPerSecond(buckets[VuThreadGpu]), msPerSecond(buckets[VuThreadOther]), msPerSecond(buckets[VuThreadIdle]),
                              static_cast<double>(mtvu.tasks) * perSecondStats, static_cast<double>(mtvu.queuedBytes) * perSecondStats / 1e6,
                              static_cast<double>(mtvu.wakeups) * perSecondStats,
                              static_cast<double>(mtvu.syncs[0] + mtvu.syncs[1] + mtvu.syncs[2]) * perSecondStats,
                              static_cast<double>(mtvu.syncs[0]) * perSecondStats, static_cast<double>(mtvu.syncs[1]) * perSecondStats,
                              static_cast<double>(mtvu.syncs[2]) * perSecondStats,
                              static_cast<double>(mtvu.frameWaitNs) * perSecondStats / 1e6, static_cast<double>(mtvu.fullWaitNs) * perSecondStats / 1e6);
            }
            char li[400] = "";
            {
                static ps2x::iop::IopThreadStats lastIop{};
                const ps2x::iop::IopThreadStats iop = ps2x::iop::iopThreadStats();
                if (iop.running)
                    std::snprintf(li, sizeof(li),
                                  "           IOP thread ms/s: busy %.0f (of it %.0f while the game waited) in %.0f pieces/s | game thread caught it up %.0f times/s (waited %.1f ms/s, ran %.1f ms/s of it itself), it was too far behind %.0f times/s | IOP writes to the EE side %.0f/s",
                                  msPerSecond(iop.busyNs - lastIop.busyNs), msPerSecond(iop.aheadNs - lastIop.aheadNs),
                                  static_cast<double>(iop.batches - lastIop.batches) * perSecond,
                                  static_cast<double>(iop.syncs - lastIop.syncs) * perSecond, msPerSecond(iop.syncWaitNs - lastIop.syncWaitNs),
                                  msPerSecond(iop.inlineNs - lastIop.inlineNs), static_cast<double>(iop.lagCatchUps - lastIop.lagCatchUps) * perSecond,
                                  static_cast<double>(iop.effects - lastIop.effects) * perSecond);
                lastIop = iop;
            }
            char l8[256] = "";
            if (g_feedSeen.load(std::memory_order_relaxed))
            {
                const double parseMs = msPerSecond(feed[0]), handoffMs = msPerSecond(feed[1]), otherMs = msPerSecond(feed[2]);
                std::snprintf(l8, sizeof(l8), "           GS feed thread ms/s: gs-parse %.0f | gpu-handoff %.0f | other %.0f | idle %.0f",
                              parseMs, handoffMs, otherMs, std::max(0.0, 1000.0 - parseMs - handoffMs - otherMs));
            }
            std::snprintf(l3, sizeof(l3),
                          "           window thread ms/s: get-frame %.0f | gs-draw %.0f | upload %.0f | draw+present %.0f    VU: %.2fM instruction pairs/s (VU1 interpreted %.2fM/s)",
                          msPerSecond(buckets[MainLatch]), msPerSecond(buckets[MainRaster]), msPerSecond(buckets[MainUpload]),
                          msPerSecond(buckets[MainEndDrawing]), rate(VuPairs) / 1e6, rate(VuPairsInterpreted) / 1e6);
            const double ramMb = static_cast<double>(processMemoryBytes()) / (1024.0 * 1024.0);
            std::snprintf(l4, sizeof(l4),
                          "           CPU cores in use: whole game %.2f (of %u) = game thread %.2f + window %.2f + raster workers %.2f (%u threads) + other %.2f | RAM in use %.0f MB",
                          processCores, std::max(1u, std::thread::hardware_concurrency()), gameCores, windowCores, rasterCores,
                          rasterThreads, otherCores + unaccountedCores, ramMb);
            // Sound: how full the buffer runs and anything that would be heard as a crackle.
            char la[512];
            {
                static ps2x::iop::AudioStats last{};
                const ps2x::iop::AudioStats now = ps2x::iop::audioStats();
                const double framesToMs = 1000.0 / 48000.0;
                std::snprintf(la, sizeof(la),
                              "           sound: game made %.2fk frames/s, %.0f ms buffered, tempo %+.2f%% (time-stretched %.0f%%) | silence %.0f ms/s (%llu dry-outs) | dropped %.0f ms/s | "
                              "clipped %.0f samples/s | sound card asked %.0f times/s",
                              static_cast<double>(now.pushedFrames - last.pushedFrames) * perSecond / 1000.0,
                              static_cast<double>(now.bufferedFrames) * framesToMs, now.rateTenthsPermille / 100.0,
                              now.pulledFrames > last.pulledFrames
                                  ? 100.0 * static_cast<double>(now.stretchedFrames - last.stretchedFrames) / static_cast<double>(now.pulledFrames - last.pulledFrames)
                                  : 0.0,
                              static_cast<double>(now.silentFrames - last.silentFrames) * framesToMs * perSecond,
                              static_cast<unsigned long long>(now.underruns - last.underruns),
                              static_cast<double>(now.droppedFrames - last.droppedFrames) * framesToMs * perSecond,
                              static_cast<double>(now.clippedSamples - last.clippedSamples) * perSecond,
                              static_cast<double>(now.callbacks - last.callbacks) * perSecond);
                last = now;
            }
            // Picture: the size frames are scaled to and what reaches the screen.
            char lp[256];
            {
                int pw = 0, ph = 0;
                ps2present::pictureSize(pw, ph);
                std::snprintf(lp, sizeof(lp), "           picture: scaled to %d x %d | shown %.0f fps%s", pw, ph, ps2present::shownFps(),
                              ps2present::interpolating() ? " (with in-between frames)" : "");
            }
            const std::string threadsText = busiestThreads(interval);
            char l6[900];
            std::snprintf(l6, sizeof(l6), "           threads: %s", threadsText.c_str());
            std::snprintf(l5, sizeof(l5), "           -> %s", verdict);

            // ---- graphics report
            const bool gpu = renderer.gpu;
            char g1[768], g2[768], g3[768], g4[768], g5[640];
            std::snprintf(g1, sizeof(g1),
                          "[%7.1fs] GS renderer: %s | PS2 picture %ux%u -> window %ux%u | window %.1f fps, new PS2 frames %.1f/s",
                          tSeconds, gpu ? "GPU (Vulkan)" : "CPU (software)", gameW, gameH, windowW, windowH, windowFps, rate(DisplayFlips));
            if (gpu)
            {
                std::snprintf(g2, sizeof(g2),
                              "           GS input: %s packets/s (%.1f MB/s) | primitives drawn %s/s in %s render passes/s | %s VRAM copies/s, %s palette updates/s",
                              formatCount(rate(GifPackets)).c_str(), rate(GifBytes) / 1e6, formatCount(rate(GpuPrimitives)).c_str(),
                              formatCount(rate(GpuRenderPasses)).c_str(), formatCount(rate(GpuCopies)).c_str(),
                              formatCount(rate(GpuPaletteUpdates)).c_str());
                std::snprintf(g3, sizeof(g3),
                              "           frames: %.1f scanouts/s, %.1f read back to the window/s, %.1f skipped (GPU still busy)/s, %s GS->CPU reads/s (%s answered from the frame before) | video memory in use %.0f MB of %.0f MB",
                              rate(GpuFrames), rate(GpuReadbacks), rate(GpuDroppedReadbacks), formatCount(rate(LocalToHostReads)).c_str(),
                              formatCount(rate(GsDownloadsPredicted)).c_str(),
                              static_cast<double>(g_gauges[GpuMemoryUsedBytes].load(std::memory_order_relaxed)) / (1024.0 * 1024.0),
                              static_cast<double>(g_gauges[GpuMemoryBudgetBytes].load(std::memory_order_relaxed)) / (1024.0 * 1024.0));
                std::snprintf(g4, sizeof(g4),
                              "           work: GPU busy %.1f ms/s (%.1f%% of the GPU) | CPU: packet parse %.0f ms/s, frame hand-off %.0f ms/s, waiting for GPU %.0f ms/s | window upload %.0f ms/s",
                              static_cast<double>(counters[GpuBusyNs]) / 1e6 * perSecond,
                              static_cast<double>(counters[GpuBusyNs]) / 1e7 * perSecond,
                              msPerSecond(buckets[GsPacket]), msPerSecond(buckets[GpuFrame]), msPerSecond(buckets[GpuSync]),
                              msPerSecond(buckets[MainUpload]));
                std::snprintf(g5, sizeof(g5), "           -> heavy lifting: GPU. The CPU only parses GS packets (%.0f ms/s) and hands frames over.",
                              msPerSecond(buckets[GsPacket]) + msPerSecond(buckets[GpuFrame]));
            }
            else
            {
                std::snprintf(g2, sizeof(g2),
                              "           GS input: %s packets/s (%.1f MB/s) | primitives %s/s (triangles %s, sprites %s, lines %s, points %s) | pixels %s/s",
                              formatCount(rate(GifPackets)).c_str(), rate(GifBytes) / 1e6,
                              formatCount(rate(GsTriangles) + rate(GsSprites) + rate(GsLines) + rate(GsPoints)).c_str(),
                              formatCount(rate(GsTriangles)).c_str(), formatCount(rate(GsSprites)).c_str(),
                              formatCount(rate(GsLines)).c_str(), formatCount(rate(GsPoints)).c_str(),
                              formatCount(rate(GsPixels)).c_str());
                std::snprintf(g3, sizeof(g3),
                              "           textures: %s uploads/s (%.2f MB/s), %s CLUT loads/s, %s GS->CPU reads/s | draw-queue flushes %s/s (tex-dep %s, clut %s, image %s, other %s)",
                              formatCount(rate(TexUploads)).c_str(), rate(TexUploadBytes) / 1e6, formatCount(rate(ClutLoads)).c_str(),
                              formatCount(rate(LocalToHostReads)).c_str(), formatCount(rate(RasterFlushes)).c_str(),
                              formatCount(rate(FlushTexDependency)).c_str(), formatCount(rate(FlushClut)).c_str(),
                              formatCount(rate(FlushImage)).c_str(),
                              formatCount(std::max(0.0, rate(RasterFlushes) - rate(FlushTexDependency) - rate(FlushClut) - rate(FlushImage))).c_str());
                std::snprintf(g4, sizeof(g4),
                              "           work: CPU parse %.0f ms/s + CPU drawing %.0f ms/s on the game thread + %.2f cores of raster workers | window: upload %.0f ms/s, draw+present %.0f ms/s",
                              msPerSecond(buckets[GsPacket]), msPerSecond(buckets[GsRaster]) + msPerSecond(buckets[MainRaster]), rasterCores,
                              msPerSecond(buckets[MainUpload]), msPerSecond(buckets[MainEndDrawing]));
                std::snprintf(g5, sizeof(g5),
                              "           -> heavy lifting: CPU. The graphics card only shows the finished picture (OpenGL upload + draw).");
            }

            {
                std::lock_guard<std::mutex> lock(g_fileMutex);
                if (g_shutdown.load(std::memory_order_relaxed))
                    return; // the session summary is written and the files are closed
                ensureFilesLocked();
                if (g_perfFile)
                {
                    writeLine(g_perfFile, l1);
                    writeLine(g_perfFile, l2);
                    if (l7[0] != '\0')
                        writeLine(g_perfFile, l7);
                    if (l8[0] != '\0')
                        writeLine(g_perfFile, l8);
                    if (li[0] != '\0')
                        writeLine(g_perfFile, li);
                    writeLine(g_perfFile, l3);
                    writeLine(g_perfFile, lp);
                    writeLine(g_perfFile, la);
                    writeLine(g_perfFile, l4);
                    writeLine(g_perfFile, l6);
                    writeLine(g_perfFile, l5);
                    static uint32_t reportCount = 0;
                    if ((++reportCount % 5u) == 0u)
                    {
                        writeProfileLocked(g_perfFile, 5.0 * seconds);
                        writeVu1MissesLocked(g_perfFile);
                    }
                    std::fflush(g_perfFile);
                }
                if (g_gfxFile)
                {
                    writeLine(g_gfxFile, g1);
                    writeLine(g_gfxFile, g2);
                    writeLine(g_gfxFile, g3);
                    writeLine(g_gfxFile, g4);
                    writeLine(g_gfxFile, g5);
                    std::fflush(g_gfxFile);
                }
            }

            if (perfMode() >= 2)
            {
                std::fprintf(stderr, "[perf] %s\n[perf] %s\n[perf] %s\n[perf] %s\n[perf] %s\n[perf] %s\n", l1, l2, l3, l4, l6, l5);
                if (l7[0] != '\0')
                    std::fprintf(stderr, "[perf] %s\n", l7);
                std::fprintf(stderr,
                             "[perf]   idle waits=%.0f/s asked=%.1fms/s over=%.1fms/s max-over=%.2fms | pace waits=%.0f/s asked=%.1fms/s over=%.1fms/s max-over=%.2fms | gif=%.0f/s raster flushes=%.0f/s pool=%.0f/s\n",
                             rate(IdleWaits), msPerSecond(counters[IdleRequestedNs]),
                             msPerSecond(counters[IdleOvershootNs]), static_cast<double>(counters[IdleOvershootMaxNs]) / 1e6,
                             rate(PaceWaits), msPerSecond(counters[PaceRequestedNs]),
                             msPerSecond(counters[PaceOvershootNs]), static_cast<double>(counters[PaceOvershootMaxNs]) / 1e6,
                             rate(GifPackets), rate(RasterFlushes), rate(RasterPoolRuns));
                std::fprintf(stderr, "[gfx] %s\n[gfx] %s\n[gfx] %s\n[gfx] %s\n", g1, g2, g3, g4);
                std::fflush(stderr);
            }

            // ---- session totals (shutdown() reads them from another thread)
            std::lock_guard<std::mutex> sessionLock(g_fileMutex);
            if (pausedInterval)
            {
                g_session.pausedIntervals++;
            }
            else
            {
                g_session.intervals++;
                if (speed >= 97.0)
                    g_session.fullSpeedIntervals++;
                g_session.speedSum += speed;
                if (speed < g_session.slowestSpeed && tSeconds > 6.0)
                {
                    g_session.slowestSpeed = speed;
                    g_session.slowestAt = tSeconds;
                }
                for (uint32_t i = 0; i < BucketCount; ++i)
                    g_session.bucketMsSum[i] += msPerSecond(buckets[i]);
                g_session.processCoresSum += processCores;
                g_session.windowFpsSum += windowFps;
                g_session.slowFrames += counters[SlowFrames];
            }
        }
        catch (...)
        {
        }

        lastNs = now;
        lastCycle = eeCycle;
    }
    } // namespace

    void shutdown() noexcept
    {
        if (g_shutdown.exchange(true)) // also stops the watchdog and the report thread, logs on or off
            return;
        // The report thread checks g_shutdown at least every 100 ms; let it finish a report it is
        // writing before the session summary and before the process tears its globals down.
        for (int i = 0; i < 300 && g_reportThreadRunning.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!enabled())
            return;
        try
        {
            std::lock_guard<std::mutex> lock(g_fileMutex);
            if (!g_filesTried)
                return;
            const double n = static_cast<double>(std::max<uint64_t>(1u, g_session.intervals));
            const double duration = nsToSeconds(nowNs()) - g_session.startSeconds;
            char line[1024];
            if (g_perfFile)
            {
                writeLine(g_perfFile, "================================================================================");
                std::snprintf(line, sizeof(line), " SESSION SUMMARY  (%dm %02ds, ended %s)", static_cast<int>(duration) / 60,
                              static_cast<int>(duration) % 60, localTimeText().c_str());
                writeLine(g_perfFile, line);
                if (g_session.intervals != 0u)
                {
                    std::snprintf(line, sizeof(line), "   average speed %.0f%% | at full speed %.0f%% of the time | slowest moment %.0f%% at %.0fs",
                                  g_session.speedSum / n, 100.0 * static_cast<double>(g_session.fullSpeedIntervals) / n,
                                  g_session.slowestSpeed < 1e8 ? g_session.slowestSpeed : 0.0, g_session.slowestAt);
                    writeLine(g_perfFile, line);
                    std::snprintf(line, sizeof(line), "   average window %.1f fps, %llu frames slower than 25 ms | average CPU use %.2f cores",
                                  g_session.windowFpsSum / n, static_cast<unsigned long long>(g_session.slowFrames), g_session.processCoresSum / n);
                    writeLine(g_perfFile, line);
                    std::snprintf(line, sizeof(line),
                                  "   game thread average ms/s: vu1 %.0f | vu0 %.0f | vif1 %.0f | gs-parse %.0f | gs-draw %.0f | gpu-handoff %.0f | gpu-wait %.0f | iop %.0f | idle %.0f | pacing %.0f",
                                  g_session.bucketMsSum[Vu1] / n, g_session.bucketMsSum[Vu0] / n, g_session.bucketMsSum[Vif1] / n,
                                  g_session.bucketMsSum[GsPacket] / n, g_session.bucketMsSum[GsRaster] / n, g_session.bucketMsSum[GpuFrame] / n,
                                  g_session.bucketMsSum[GpuSync] / n, g_session.bucketMsSum[Iop] / n, g_session.bucketMsSum[IdleWait] / n,
                                  g_session.bucketMsSum[PaceWait] / n);
                    writeLine(g_perfFile, line);
                }
                if (g_session.pausedIntervals != 0u)
                {
                    std::snprintf(line, sizeof(line), "   paused in the F2 settings menu for about %llu s (left out of the numbers above)",
                                  static_cast<unsigned long long>(g_session.pausedIntervals * (kReportIntervalNs / 1000000000ull)));
                    writeLine(g_perfFile, line);
                }
                writeLine(g_perfFile, "================================================================================");
                std::fclose(g_perfFile);
                g_perfFile = nullptr;
            }
            if (g_gfxFile)
            {
                writeLine(g_gfxFile, "================================================================================");
                std::snprintf(line, sizeof(line), " SESSION END (%dm %02ds) - GS renderer was: %s on %s", static_cast<int>(duration) / 60,
                              static_cast<int>(duration) % 60, g_renderer.name.c_str(), g_renderer.device.c_str());
                writeLine(g_gfxFile, line);
                writeLine(g_gfxFile, "================================================================================");
                std::fclose(g_gfxFile);
                g_gfxFile = nullptr;
            }
        }
        catch (...)
        {
        }
    }
}
