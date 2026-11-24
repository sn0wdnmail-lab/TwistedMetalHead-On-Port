#include "ps2_capture.h"

#include "runtime/ps2_perf.h"
#include "runtime/ps2_settings.h"

#include "raylib.h"
#include "rlgl.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "external/stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace ps2capture
{
    namespace
    {
        constexpr double kInterval = 0.5;  // seconds between pictures
        constexpr size_t kKeep = 1200;     // ten minutes of pictures
        constexpr int kMaxWidth = 1280;    // larger windows are scaled down to this width
        constexpr size_t kMaxQueued = 4;   // the worker is behind: skip pictures rather than pile up
        constexpr int kJpegQuality = 82;

        struct Job
        {
            std::vector<uint8_t> rgba;
            int width = 0;
            int height = 0;
            double seconds = 0.0;
            uint64_t index = 0;
            std::filesystem::path pngPath; // set: a screenshot, written whole as PNG
        };
        std::atomic<bool> g_screenshotWanted{false};

        std::mutex g_mutex;
        std::condition_variable g_cv;
        std::deque<Job> g_jobs;
        std::thread g_worker;
        bool g_stop = false;
        std::filesystem::path g_folder;
        std::deque<std::filesystem::path> g_written;

        std::filesystem::path pathFromUtf8(const std::string &text)
        {
            return std::filesystem::path(std::u8string(text.begin(), text.end()));
        }

        void writeToFile(void *context, void *data, int size)
        {
            std::fwrite(data, 1, static_cast<size_t>(size), static_cast<FILE *>(context));
        }

        FILE *openForWrite(const std::filesystem::path &path)
        {
#if defined(_WIN32)
            return _wfopen(path.wstring().c_str(), L"wb");
#else
            return std::fopen(path.string().c_str(), "wb");
#endif
        }

        void encode(Job &job)
        {
            // Box-filter down to at most kMaxWidth pixels wide, dropping alpha for the JPEG.
            const int factor = std::max(1, (job.width + kMaxWidth - 1) / kMaxWidth);
            const int outWidth = job.width / factor;
            const int outHeight = job.height / factor;
            if (outWidth <= 0 || outHeight <= 0)
                return;
            std::vector<uint8_t> rgb(static_cast<size_t>(outWidth) * static_cast<size_t>(outHeight) * 3u);
            const int area = factor * factor;
            for (int y = 0; y < outHeight; ++y)
            {
                for (int x = 0; x < outWidth; ++x)
                {
                    int sum[3] = {0, 0, 0};
                    for (int dy = 0; dy < factor; ++dy)
                    {
                        const uint8_t *row = job.rgba.data() + (static_cast<size_t>(y * factor + dy) * static_cast<size_t>(job.width) +
                                                               static_cast<size_t>(x * factor)) * 4u;
                        for (int dx = 0; dx < factor; ++dx)
                        {
                            sum[0] += row[dx * 4 + 0];
                            sum[1] += row[dx * 4 + 1];
                            sum[2] += row[dx * 4 + 2];
                        }
                    }
                    uint8_t *out = rgb.data() + (static_cast<size_t>(y) * static_cast<size_t>(outWidth) + static_cast<size_t>(x)) * 3u;
                    out[0] = static_cast<uint8_t>(sum[0] / area);
                    out[1] = static_cast<uint8_t>(sum[1] / area);
                    out[2] = static_cast<uint8_t>(sum[2] / area);
                }
            }
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%06llu_%07.1fs.jpg", static_cast<unsigned long long>(job.index), job.seconds);
            const std::filesystem::path path = g_folder / name;
            FILE *file = openForWrite(path);
            if (!file)
                return;
            stbi_write_jpg_to_func(writeToFile, file, outWidth, outHeight, 3, rgb.data(), kJpegQuality);
            std::fclose(file);
            g_written.push_back(path);
            while (g_written.size() > kKeep)
            {
                std::error_code ignored;
                std::filesystem::remove(g_written.front(), ignored);
                g_written.pop_front();
            }
        }

        void writePng(const Job &job)
        {
            FILE *file = openForWrite(job.pngPath);
            if (!file)
                return;
            stbi_write_png_to_func(writeToFile, file, job.width, job.height, 4, job.rgba.data(), job.width * 4);
            std::fclose(file);
        }

        void workerMain()
        {
            for (;;)
            {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_cv.wait(lock, [] { return g_stop || !g_jobs.empty(); });
                    if (g_jobs.empty())
                        return; // stopping, nothing left
                    job = std::move(g_jobs.front());
                    g_jobs.pop_front();
                }
                if (!job.pngPath.empty())
                    writePng(job);
                else
                    encode(job);
            }
        }

        void startWorker()
        {
            if (!g_worker.joinable())
            {
                g_stop = false;
                g_worker = std::thread(workerMain);
            }
        }

        bool startSession()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
            std::error_code error;
            g_folder = pathFromUtf8(ps2perf::logDirectory()) / "frames" / stamp;
            std::filesystem::create_directories(g_folder, error);
            if (error)
                return false;
            g_written.clear();
            startWorker();
            ps2perf::performanceNote("Recording screenshots every %.1f s to %s", kInterval, g_folder.u8string().c_str());
            return true;
        }
    }

    void afterDraw(bool recording) noexcept
    {
        static bool active = false;
        static bool failed = false;
        static double startedAt = 0.0;
        static double nextAt = 0.0;
        static uint64_t index = 0;
        if (!recording || failed)
        {
            active = false;
            return;
        }
        try
        {
            const double now = GetTime();
            if (!active)
            {
                if (!startSession())
                {
                    failed = true;
                    return;
                }
                active = true;
                startedAt = now;
                nextAt = now;
                index = 0;
            }
            if (now < nextAt)
                return;
            nextAt += kInterval;
            if (nextAt < now)
                nextAt = now + kInterval;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_jobs.size() >= kMaxQueued)
                    return;
            }
            rlDrawRenderBatchActive(); // raylib batches draws until EndDrawing; put them on the back buffer first
            Image screen = LoadImageFromScreen();
            if (!screen.data || screen.width <= 0 || screen.height <= 0)
            {
                UnloadImage(screen);
                return;
            }
            Job job;
            job.width = screen.width;
            job.height = screen.height;
            job.seconds = now - startedAt;
            job.index = index++;
            const uint8_t *pixels = static_cast<const uint8_t *>(screen.data);
            job.rgba.assign(pixels, pixels + static_cast<size_t>(screen.width) * static_cast<size_t>(screen.height) * 4u);
            UnloadImage(screen);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_jobs.push_back(std::move(job));
            }
            g_cv.notify_one();
        }
        catch (...)
        {
            failed = true;
        }
    }

    void requestScreenshot() noexcept
    {
        g_screenshotWanted.store(true, std::memory_order_relaxed);
    }

    void afterPicture(float x, float y, float width, float height) noexcept
    {
        if (!g_screenshotWanted.exchange(false, std::memory_order_relaxed))
            return;
        try
        {
            rlDrawRenderBatchActive(); // the picture is still in raylib's batch until it is flushed
            Image screen = LoadImageFromScreen();
            if (!screen.data || screen.width <= 0 || screen.height <= 0)
            {
                UnloadImage(screen);
                ps2settings::notify("Screenshot failed: the picture could not be read");
                return;
            }
            // Just the game picture (no black bars): screen coordinates to pixels (high-DPI too).
            const float sx = static_cast<float>(screen.width) / std::max(1.0f, static_cast<float>(GetScreenWidth()));
            const float sy = static_cast<float>(screen.height) / std::max(1.0f, static_cast<float>(GetScreenHeight()));
            const int left = std::clamp(static_cast<int>(x * sx + 0.5f), 0, screen.width - 1);
            const int top = std::clamp(static_cast<int>(y * sy + 0.5f), 0, screen.height - 1);
            const int right = std::clamp(static_cast<int>((x + width) * sx + 0.5f), left + 1, screen.width);
            const int bottom = std::clamp(static_cast<int>((y + height) * sy + 0.5f), top + 1, screen.height);
            Job job;
            job.width = right - left;
            job.height = bottom - top;
            job.rgba.resize(static_cast<size_t>(job.width) * static_cast<size_t>(job.height) * 4u);
            const uint8_t *pixels = static_cast<const uint8_t *>(screen.data);
            for (int row = 0; row < job.height; ++row)
            {
                const uint8_t *src = pixels + (static_cast<size_t>(top + row) * static_cast<size_t>(screen.width) + static_cast<size_t>(left)) * 4u;
                uint8_t *dst = job.rgba.data() + static_cast<size_t>(row) * static_cast<size_t>(job.width) * 4u;
                std::memcpy(dst, src, static_cast<size_t>(job.width) * 4u);
                for (int px = 0; px < job.width; ++px)
                    dst[px * 4 + 3] = 255u; // opaque
            }
            UnloadImage(screen);

            // Screenshots\TMHO 2026-11-24 18-05-42.png next to the game (a number added if taken
            // within the same second).
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char stamp[48];
            std::strftime(stamp, sizeof(stamp), "TMHO %Y-%m-%d %H-%M-%S", &local);
            const std::filesystem::path folder = pathFromUtf8(GetApplicationDirectory()) / "Screenshots";
            std::error_code error;
            std::filesystem::create_directories(folder, error);
            if (error)
            {
                ps2settings::notify("Screenshot failed: could not create the Screenshots folder");
                return;
            }
            std::filesystem::path path = folder / (std::string(stamp) + ".png");
            for (int n = 2; std::filesystem::exists(path, error) && n < 100; ++n)
                path = folder / (std::string(stamp) + " (" + std::to_string(n) + ").png");
            job.pngPath = path;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                startWorker();
                g_jobs.push_back(std::move(job));
            }
            g_cv.notify_one();
            const std::u8string fileName = path.filename().u8string();
            const std::string shown = "Screenshot saved: Screenshots\\" + std::string(fileName.begin(), fileName.end());
            ps2settings::notify(shown.c_str());
        }
        catch (...)
        {
            ps2settings::notify("Screenshot failed");
        }
    }

    void shutdown() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_stop = true;
        }
        g_cv.notify_all();
        if (g_worker.joinable())
            g_worker.join();
    }
}
