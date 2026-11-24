// In-game settings drawer (F2), after the chosen design concept: a dark side drawer with
// Video / Audio / Game / UI tabs, an accent colour the player picks, and keyboard, controller
// and mouse control. Settings persist in settings.ini next to the executable's working directory.

#include "runtime/ps2_settings.h"
#include "tmho_game.h"
#include "ps2_movie_state.h"
#include "runtime/gs/gs_gpu_renderer.h"
#include "runtime/ps2_perf.h"
#include "runtime/ps2_launcher.h"
#include "runtime/ps2_present.h"
#include "ps2_capture.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

// raylib's own GLFW (linked into libraylib): chaining its callbacks catches presses that are
// released again before the next frame - touchpad taps, very short key presses.
extern "C"
{
    typedef struct GLFWwindow GLFWwindow;
    typedef void (*GLFWmousebuttonfun)(GLFWwindow *, int, int, int);
    typedef void (*GLFWkeyfun)(GLFWwindow *, int, int, int, int);
    GLFWwindow *glfwGetCurrentContext(void);
    GLFWmousebuttonfun glfwSetMouseButtonCallback(GLFWwindow *window, GLFWmousebuttonfun callback);
    GLFWkeyfun glfwSetKeyCallback(GLFWwindow *window, GLFWkeyfun callback);
    typedef struct GLFWmonitor GLFWmonitor;
    typedef struct GLFWvidmode
    {
        int width, height, redBits, greenBits, blueBits, refreshRate;
    } GLFWvidmode;
    GLFWmonitor **glfwGetMonitors(int *count);
    const GLFWvidmode *glfwGetVideoModes(GLFWmonitor *monitor, int *count);
}

namespace ps2settings
{
    namespace
    {
        // ---- quick taps (see above)
        GLFWmousebuttonfun g_raylibMouseButton = nullptr;
        GLFWkeyfun g_raylibKey = nullptr;
        bool g_tapClick = false;          // left button went down since the last frame
        bool g_clickThisFrame = false;    // latched once per frame in update()
        bool g_tapKeys[512] = {};         // keys that went down since the last frame
        bool g_keysThisFrame[512] = {};
        bool g_tapAltEnter = false;       // Alt+Enter went down since the last frame
        bool g_altEnterThisFrame = false;

        void onMouseButton(GLFWwindow *window, int button, int action, int mods)
        {
            if (button == 0 && action == 1)
                g_tapClick = true;
            if (g_raylibMouseButton)
                g_raylibMouseButton(window, button, action, mods);
        }

        void onKey(GLFWwindow *window, int key, int scancode, int action, int mods)
        {
            if (action == 1 && key >= 0 && key < 512)
                g_tapKeys[key] = true;
            // Keys the port itself answers, kept from raylib and the game: F11 and Alt+Enter
            // (fullscreen), F12 (screenshot; raylib's own F12 would overwrite screenshot000.png
            // every session and Ctrl+F12 would start a slow GIF recording).
            constexpr int kAlt = 0x0004; // GLFW_MOD_ALT
            if (key == KEY_ENTER && (mods & kAlt) != 0)
            {
                if (action == 1)
                    g_tapAltEnter = true;
                return;
            }
            if (key == KEY_F11 || key == KEY_F12)
                return;
            if (g_raylibKey)
                g_raylibKey(window, key, scancode, action, mods);
        }

        void installTapCapture()
        {
            static bool installed = false;
            if (installed)
                return;
            GLFWwindow *window = glfwGetCurrentContext();
            if (!window)
                return;
            installed = true;
            g_raylibMouseButton = glfwSetMouseButtonCallback(window, onMouseButton);
            g_raylibKey = glfwSetKeyCallback(window, onKey);
        }

        // Once per frame: what was pressed since the previous frame.
        void latchTaps()
        {
            g_clickThisFrame = g_tapClick || IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
            g_tapClick = false;
            for (int k = 0; k < 512; ++k)
            {
                g_keysThisFrame[k] = g_tapKeys[k];
                g_tapKeys[k] = false;
            }
            g_altEnterThisFrame = g_tapAltEnter;
            g_tapAltEnter = false;
        }

        Settings g_settings;
        std::atomic<bool> g_menuOpen{false};
        std::atomic<bool> g_paused{false};
        std::string g_iniPath = "settings.ini";
        bool g_gpuActive = false;
        bool g_gpuAvailable = false;
        std::string g_gpuDevice;
        std::string g_rendererHelp = "Vulkan draws the game on your graphics card; Software uses the CPU. Applies on the next start.";
        std::string g_setupNote;

        struct WindowSize
        {
            int width;
            int height;
            const char *label;
        };
        // Window sizes of older settings files (window_size=<index>).
        const WindowSize kWindowSizes[] = {
            {640, 480, "640 x 480"},   {960, 720, "960 x 720"},   {1280, 960, "1280 x 960"},   {1280, 720, "1280 x 720"},
            {1600, 900, "1600 x 900"}, {1920, 1080, "1920 x 1080"}, {2560, 1440, "2560 x 1440"}, {3840, 2160, "3840 x 2160"},
        };
        constexpr int kWindowSizeCount = static_cast<int>(sizeof(kWindowSizes) / sizeof(kWindowSizes[0]));

        struct Swatch
        {
            uint32_t rgb;
            const char *name;
        };
        const Swatch kSwatches[] = {
            {0x4C8DFFu, "Blue"}, {0x22C3C3u, "Teal"}, {0x2BD67Bu, "Green"}, {0xF2B705u, "Yellow"},
            {0xFF7A1Au, "Orange"}, {0xFF4D4Du, "Red"}, {0xFF5FA8u, "Pink"}, {0x9B7BFFu, "Purple"},
        };
        constexpr int kSwatchCount = 8;

        // ---------------------------------------------------------------- persistence
        void save()
        {
            std::ofstream out(g_iniPath, std::ios::trunc);
            if (!out)
                return;
            const Settings &s = g_settings;
            out << "# Twisted Metal: Head-On PC port settings (F2 in game)\n";
            out << "window_mode=" << s.windowMode << "\n";
            out << "# resolution: the game picture's lines - 0 = 480p (1x), 1 = 720p, 2 = 1080p, 3 = 1440p, 4 = 2160p\n";
            out << "resolution=" << s.resolution << "\n";
            out << "# frame_rate: 0 = 60 fps, 1 = 120 fps, 2 = the monitor's rate (in-between frames; the game keeps its speed)\n";
            out << "frame_rate=" << s.frameRate << "\n";
            out << "aspect=" << s.aspect << "\n";
            out << "filter=" << s.filter << "\n";
            out << "vsync=" << (s.vsync ? 1 : 0) << "\n";
            out << "show_fps=" << (s.showFps ? 1 : 0) << "\n";
            out << "record_frames=" << (s.recordFrames ? 1 : 0) << "\n";
            out << "fov=" << s.fovDegrees << "\n";
            out << "camera_distance=" << s.cameraDistance << "\n";
            out << "# battle HUD layout (F2 > HUD): offsets in 0.1% of the picture, sizes in %\n";
            out << "hud_radar_x=" << s.hudRadarX << "\n";
            out << "hud_radar_y=" << s.hudRadarY << "\n";
            out << "hud_radar_scale=" << s.hudRadarScale << "\n";
            out << "hud_weapon_x=" << s.hudWeaponX << "\n";
            out << "hud_weapon_y=" << s.hudWeaponY << "\n";
            out << "hud_weapon_scale=" << s.hudWeaponScale << "\n";
            if (s.cpuLimitPercent > 0)
            {
                out << "# testing: the whole game may use at most this % of all CPU cores together (0 = no limit)\n";
                out << "cpu_limit_percent=" << s.cpuLimitPercent << "\n";
            }
            if (s.gpuFlushPrimitives > 0)
                out << "gpu_flush_primitives=" << s.gpuFlushPrimitives << "\n";
            if (s.gpuTimestamps != 1)
                out << "gpu_timestamps=" << s.gpuTimestamps << "\n";
            if (s.gpuFeedBatch != 1)
                out << "gpu_feed_batch=" << s.gpuFeedBatch << "\n";
            if (s.audioDump != 0)
                out << "audio_dump=" << s.audioDump << "\n";
            if (s.gsDownloadPrediction != 1)
                out << "gs_download_prediction=" << s.gsDownloadPrediction << "\n";
            out << "master_volume=" << s.masterVolume << "\n";
            out << "mute_unfocused=" << (s.muteUnfocused ? 1 : 0) << "\n";
            out << "pause_in_menu=" << (s.pauseInMenu ? 1 : 0) << "\n";
            out << "pause_unfocused=" << (s.pauseUnfocused ? 1 : 0) << "\n";
            out << "accent=" << s.accentIndex << "\n";
            char hex[16];
            std::snprintf(hex, sizeof(hex), "%06X", s.accentCustom & 0xFFFFFFu);
            out << "accent_custom=" << hex << "\n";
            out << "menu_opacity=" << s.menuOpacity << "\n";
            out << "menu_side=" << s.menuSide << "\n";
            out << "# renderer: 0 = Vulkan GPU, 1 = software (CPU). Takes effect on the next start.\n";
            out << "renderer=" << s.renderer << "\n";
            out << "# vu1_thread (GPU renderer): 1 = VIF1 and VU1 microcode run on their own CPU core, 0 = on the game thread. Next start.\n";
            out << "vu1_thread=" << (s.vu1Thread ? 1 : 0) << "\n";
        }

        void load()
        {
            std::ifstream in(g_iniPath);
            if (!in)
                return;
            std::string line;
            Settings &s = g_settings;
            while (std::getline(in, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;
                const size_t eq = line.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key = line.substr(0, eq);
                const std::string value = line.substr(eq + 1);
                const int number = std::atoi(value.c_str());
                if (key == "window_mode") s.windowMode = std::clamp(number, 0, 2);
                else if (key == "window_size")
                {
                    // Older files: a window size index; becomes the resolution unless one is set.
                    s.windowSize = std::clamp(number, 0, kWindowSizeCount - 1);
                    if (s.resWidth == 0 && s.resHeight == 0)
                    {
                        s.resWidth = kWindowSizes[s.windowSize].width;
                        s.resHeight = kWindowSizes[s.windowSize].height;
                    }
                }
                else if (key == "res_width") s.resWidth = std::clamp(number, 0, 7680);
                else if (key == "res_height") s.resHeight = std::clamp(number, 0, 4320);
                else if (key == "aspect") s.aspect = std::clamp(number, 0, 2);
                else if (key == "filter") s.filter = std::clamp(number, 0, 1);
                else if (key == "vsync") s.vsync = number != 0;
                else if (key == "show_fps") s.showFps = number != 0;
                else if (key == "record_frames") s.recordFrames = number != 0;
                else if (key == "fov") s.fovDegrees = number <= 0 ? 0 : std::clamp(number, 50, 120);
                else if (key == "camera_distance") s.cameraDistance = std::clamp(number, 50, 200);
                else if (key == "hud_radar_x") s.hudRadarX = std::clamp(number, -1000, 1000);
                else if (key == "hud_radar_y") s.hudRadarY = std::clamp(number, -1000, 1000);
                else if (key == "hud_radar_scale") s.hudRadarScale = std::clamp(number, 40, 250);
                else if (key == "hud_weapon_x") s.hudWeaponX = std::clamp(number, -1000, 1000);
                else if (key == "hud_weapon_y") s.hudWeaponY = std::clamp(number, -1000, 1000);
                else if (key == "hud_weapon_scale") s.hudWeaponScale = std::clamp(number, 40, 250);
                else if (key == "cpu_limit_percent") s.cpuLimitPercent = std::clamp(number, 0, 100);
                else if (key == "gpu_flush_primitives") s.gpuFlushPrimitives = std::clamp(number, 0, 1 << 20);
                else if (key == "gpu_timestamps") s.gpuTimestamps = std::clamp(number, 0, 1);
                else if (key == "gpu_feed_batch") s.gpuFeedBatch = std::clamp(number, 0, 1);
                else if (key == "audio_dump") s.audioDump = std::clamp(number, 0, 1);
                else if (key == "gs_download_prediction") s.gsDownloadPrediction = std::clamp(number, 0, 1);
                else if (key == "master_volume") s.masterVolume = std::clamp(number, 0, 100);
                else if (key == "mute_unfocused") s.muteUnfocused = number != 0;
                else if (key == "pause_in_menu") s.pauseInMenu = number != 0;
                else if (key == "pause_unfocused") s.pauseUnfocused = number != 0;
                else if (key == "accent") s.accentIndex = std::clamp(number, 0, kSwatchCount);
                else if (key == "accent_custom") s.accentCustom = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 16)) & 0xFFFFFFu;
                else if (key == "menu_opacity") s.menuOpacity = std::clamp(number, 60, 100);
                else if (key == "menu_side") s.menuSide = std::clamp(number, 0, 1);
                else if (key == "renderer") s.renderer = std::clamp(number, 0, 1);
                else if (key == "internal_resolution") s.internalScale = number <= 0 ? 0 : std::clamp(number + 1, 2, 3); // older files
                else if (key == "internal_res") s.internalScale = std::clamp(number, 0, 3);
                else if (key == "vu1_thread") s.vu1Thread = number != 0;
                else if (key == "resolution") s.resolution = std::clamp(number, 0, 4);
                else if (key == "frame_rate") s.frameRate = std::clamp(number, 0, 2);
            }
        }

        // ---------------------------------------------------------------- applying
        int g_appliedWindowMode = 0;
        int g_appliedAspectForWindow = -1;
        int g_appliedSwapInterval = -1; // 0 or 1 as last given to the driver
        bool g_compositorSync = false;  // V-Sync done by waiting for the desktop compositor (Windows)
        int g_appliedFilter = -1;
        Texture2D *g_frameTexture = nullptr;

        void centerWindow(int width, int height)
        {
            const int monitor = GetCurrentMonitor();
            const int mw = GetMonitorWidth(monitor);
            const int mh = GetMonitorHeight(monitor);
            const Vector2 origin = GetMonitorPosition(monitor);
            SetWindowPosition(static_cast<int>(origin.x) + std::max(0, (mw - width) / 2),
                              static_cast<int>(origin.y) + std::max(0, (mh - height) / 2));
        }

        bool isWideFamily(int width, int height)
        {
            return height > 0 && static_cast<float>(width) / static_cast<float>(height) > 1.5f;
        }

        // The window size for windowed mode: the chosen resolution in the picture's shape, shrunk to
        // leave room for the title bar and the taskbar when the monitor is smaller.
        void windowedSize(int &width, int &height)
        {
            const Settings &s = g_settings;
            height = resolutionLines(s.resolution);
            const float aspect = s.aspect == 0 ? 4.0f / 3.0f : 16.0f / 9.0f;
            width = static_cast<int>(std::lround(static_cast<float>(height) * aspect));
            const int monitor = GetCurrentMonitor();
            const int mw = GetMonitorWidth(monitor);
            const int mh = GetMonitorHeight(monitor);
            if (mw > 0 && mh > 0)
            {
                const float maxWidth = static_cast<float>(mw) * 0.95f;
                const float maxHeight = static_cast<float>(mh) * 0.88f;
                if (static_cast<float>(width) > maxWidth || static_cast<float>(height) > maxHeight)
                {
                    const float scale = std::min(maxWidth / static_cast<float>(width), maxHeight / static_cast<float>(height));
                    width = static_cast<int>(static_cast<float>(width) * scale);
                    height = static_cast<int>(static_cast<float>(height) * scale);
                }
            }
        }

        int g_appliedResolution = -1;

        // Windowed: a window the size of the chosen resolution. Borderless and full screen: the
        // monitor's own size and mode (the picture is scaled to it).
        void applyWindowMode()
        {
            const Settings &s = g_settings;
            const bool windowedChanged = s.windowMode == 0 &&
                                         (s.resolution != g_appliedResolution || s.aspect != g_appliedAspectForWindow);
            if (s.windowMode == g_appliedWindowMode && !windowedChanged)
                return;
            if (s.windowMode == g_appliedWindowMode)
            {
                int width = 0, height = 0;
                windowedSize(width, height);
                SetWindowSize(width, height);
                centerWindow(width, height);
                g_appliedResolution = s.resolution;
                g_appliedAspectForWindow = s.aspect;
                return;
            }
            const int monitor = GetCurrentMonitor();
            const int fullWidth = GetMonitorWidth(monitor);
            const int fullHeight = GetMonitorHeight(monitor);
            // Leave the current mode first.
            if (IsWindowFullscreen())
                ToggleFullscreen();
            if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
                ToggleBorderlessWindowed();
            if (IsWindowState(FLAG_WINDOW_UNDECORATED))
                ClearWindowState(FLAG_WINDOW_UNDECORATED); // set below for full screen
            if (s.windowMode == 0)
            {
                int width = 0, height = 0;
                windowedSize(width, height);
                SetWindowSize(width, height);
                centerWindow(width, height);
            }
            else if (s.windowMode == 1)
            {
                ToggleBorderlessWindowed();
            }
            else
            {
                // Exclusive full screen uses the window size as the display mode. Windows keeps a
                // window with a title bar smaller than the screen (1920 x 1080 came out 1920 x 1060,
                // leaving a strip below the picture), so the title bar goes first.
                SetWindowState(FLAG_WINDOW_UNDECORATED);
                SetWindowSize(fullWidth, fullHeight);
                ToggleFullscreen();
            }
            g_appliedWindowMode = s.windowMode;
            g_appliedResolution = s.resolution;
            g_appliedAspectForWindow = s.aspect;
        }

        // What the GPU renderer draws at for the chosen resolution.
        void applyInternalResolution()
        {
            if (!g_gpuAvailable || g_settings.renderer != 0)
                return;
            const uint32_t rate = superSamplingForResolution(g_settings.resolution);
            static uint32_t requested = 0u;
            if (rate != requested)
            {
                requested = rate;
                ps2gpu::requestSuperSampling(rate);
            }
        }

        // V-Sync waits for the monitor's refresh, so it only makes sense when the window draws at
        // least as often as the monitor refreshes (Monitor frame rate, or 60 fps on a 60 Hz screen);
        // waiting for it at 120 fps on a 170 Hz screen would drop the frame rate to 85.
        // Windowed and borderless windows on Windows wait for the desktop compositor instead of
        // asking the driver: NVIDIA's settings can switch the driver's V-Sync off for every game,
        // and then the window ran at thousands of frames a second.
        void applyVsync()
        {
            const int refresh = GetMonitorRefreshRate(GetCurrentMonitor());
            const int target = ps2present::targetFps();
            const bool fastEnough = target == 0 || refresh <= 0 || target * 100 >= refresh * 95;
            const bool sync = g_settings.vsync && fastEnough;
#if defined(_WIN32)
            const bool compositor = sync && g_settings.windowMode != 2;
#else
            const bool compositor = false;
#endif
            g_compositorSync = compositor;
            const int interval = (sync && !compositor) ? 1 : 0;
            if (interval == g_appliedSwapInterval)
                return;
            if (interval)
                SetWindowState(FLAG_VSYNC_HINT);
            else
                ClearWindowState(FLAG_VSYNC_HINT);
            g_appliedSwapInterval = interval;
        }

        void applyFilter()
        {
            // The GPU renderer recreates the texture when the picture size changes: re-apply then.
            static unsigned appliedTexture = 0u;
            if (!g_frameTexture || g_frameTexture->id == 0 ||
                (g_settings.filter == g_appliedFilter && g_frameTexture->id == appliedTexture))
                return;
            SetTextureFilter(*g_frameTexture, g_settings.filter ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);
            g_appliedFilter = g_settings.filter;
            appliedTexture = g_frameTexture->id;
        }

        void applyVolume()
        {
            const bool muted = g_settings.muteUnfocused && !IsWindowFocused();
            static float applied = -1.0f;
            const float volume = muted ? 0.0f : static_cast<float>(g_settings.masterVolume) / 100.0f;
            if (volume != applied)
            {
                SetMasterVolume(volume);
                applied = volume;
            }
        }

        // ---------------------------------------------------------------- look
        Font g_font{};
        Font g_fontBold{};
        bool g_fontsLoaded = false;

        void loadFonts()
        {
            if (g_fontsLoaded)
                return;
            g_fontsLoaded = true;
            std::vector<int> codepoints;
            for (int c = 32; c < 127; ++c)
                codepoints.push_back(c);
            for (int c : {0x00B0, 0x00B7, 0x00D7, 0x2022, 0x2039, 0x203A, 0x2190, 0x2191, 0x2192, 0x2193, 0x2014})
                codepoints.push_back(c);
            const char *regular[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
            const char *bold[] = {"C:/Windows/Fonts/segoeuib.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"};
            for (const char *path : regular)
            {
                if (FileExists(path))
                {
                    g_font = LoadFontEx(path, 48, codepoints.data(), static_cast<int>(codepoints.size()));
                    break;
                }
            }
            for (const char *path : bold)
            {
                if (FileExists(path))
                {
                    g_fontBold = LoadFontEx(path, 48, codepoints.data(), static_cast<int>(codepoints.size()));
                    break;
                }
            }
            if (g_font.texture.id == 0)
                g_font = GetFontDefault();
            if (g_fontBold.texture.id == 0)
                g_fontBold = g_font;
            SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
            SetTextureFilter(g_fontBold.texture, TEXTURE_FILTER_BILINEAR);
        }

        Color rgb(uint32_t value, unsigned char alpha = 255)
        {
            return Color{static_cast<unsigned char>((value >> 16) & 0xFF), static_cast<unsigned char>((value >> 8) & 0xFF),
                         static_cast<unsigned char>(value & 0xFF), alpha};
        }

        uint32_t accentRgb()
        {
            const Settings &s = g_settings;
            return s.accentIndex < kSwatchCount ? kSwatches[s.accentIndex].rgb : (s.accentCustom & 0xFFFFFFu);
        }

        // Dark or white text on top of the accent colour, by luminance.
        Color inkFor(uint32_t value)
        {
            const float r = static_cast<float>((value >> 16) & 0xFF) / 255.0f;
            const float g = static_cast<float>((value >> 8) & 0xFF) / 255.0f;
            const float b = static_cast<float>(value & 0xFF) / 255.0f;
            const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            return luminance > 0.55f ? rgb(0x0E1116) : WHITE;
        }

        void text(const char *value, float x, float y, float size, Color color, bool bold = false)
        {
            DrawTextEx(bold ? g_fontBold : g_font, value, Vector2{std::round(x), std::round(y)}, size, size * 0.02f, color);
        }

        float textWidth(const char *value, float size, bool bold = false)
        {
            return MeasureTextEx(bold ? g_fontBold : g_font, value, size, size * 0.02f).x;
        }

        // ---------------------------------------------------------------- rows
        enum class RowKind
        {
            Choice,
            Toggle,
            Slider,
            Swatches,
            HexColor,
            Action,
        };

        struct Row
        {
            const char *label;
            const char *help;
            RowKind kind;
            std::function<int()> getValue;            // Choice/Slider/Swatches: current value
            std::function<void(int)> setValue;        // apply a new value
            std::vector<std::string> choices;         // Choice
            int minimum = 0;
            int maximum = 100;
            int step = 1;
            const char *suffix = "";
            std::function<bool()> enabled = [] { return true; };
            const char *disabledNote = "";
            const char *actionLabel = "Reset"; // Action: the button's text
        };

        int g_tab = 0;
        int g_focus = 0;
        double g_quitArmedUntil = 0.0;          // "Quit game" pressed once: a second press before this quits
        std::atomic<bool> g_quitRequested{false};
        bool g_editingHex = false;
        std::string g_hexBuffer;
        bool g_draggingSlider = false;

        const char *const kTabs[] = {"VIDEO", "AUDIO", "GAME", "HUD", "UI"};
        constexpr int kTabCount = 5;

        // ---------------------------------------------------------------- HUD editor state
        bool g_hudEditor = false;          // the layout editor covers the screen instead of the drawer
        int g_hudSelected = 0;             // tmho::HudPiece being edited
        int g_hudDrag = 0;                 // 0 none, 1 moving, 2 resizing
        Vector2 g_hudDragStart{};
        int g_hudStartX = 0, g_hudStartY = 0, g_hudStartScale = 100;
        // Until when (GetTime()) the paused game may run after a HUD layout change, so it draws its
        // HUD in the new place: a few game frames, whatever the monitor's refresh rate.
        std::atomic<double> g_hudStepUntil{0.0};
        void stepHudFrames() { g_hudStepUntil.store(GetTime() + 0.15, std::memory_order_relaxed); }
        bool g_hudDirty = false;           // layout changed since the last save
        Texture2D g_hudPreview{};          // the last battle's picture, when no battle is running
        bool g_hudPreviewLive = false;     // the game picture behind the editor shows the HUD now
        void openHudEditor();

        std::vector<Row> buildRows(int tab)
        {
            Settings &s = g_settings;
            std::vector<Row> rows;
            auto changed = [] { save(); };
            if (tab == 0)
            {
                Row mode{"Display mode", "Windowed, borderless full screen, or exclusive full screen. F11 or Alt+Enter switches between windowed and full screen at any time.", RowKind::Choice,
                         [&s] { return s.windowMode; }, [&s, changed](int v) { s.windowMode = v; s.resWidth = 0; s.resHeight = 0; changed(); },
                         {"Windowed", "Borderless", "Fullscreen"}};
                rows.push_back(mode);
                Row resolution{"Resolution",
                               "The game picture's resolution, whatever your screen is. Higher is sharper; lower is lighter on the graphics card. "
                               "One above your screen's is drawn big and shrunk to fit, for smoother edges. Changes straight away.",
                               RowKind::Choice, [&s] { return std::clamp(s.resolution, 0, 4); },
                               [&s, changed](int v) { s.resolution = v; changed(); },
                               {"480p  (1x)", "720p", "1080p", "1440p", "2160p  (4K)"}};
                rows.push_back(resolution);
                Row aspect{"Aspect ratio", "4:3 is the original picture. 16:9 is true widescreen: the game's cameras see more to each side and the HUD and text keep their shape (movies stay 4:3). Stretch fills the screen with the 4:3 picture.", RowKind::Choice,
                           [&s] { return s.aspect; },
                           [&s, changed](int v)
                           {
                               s.aspect = v;
                               changed();
                               tmho::settingsChanged();
                           },
                           {"4:3", "16:9", "Stretch"}};
                rows.push_back(aspect);
                Row filter{"Scaling", "How the picture is enlarged when the resolution is below the screen's: Sharp keeps square pixels, Smooth uses a sharp Lanczos filter.", RowKind::Choice,
                           [&s] { return s.filter; }, [&s, changed](int v) { s.filter = v; changed(); },
                           {"Sharp", "Smooth"}};
                rows.push_back(filter);
                Row vsync{"V-Sync", "Waits for the monitor's refresh to avoid tearing.", RowKind::Toggle,
                          [&s] { return s.vsync ? 1 : 0; }, [&s, changed](int v) { s.vsync = v != 0; changed(); }};
                rows.push_back(vsync);
                Row renderer{"Renderer", g_rendererHelp.c_str(), RowKind::Choice,
                             [&s] { return s.renderer; }, [&s, changed](int v) { s.renderer = v; changed(); },
                             {"Vulkan (GPU)", "Software (CPU)"}};
                renderer.enabled = [] { return g_gpuAvailable; };
                renderer.disabledNote = "This build has no Vulkan renderer";
                rows.push_back(renderer);
                char monitorRate[48];
                std::snprintf(monitorRate, sizeof(monitorRate), "Monitor  (%d fps)", std::max(60, GetMonitorRefreshRate(GetCurrentMonitor())));
                Row rate{"Frame rate",
                         "60 fps shows the game's own frames. 120 fps and Monitor add in-between frames made from the two newest game frames, "
                         "so motion is smoother; the game itself keeps its normal speed. In-between frames show the picture about one frame later.",
                         RowKind::Choice, [&s] { return s.frameRate; }, [&s, changed](int v) { s.frameRate = v; changed(); },
                         {"60 fps", "120 fps", monitorRate}};
                rows.push_back(rate);
            }
            else if (tab == 1)
            {
                Row volume{"Master volume", "Volume of everything the game plays.", RowKind::Slider,
                           [&s] { return s.masterVolume; }, [&s, changed](int v) { s.masterVolume = v; changed(); }};
                volume.minimum = 0;
                volume.maximum = 100;
                volume.step = 5;
                volume.suffix = "%";
                rows.push_back(volume);
                Row mute{"Mute in background", "Silences the game while its window is not focused.", RowKind::Toggle,
                         [&s] { return s.muteUnfocused ? 1 : 0; }, [&s, changed](int v) { s.muteUnfocused = v != 0; changed(); }};
                rows.push_back(mute);
                for (const char *name : {"Music volume", "Effects volume", "Movie volume"})
                {
                    Row part{name, "Separate volumes need the per-voice mixer, which is being built.", RowKind::Slider,
                             [] { return 100; }, [](int) {}};
                    part.suffix = "%";
                    part.enabled = [] { return false; };
                    part.disabledNote = "Coming soon";
                    rows.push_back(part);
                }
            }
            else if (tab == 2)
            {
                Row pause{"Pause in this menu", "Stops the game while the settings drawer is open.", RowKind::Toggle,
                          [&s] { return s.pauseInMenu ? 1 : 0; }, [&s, changed](int v) { s.pauseInMenu = v != 0; changed(); }};
                rows.push_back(pause);
                Row background{"Pause in background", "Stops the game while its window is not focused (another program in front), and carries on when you come back.", RowKind::Toggle,
                               [&s] { return s.pauseUnfocused ? 1 : 0; }, [&s, changed](int v) { s.pauseUnfocused = v != 0; changed(); }};
                rows.push_back(background);
                Row fps{"FPS counter", "Shows FPS in the corner: pictures on screen per second (with in-between frames on, those count too).", RowKind::Toggle,
                        [&s] { return s.showFps ? 1 : 0; }, [&s, changed](int v) { s.showFps = v != 0; changed(); }};
                rows.push_back(fps);
                Row record{"Record screenshots", "Saves a picture of the game every half second in Logs\\frames, for bug reports. About 6 MB a minute; pictures older than 10 minutes are deleted.",
                           RowKind::Toggle, [&s] { return s.recordFrames ? 1 : 0; }, [&s, changed](int v) { s.recordFrames = v != 0; changed(); }};
                rows.push_back(record);
                Row shot{"Screenshot", "Saves the game picture (without menus or the FPS counter) as a PNG in the Screenshots folder next to TMHO.exe. F12 does the same at any time.",
                         RowKind::Action, [] { return 0; }, [](int) { ps2capture::requestScreenshot(); }};
                shot.actionLabel = "Take";
                rows.push_back(shot);
                Row vu1{"VU1 thread", "Runs the PS2's 3D math unit (VU1) and the graphics feed on their own CPU core, so busy scenes stay at full speed. Applies on the next start.",
                        RowKind::Toggle, [&s] { return s.vu1Thread ? 1 : 0; }, [&s, changed](int v) { s.vu1Thread = v != 0; changed(); }};
                rows.push_back(vu1);
                Row setup{"Game disc", g_setupNote.empty() ? "Opens the game setup screen (choose another disc, build again) the next time you start TMHO.exe."
                                                           : g_setupNote.c_str(),
                          RowKind::Action, [] { return 0; }, [](int)
                          {
                              ps2launcher::requestSetupAtNextStart();
                              g_setupNote = "The setup screen will open the next time you start TMHO.exe.";
                          }};
                setup.actionLabel = "Setup";
                rows.push_back(setup);
                Row fov{"Field of view", "How wide the battle camera sees, across the picture. The game's own view is 65\xC2\xB0 at 4:3 and 81\xC2\xB0 at 16:9.",
                        RowKind::Slider,
                        [&s] { return s.fovDegrees > 0 ? s.fovDegrees : static_cast<int>(std::lround(tmho::defaultHorizontalFov())); },
                        [&s, changed](int v)
                        {
                            const int gameDefault = static_cast<int>(std::lround(tmho::defaultHorizontalFov()));
                            s.fovDegrees = v == gameDefault ? 0 : v;
                            changed();
                            tmho::settingsChanged();
                        }};
                fov.minimum = 50;
                fov.maximum = 120;
                fov.suffix = "\xC2\xB0";
                rows.push_back(fov);
                Row distance{"Camera distance", "How far the chase camera sits behind your car: 100% is the game's own, lower is closer, higher is further back. Changes straight away.",
                             RowKind::Slider, [&s] { return s.cameraDistance; }, [&s, changed](int v) { s.cameraDistance = v; changed(); }};
                distance.minimum = 50;
                distance.maximum = 200;
                distance.step = 5;
                distance.suffix = "%";
                rows.push_back(distance);
                // Quits without the window's close button (full screen has none). Two presses, so
                // a stray Enter does not end a match.
                const bool armed = GetTime() < g_quitArmedUntil;
                Row quit{"Quit game", armed ? "Press Quit again to close the game." : "Closes the game. Progress not saved in the game is lost.",
                         RowKind::Action, [] { return 0; }, [](int)
                         {
                             if (GetTime() < g_quitArmedUntil)
                                 g_quitRequested.store(true, std::memory_order_relaxed);
                             else
                                 g_quitArmedUntil = GetTime() + 3.0;
                         }};
                quit.actionLabel = armed ? "Quit now" : "Quit";
                rows.push_back(quit);
            }
            else if (tab == 3)
            {
                Row edit{"Edit HUD layout", "Move and resize the battle HUD over your own game picture: drag a piece, scroll or drag a corner to resize. Best opened during a battle.",
                         RowKind::Action, [] { return 0; }, [](int) { openHudEditor(); }};
                edit.actionLabel = "Edit";
                rows.push_back(edit);
                auto hudChanged = [changed]
                {
                    changed();
                    tmho::hudLayoutChanged();
                    stepHudFrames();
                };
                Row radar{"Radar size", "Size of the radar with the cars-left count and your target's name.", RowKind::Slider,
                          [&s] { return s.hudRadarScale; }, [&s, hudChanged](int v) { s.hudRadarScale = v; hudChanged(); }};
                radar.minimum = 40;
                radar.maximum = 250;
                radar.step = 5;
                radar.suffix = "%";
                rows.push_back(radar);
                Row weapon{"Weapon panel size", "Size of the weapon, ammo, health and turbo panel.", RowKind::Slider,
                           [&s] { return s.hudWeaponScale; }, [&s, hudChanged](int v) { s.hudWeaponScale = v; hudChanged(); }};
                weapon.minimum = 40;
                weapon.maximum = 250;
                weapon.step = 5;
                weapon.suffix = "%";
                rows.push_back(weapon);
                Row edges{"Snap to screen edges", "Moves the radar and the weapon panel right into the corners of the screen, keeping the sizes you picked.",
                          RowKind::Action, [] { return 0; }, [hudChanged](int)
                          {
                              // A small margin of 1.5 % of the picture's height on every side.
                              const tmho::HudBox radarBox = tmho::hudBox(tmho::HudPiece::Radar);
                              const tmho::HudBox weaponBox = tmho::hudBox(tmho::HudPiece::Weapon);
                              const float aspect = g_settings.aspect == 1 ? 16.0f / 9.0f : 4.0f / 3.0f;
                              const float marginY = 0.015f, marginX = 0.015f / aspect;
                              Settings &st = g_settings;
                              st.hudRadarX += static_cast<int>(std::lround((marginX - radarBox.x) * 1000.0f));
                              st.hudRadarY += static_cast<int>(std::lround((marginY - radarBox.y) * 1000.0f));
                              st.hudWeaponX += static_cast<int>(std::lround((1.0f - marginX - (weaponBox.x + weaponBox.width)) * 1000.0f));
                              st.hudWeaponY += static_cast<int>(std::lround((1.0f - marginY - (weaponBox.y + weaponBox.height)) * 1000.0f));
                              hudChanged();
                          }};
                edges.actionLabel = "Snap";
                rows.push_back(edges);
                Row reset{"Reset HUD layout", "Puts the HUD back to its default place: right at the screen edges in 16:9, where the game has it in 4:3.",
                          RowKind::Action, [] { return 0; }, [&s, hudChanged](int)
                          {
                              s.hudRadarX = s.hudRadarY = s.hudWeaponX = s.hudWeaponY = 0;
                              s.hudRadarScale = s.hudWeaponScale = 100;
                              hudChanged();
                          }};
                rows.push_back(reset);
            }
            else
            {
                Row accent{"Accent color", "Colour of highlights in this menu.", RowKind::Swatches,
                           [&s] { return s.accentIndex; }, [&s, changed](int v) { s.accentIndex = v; changed(); }};
                accent.minimum = 0;
                accent.maximum = kSwatchCount;
                rows.push_back(accent);
                Row hex{"Custom color", "Press Enter and type a hex colour, like FF7A1A.", RowKind::HexColor,
                        [&s] { return static_cast<int>(s.accentCustom); }, [&s, changed](int v) { s.accentCustom = static_cast<uint32_t>(v) & 0xFFFFFFu; s.accentIndex = kSwatchCount; changed(); }};
                rows.push_back(hex);
                Row opacity{"Menu opacity", "How much of the game shows through this drawer.", RowKind::Slider,
                            [&s] { return s.menuOpacity; }, [&s, changed](int v) { s.menuOpacity = v; changed(); }};
                opacity.minimum = 60;
                opacity.maximum = 100;
                opacity.step = 5;
                opacity.suffix = "%";
                rows.push_back(opacity);
                Row side{"Drawer side", "Which edge of the screen this drawer opens from.", RowKind::Choice,
                         [&s] { return s.menuSide; }, [&s, changed](int v) { s.menuSide = v; changed(); }, {"Left", "Right"}};
                rows.push_back(side);
                Row reset{"Reset all settings", "Puts every setting back to its default.", RowKind::Action,
                          [] { return 0; }, [&s, changed](int) { s = Settings{}; changed(); tmho::settingsChanged(); }};
                rows.push_back(reset);
            }
            return rows;
        }

        void stepRow(const Row &row, int direction)
        {
            if (!row.enabled())
                return;
            const int value = row.getValue();
            switch (row.kind)
            {
            case RowKind::Choice:
            {
                const int count = static_cast<int>(row.choices.size());
                row.setValue((value + direction + count) % count);
                break;
            }
            case RowKind::Toggle:
                row.setValue(value ? 0 : 1);
                break;
            case RowKind::Slider:
                row.setValue(std::clamp(value + direction * row.step, row.minimum, row.maximum));
                break;
            case RowKind::Swatches:
                row.setValue((value + direction + (row.maximum + 1)) % (row.maximum + 1));
                break;
            default:
                break;
            }
        }

        void activateRow(const Row &row)
        {
            if (!row.enabled())
                return;
            if (row.kind == RowKind::Toggle)
                row.setValue(row.getValue() ? 0 : 1);
            else if (row.kind == RowKind::Action)
                row.setValue(1);
            else if (row.kind == RowKind::HexColor)
            {
                char hex[8];
                std::snprintf(hex, sizeof(hex), "%06X", static_cast<unsigned>(row.getValue()) & 0xFFFFFFu);
                g_hexBuffer = hex;
                g_editingHex = true;
            }
            else
                stepRow(row, +1);
        }

        // ---------------------------------------------------------------- layout
        struct Layout
        {
            float scale;
            Rectangle drawer;
            float rowTop;
            float rowHeight;
            float padding;
            float tabTop;
            float tabHeight;
        };

        Layout computeLayout()
        {
            Layout l{};
            const float sw = static_cast<float>(GetScreenWidth());
            const float sh = static_cast<float>(GetScreenHeight());
            l.scale = std::clamp(sh / 720.0f, 0.6f, 3.0f);
            const float width = std::min(sw * 0.62f, 460.0f * l.scale);
            l.drawer = Rectangle{g_settings.menuSide == 0 ? 0.0f : sw - width, 0.0f, width, sh};
            l.padding = 24.0f * l.scale;
            l.tabTop = 76.0f * l.scale;
            l.tabHeight = 40.0f * l.scale;
            l.rowTop = l.tabTop + l.tabHeight + 18.0f * l.scale;
            l.rowHeight = 50.0f * l.scale;
            return l;
        }

        Rectangle rowRect(const Layout &l, int index)
        {
            return Rectangle{l.drawer.x + 10.0f * l.scale, l.rowTop + static_cast<float>(index) * l.rowHeight,
                             l.drawer.width - 20.0f * l.scale, l.rowHeight - 6.0f * l.scale};
        }

        Rectangle tabRect(const Layout &l, int index)
        {
            const float width = (l.drawer.width - 2.0f * l.padding) / static_cast<float>(kTabCount);
            return Rectangle{l.drawer.x + l.padding + width * static_cast<float>(index), l.tabTop, width, l.tabHeight};
        }

        // Value area of a row (right part), for mouse hits.
        Rectangle valueRect(const Layout &l, const Rectangle &row)
        {
            const float width = std::min(row.width * 0.52f, 220.0f * l.scale);
            return Rectangle{row.x + row.width - width - 12.0f * l.scale, row.y, width, row.height};
        }

        // ---------------------------------------------------------------- input
        // Pressed this frame (also when it was let go again before the frame), no auto-repeat.
        bool keyTap(int key)
        {
            return IsKeyPressed(key) || (key >= 0 && key < 512 && g_keysThisFrame[key]);
        }

        // keyTap, or held down long enough to auto-repeat (moving through lists, sliders).
        bool keyPressed(int key)
        {
            return keyTap(key) || IsKeyPressedRepeat(key);
        }

        bool padPressed(int button)
        {
            for (int pad = 0; pad < 4; ++pad)
                if (IsGamepadAvailable(pad) && IsGamepadButtonPressed(pad, button))
                    return true;
            return false;
        }

        void handleMenuInput()
        {
            const Layout layout = computeLayout();
            std::vector<Row> rows = buildRows(g_tab);
            g_focus = std::clamp(g_focus, 0, static_cast<int>(rows.size()) - 1);

            if (g_editingHex)
            {
                for (int c = GetCharPressed(); c != 0; c = GetCharPressed())
                {
                    if (std::isxdigit(c) && g_hexBuffer.size() < 6)
                        g_hexBuffer.push_back(static_cast<char>(std::toupper(c)));
                }
                if (keyPressed(KEY_BACKSPACE) && !g_hexBuffer.empty())
                    g_hexBuffer.pop_back();
                if (keyTap(KEY_ENTER) || keyTap(KEY_KP_ENTER))
                {
                    if (g_hexBuffer.size() == 6)
                        rows[static_cast<size_t>(g_focus)].setValue(static_cast<int>(std::strtoul(g_hexBuffer.c_str(), nullptr, 16)));
                    g_editingHex = false;
                }
                if (keyTap(KEY_ESCAPE))
                    g_editingHex = false;
                return;
            }

            // Tabs: Q/E, Tab, controller shoulder buttons.
            int tabDelta = 0;
            if (keyTap(KEY_Q) || padPressed(GAMEPAD_BUTTON_LEFT_TRIGGER_1))
                tabDelta = -1;
            if (keyTap(KEY_E) || keyTap(KEY_TAB) || padPressed(GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
                tabDelta = +1;
            if (tabDelta != 0)
            {
                g_tab = (g_tab + tabDelta + kTabCount) % kTabCount;
                g_focus = 0;
                return;
            }

            if (keyPressed(KEY_UP) || keyPressed(KEY_W) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_UP))
                g_focus = (g_focus - 1 + static_cast<int>(rows.size())) % static_cast<int>(rows.size());
            if (keyPressed(KEY_DOWN) || keyPressed(KEY_S) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN))
                g_focus = (g_focus + 1) % static_cast<int>(rows.size());
            Row &focused = rows[static_cast<size_t>(g_focus)];
            if (keyPressed(KEY_LEFT) || keyPressed(KEY_A) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT))
                stepRow(focused, -1);
            if (keyPressed(KEY_RIGHT) || keyPressed(KEY_D) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
                stepRow(focused, +1);
            if (keyTap(KEY_ENTER) || keyTap(KEY_SPACE) || keyTap(KEY_X) || padPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
                activateRow(focused);

            // Mouse.
            const Vector2 mouse = GetMousePosition();
            if (g_clickThisFrame)
            {
                for (int t = 0; t < kTabCount; ++t)
                {
                    if (CheckCollisionPointRec(mouse, tabRect(layout, t)))
                    {
                        g_tab = t;
                        g_focus = 0;
                        return;
                    }
                }
                for (size_t i = 0; i < rows.size(); ++i)
                {
                    const Rectangle r = rowRect(layout, static_cast<int>(i));
                    if (!CheckCollisionPointRec(mouse, r))
                        continue;
                    g_focus = static_cast<int>(i);
                    const Row &row = rows[i];
                    const Rectangle value = valueRect(layout, r);
                    if (row.kind == RowKind::Slider && CheckCollisionPointRec(mouse, value))
                        g_draggingSlider = row.enabled();
                    else if (row.kind == RowKind::Swatches && CheckCollisionPointRec(mouse, value))
                    {
                        const float cell = value.width / static_cast<float>(kSwatchCount + 1);
                        const int index = std::clamp(static_cast<int>((mouse.x - value.x) / cell), 0, kSwatchCount);
                        if (row.enabled())
                            row.setValue(index);
                    }
                    else if (row.kind == RowKind::Choice && CheckCollisionPointRec(mouse, value))
                        stepRow(row, mouse.x < value.x + value.width * 0.5f ? -1 : +1);
                    else
                        activateRow(row);
                    break;
                }
            }
            if (g_draggingSlider)
            {
                if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
                    g_draggingSlider = false;
                else
                {
                    const Row &row = rows[static_cast<size_t>(g_focus)];
                    if (row.kind == RowKind::Slider)
                    {
                        const Rectangle value = valueRect(layout, rowRect(layout, g_focus));
                        const float trackX = value.x;
                        const float trackW = value.width - 52.0f * layout.scale;
                        const float t = std::clamp((mouse.x - trackX) / std::max(1.0f, trackW), 0.0f, 1.0f);
                        int v = row.minimum + static_cast<int>(std::lround(t * static_cast<float>(row.maximum - row.minimum) / static_cast<float>(row.step))) * row.step;
                        v = std::clamp(v, row.minimum, row.maximum);
                        if (v != row.getValue())
                            row.setValue(v);
                    }
                }
            }
        }

        // ---------------------------------------------------------------- drawing
        // ---- short notices (notify)
        std::string g_noticeText;
        double g_noticeUntil = 0.0;

        void drawNotice()
        {
            const double now = GetTime();
            if (g_noticeText.empty() || now >= g_noticeUntil)
                return;
            const float fade = static_cast<float>(std::clamp((g_noticeUntil - now) / 0.4, 0.0, 1.0)); // fades out at the end
            const float scale = std::clamp(static_cast<float>(GetScreenHeight()) / 720.0f, 0.6f, 3.0f);
            const float size = 15.0f * scale;
            const float padX = 10.0f * scale;
            const float padY = 5.0f * scale;
            const float width = textWidth(g_noticeText.c_str(), size, false) + 2.0f * padX;
            const float height = size + 2.0f * padY;
            const Rectangle box{(static_cast<float>(GetScreenWidth()) - width) * 0.5f, 10.0f * scale, width, height};
            DrawRectangleRounded(box, 0.3f, 6, Color{14, 17, 22, static_cast<unsigned char>(200.0f * fade)});
            text(g_noticeText.c_str(), box.x + padX, box.y + padY, size, Color{235, 240, 245, static_cast<unsigned char>(255.0f * fade)}, false);
        }

        // F11 / Alt+Enter: windowed <-> the full-screen mode used last (borderless at first).
        int g_lastFullMode = 1;

        void toggleFullscreen()
        {
            Settings &s = g_settings;
            if (s.windowMode != 0)
            {
                g_lastFullMode = s.windowMode;
                s.windowMode = 0;
            }
            else
                s.windowMode = g_lastFullMode;
            s.resWidth = 0;
            s.resHeight = 0;
            save();
        }

        void drawFpsOverlay(double)
        {
            // "FPS - 60": distinct pictures on screen per second - the game's new frames, plus the
            // in-between frames when they are on.
            const ps2perf::LiveStats live = ps2perf::liveStats();
            if (live.windowFps < 0.0)
                return; // no game running yet (the setup screen)
            double fps = ps2present::shownFps();
            if (fps < 0.0)
                fps = live.gameFps;
            if (live.paused)
                fps = 0.0;
            char label[32];
            if (fps < 0.0)
                std::snprintf(label, sizeof(label), "FPS - --");
            else
                std::snprintf(label, sizeof(label), "FPS - %.0f", fps);
            const float scale = std::clamp(static_cast<float>(GetScreenHeight()) / 720.0f, 0.6f, 3.0f);
            const float size = 15.0f * scale;
            const float padX = 7.0f * scale;
            const float padY = 3.0f * scale;
            const float width = textWidth(label, size, true) + 2.0f * padX;
            const float height = size + 2.0f * padY;
            const Rectangle box{static_cast<float>(GetScreenWidth()) - width - 8.0f * scale, 8.0f * scale, width, height};
            DrawRectangleRounded(box, 0.3f, 6, Color{14, 17, 22, 170});
            text(label, box.x + padX, box.y + padY, size, Color{235, 240, 245, 255}, true);
        }

        void drawDrawer()
        {
            const Layout l = computeLayout();
            const float s = l.scale;
            const uint32_t accent = accentRgb();
            const Color accentColor = rgb(accent);
            const unsigned char panelAlpha = static_cast<unsigned char>(std::clamp(g_settings.menuOpacity, 60, 100) * 255 / 100);

            // Dim the game, then the drawer.
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 90});
            DrawRectangleRec(l.drawer, Color{14, 17, 22, panelAlpha});
            const float edgeX = g_settings.menuSide == 0 ? l.drawer.x + l.drawer.width - 1.0f : l.drawer.x;
            DrawRectangle(static_cast<int>(edgeX), 0, 1, static_cast<int>(l.drawer.height), Color{255, 255, 255, 30});

            // Header.
            DrawRectangle(static_cast<int>(l.drawer.x + l.padding), static_cast<int>(28.0f * s), static_cast<int>(4.0f * s),
                          static_cast<int>(28.0f * s), accentColor);
            text("SETTINGS", l.drawer.x + l.padding + 14.0f * s, 26.0f * s, 28.0f * s, WHITE, true);
            const char *hint = "F2";
            const float hintSize = 14.0f * s;
            const float hintW = textWidth(hint, hintSize, true) + 14.0f * s;
            const Rectangle hintBox{l.drawer.x + l.drawer.width - l.padding - hintW, 31.0f * s, hintW, 22.0f * s};
            DrawRectangleRoundedLinesEx(hintBox, 0.3f, 6, 1.0f, Color{255, 255, 255, 70});
            text(hint, hintBox.x + 7.0f * s, hintBox.y + (hintBox.height - hintSize) * 0.5f, hintSize, Color{180, 188, 200, 255}, true);

            // Tabs.
            for (int t = 0; t < kTabCount; ++t)
            {
                const Rectangle r = tabRect(l, t);
                const bool active = t == g_tab;
                const float size = 14.0f * s;
                const float w = textWidth(kTabs[t], size, true);
                text(kTabs[t], r.x + (r.width - w) * 0.5f, r.y + (r.height - size) * 0.5f - 2.0f * s, size,
                     active ? WHITE : Color{138, 147, 163, 255}, true);
                if (active)
                    DrawRectangle(static_cast<int>(r.x + 10.0f * s), static_cast<int>(r.y + r.height - 3.0f * s),
                                  static_cast<int>(r.width - 20.0f * s), static_cast<int>(3.0f * s), accentColor);
            }
            DrawRectangle(static_cast<int>(l.drawer.x + l.padding), static_cast<int>(l.tabTop + l.tabHeight),
                          static_cast<int>(l.drawer.width - 2.0f * l.padding), 1, Color{255, 255, 255, 28});

            // Rows.
            const std::vector<Row> rows = buildRows(g_tab);
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const Row &row = rows[i];
                const Rectangle r = rowRect(l, static_cast<int>(i));
                const bool focused = static_cast<int>(i) == g_focus;
                const bool enabled = row.enabled();
                if (focused)
                {
                    DrawRectangleRounded(r, 0.18f, 6, rgb(accent, 40));
                    DrawRectangle(static_cast<int>(r.x), static_cast<int>(r.y + 8.0f * s), static_cast<int>(3.0f * s),
                                  static_cast<int>(r.height - 16.0f * s), accentColor);
                }
                const Color labelColor = enabled ? Color{230, 233, 239, 255} : Color{110, 117, 130, 255};
                const float labelSize = 17.0f * s;
                const float labelY = enabled || !row.disabledNote[0] ? r.y + (r.height - labelSize) * 0.5f : r.y + 6.0f * s;
                text(row.label, r.x + 16.0f * s, labelY, labelSize, labelColor);
                if (!enabled && row.disabledNote[0])
                    text(row.disabledNote, r.x + 16.0f * s, labelY + labelSize + 1.0f * s, 12.5f * s, Color{120, 128, 142, 255});

                const Rectangle value = valueRect(l, r);
                const Color valueColor = enabled ? WHITE : Color{110, 117, 130, 255};
                const float valueSize = 16.0f * s;
                const float centerY = r.y + r.height * 0.5f;
                switch (row.kind)
                {
                case RowKind::Choice:
                {
                    const std::string &label = row.choices[static_cast<size_t>(std::clamp(row.getValue(), 0, static_cast<int>(row.choices.size()) - 1))];
                    const float w = textWidth(label.c_str(), valueSize, true);
                    text(label.c_str(), value.x + (value.width - w) * 0.5f, centerY - valueSize * 0.5f, valueSize, valueColor, true);
                    const Color arrow = enabled ? (focused ? accentColor : Color{150, 158, 172, 255}) : Color{80, 86, 98, 255};
                    const float a = 6.0f * s;
                    DrawTriangle(Vector2{value.x + a, centerY}, Vector2{value.x + 2.0f * a, centerY + a}, Vector2{value.x + 2.0f * a, centerY - a}, arrow);
                    const float rx = value.x + value.width;
                    DrawTriangle(Vector2{rx - a, centerY}, Vector2{rx - 2.0f * a, centerY - a}, Vector2{rx - 2.0f * a, centerY + a}, arrow);
                    break;
                }
                case RowKind::Toggle:
                {
                    const bool on = row.getValue() != 0;
                    const Rectangle pill{value.x + value.width - 46.0f * s, centerY - 12.0f * s, 46.0f * s, 24.0f * s};
                    DrawRectangleRounded(pill, 1.0f, 12, on ? accentColor : Color{58, 64, 76, 255});
                    const float knobX = on ? pill.x + pill.width - 12.0f * s : pill.x + 12.0f * s;
                    DrawCircleV(Vector2{knobX, centerY}, 9.0f * s, on ? inkFor(accent) : Color{200, 205, 214, 255});
                    const char *state = on ? "On" : "Off";
                    text(state, pill.x - textWidth(state, valueSize) - 10.0f * s, centerY - valueSize * 0.5f, valueSize, valueColor);
                    break;
                }
                case RowKind::Slider:
                {
                    const int v = row.getValue();
                    const float trackW = value.width - 52.0f * s;
                    const Rectangle track{value.x, centerY - 3.0f * s, trackW, 6.0f * s};
                    DrawRectangleRounded(track, 1.0f, 6, Color{58, 64, 76, 255});
                    const float t = row.maximum > row.minimum ? static_cast<float>(v - row.minimum) / static_cast<float>(row.maximum - row.minimum) : 0.0f;
                    DrawRectangleRounded(Rectangle{track.x, track.y, std::max(track.height, track.width * t), track.height}, 1.0f, 6,
                                         enabled ? accentColor : Color{80, 86, 98, 255});
                    DrawCircleV(Vector2{track.x + track.width * t, centerY}, 8.0f * s, enabled ? WHITE : Color{120, 126, 138, 255});
                    char number[24];
                    std::snprintf(number, sizeof(number), "%d%s", v, row.suffix);
                    const float w = textWidth(number, valueSize, true);
                    text(number, value.x + value.width - w, centerY - valueSize * 0.5f, valueSize, valueColor, true);
                    break;
                }
                case RowKind::Swatches:
                {
                    const float cell = value.width / static_cast<float>(kSwatchCount + 1);
                    const float radius = std::min(cell * 0.36f, 11.0f * s);
                    for (int k = 0; k <= kSwatchCount; ++k)
                    {
                        const Vector2 c{value.x + cell * (static_cast<float>(k) + 0.5f), centerY};
                        const uint32_t color = k < kSwatchCount ? kSwatches[k].rgb : g_settings.accentCustom;
                        DrawCircleV(c, radius, rgb(color));
                        if (k == kSwatchCount)
                            text("+", c.x - radius * 0.42f, c.y - radius * 0.95f, radius * 1.8f, inkFor(color), true);
                        if (k == g_settings.accentIndex)
                            DrawRing(c, radius + 2.0f * s, radius + 4.5f * s, 0.0f, 360.0f, 32, WHITE);
                    }
                    break;
                }
                case RowKind::HexColor:
                {
                    char hex[16];
                    if (g_editingHex && focused)
                        std::snprintf(hex, sizeof(hex), "#%s_", g_hexBuffer.c_str());
                    else
                        std::snprintf(hex, sizeof(hex), "#%06X", static_cast<unsigned>(g_settings.accentCustom) & 0xFFFFFFu);
                    const Rectangle box{value.x + value.width - 120.0f * s, centerY - 14.0f * s, 120.0f * s, 28.0f * s};
                    DrawRectangleRounded(box, 0.3f, 6, Color{30, 35, 44, 255});
                    DrawRectangleRoundedLinesEx(box, 0.3f, 6, 1.0f, g_editingHex && focused ? accentColor : Color{255, 255, 255, 50});
                    DrawCircleV(Vector2{box.x + 14.0f * s, centerY}, 6.0f * s, rgb(g_settings.accentCustom));
                    text(hex, box.x + 28.0f * s, centerY - valueSize * 0.5f, valueSize, WHITE, true);
                    break;
                }
                case RowKind::Action:
                {
                    const char *label = row.actionLabel;
                    const float w = textWidth(label, valueSize, true) + 28.0f * s;
                    const Rectangle button{value.x + value.width - w, centerY - 14.0f * s, w, 28.0f * s};
                    DrawRectangleRounded(button, 0.4f, 6, focused ? accentColor : Color{48, 54, 66, 255});
                    text(label, button.x + 14.0f * s, centerY - valueSize * 0.5f, valueSize, focused ? inkFor(accent) : WHITE, true);
                    break;
                }
                }
            }

            // Help text for the focused row.
            if (!rows.empty())
            {
                const Row &row = rows[static_cast<size_t>(std::clamp(g_focus, 0, static_cast<int>(rows.size()) - 1))];
                const float y = l.rowTop + static_cast<float>(rows.size()) * l.rowHeight + 10.0f * s;
                // Wrap the help line to the drawer width.
                const float size = 14.0f * s;
                const float maxWidth = l.drawer.width - 2.0f * l.padding;
                std::istringstream words(row.help);
                std::string word, line;
                float lineY = y;
                while (words >> word)
                {
                    const std::string candidate = line.empty() ? word : line + " " + word;
                    if (textWidth(candidate.c_str(), size) > maxWidth && !line.empty())
                    {
                        text(line.c_str(), l.drawer.x + l.padding, lineY, size, Color{150, 158, 172, 255});
                        lineY += size * 1.35f;
                        line = word;
                    }
                    else
                        line = candidate;
                }
                if (!line.empty())
                    text(line.c_str(), l.drawer.x + l.padding, lineY, size, Color{150, 158, 172, 255});
            }

            // Footer.
            const float footerY = l.drawer.height - 44.0f * s;
            DrawRectangle(static_cast<int>(l.drawer.x + l.padding), static_cast<int>(footerY - 10.0f * s),
                          static_cast<int>(l.drawer.width - 2.0f * l.padding), 1, Color{255, 255, 255, 28});
            const char *footer = g_editingHex ? "Type 6 hex digits   Enter OK   Esc Cancel"
                                              : "\xE2\x86\x91\xE2\x86\x93 Select   \xE2\x86\x90\xE2\x86\x92 Change   Q/E Tab   F2 Close";
            text(footer, l.drawer.x + l.padding, footerY + 4.0f * s, 14.0f * s, Color{138, 147, 163, 255});
        }

        // ---------------------------------------------------------------- HUD editor
        // Lays the battle HUD's pieces out over the player's own game picture. The game stays
        // paused but runs a few frames after every change, so the real HUD follows the boxes.
        bool g_hudLiveAtOpen = false; // the game showed its HUD right before F2 paused it

        // Where the game picture is on the screen, as the window loop draws it.
        Rectangle pictureOnScreen()
        {
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
            presentRect(0.0f, 0.0f, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight()), x, y, w, h);
            return Rectangle{x, y, w, h};
        }

        std::string hudPreviewPath()
        {
            return ps2perf::logDirectory() + "/hud_preview.png";
        }

        void getPiece(int piece, int &x, int &y, int &scale)
        {
            const Settings &s = g_settings;
            x = piece == 0 ? s.hudRadarX : s.hudWeaponX;
            y = piece == 0 ? s.hudRadarY : s.hudWeaponY;
            scale = piece == 0 ? s.hudRadarScale : s.hudWeaponScale;
        }

        void setPiece(int piece, int x, int y, int scale)
        {
            Settings &s = g_settings;
            x = std::clamp(x, -1000, 1000);
            y = std::clamp(y, -1000, 1000);
            scale = std::clamp(scale, 40, 250);
            int &px = piece == 0 ? s.hudRadarX : s.hudWeaponX;
            int &py = piece == 0 ? s.hudRadarY : s.hudWeaponY;
            int &ps = piece == 0 ? s.hudRadarScale : s.hudWeaponScale;
            if (px == x && py == y && ps == scale)
                return;
            px = x;
            py = y;
            ps = scale;
            g_hudDirty = true;
            tmho::hudLayoutChanged();
            stepHudFrames(); // let the paused game redraw the HUD
        }

        Rectangle hudBoxOnScreen(int piece, const Rectangle &pic)
        {
            const tmho::HudBox b = tmho::hudBox(static_cast<tmho::HudPiece>(piece));
            return Rectangle{pic.x + b.x * pic.width, pic.y + b.y * pic.height, b.width * pic.width, b.height * pic.height};
        }

        // The editor's bar (title, hints, buttons) sits at the top of the screen, or at the bottom
        // when a HUD piece is under the top one.
        Rectangle hudToolbar(float s, const Rectangle &pic)
        {
            const float h = 58.0f * s;
            const float sw = static_cast<float>(GetScreenWidth());
            const Rectangle top{0.0f, 0.0f, sw, h};
            const Rectangle bottom{0.0f, static_cast<float>(GetScreenHeight()) - h, sw, h};
            float overlapTop = 0.0f, overlapBottom = 0.0f;
            for (int p = 0; p < 2; ++p)
            {
                const Rectangle box = hudBoxOnScreen(p, pic);
                const Rectangle a = GetCollisionRec(top, box), b = GetCollisionRec(bottom, box);
                overlapTop += a.width * a.height;
                overlapBottom += b.width * b.height;
            }
            return overlapTop <= overlapBottom ? top : bottom;
        }

        void hudButtons(float s, Rectangle (&out)[3])
        {
            const Rectangle bar = hudToolbar(s, pictureOnScreen());
            const float h = 34.0f * s;
            const float y = bar.y + (bar.height - h) * 0.5f;
            float right = static_cast<float>(GetScreenWidth()) - 16.0f * s;
            const char *labels[3] = {"Done", "Reset all", "Reset piece"};
            for (int i = 0; i < 3; ++i)
            {
                const float w = textWidth(labels[i], 16.0f * s, true) + 32.0f * s;
                out[i] = Rectangle{right - w, y, w, h};
                right -= w + 10.0f * s;
            }
        }

        void saveHudIfChanged()
        {
            if (g_hudDirty)
            {
                save();
                g_hudDirty = false;
            }
        }

        void openHudEditor()
        {
            g_hudEditor = true;
            g_hudDrag = 0;
            g_hudPreviewLive = tmho::hudShownRecently(1.0) || (g_paused.load(std::memory_order_relaxed) && g_hudLiveAtOpen);
            if (g_hudPreviewLive && g_frameTexture && g_frameTexture->id != 0)
            {
                // This battle's picture, for opening the editor outside a battle later.
                Image image = LoadImageFromTexture(*g_frameTexture);
                if (image.data)
                {
                    if (image.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
                        fixFrameCopy(static_cast<unsigned char *>(image.data), static_cast<size_t>(image.width) * static_cast<size_t>(image.height));
                    ExportImage(image, hudPreviewPath().c_str());
                    UnloadImage(image);
                }
                if (g_hudPreview.id != 0)
                {
                    UnloadTexture(g_hudPreview);
                    g_hudPreview = Texture2D{};
                }
            }
            if (!g_hudPreviewLive && g_hudPreview.id == 0 && FileExists(hudPreviewPath().c_str()))
            {
                g_hudPreview = LoadTexture(hudPreviewPath().c_str());
                if (g_hudPreview.id != 0)
                    SetTextureFilter(g_hudPreview, TEXTURE_FILTER_BILINEAR);
            }
        }

        void closeHudEditor()
        {
            g_hudEditor = false;
            g_hudDrag = 0;
            saveHudIfChanged();
        }

        void handleHudEditorInput()
        {
            const float s = std::clamp(static_cast<float>(GetScreenHeight()) / 720.0f, 0.6f, 3.0f);
            const Rectangle pic = pictureOnScreen();
            const Vector2 mouse = GetMousePosition();
            if (keyTap(KEY_ESCAPE) || keyTap(KEY_ENTER) || keyTap(KEY_KP_ENTER) || padPressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) ||
                padPressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            {
                closeHudEditor();
                return;
            }
            if (keyTap(KEY_TAB) || keyTap(KEY_Q) || keyTap(KEY_E) || padPressed(GAMEPAD_BUTTON_LEFT_TRIGGER_1) ||
                padPressed(GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
                g_hudSelected = (g_hudSelected + 1) % 2;

            int x = 0, y = 0, scale = 100;
            getPiece(g_hudSelected, x, y, scale);
            const int step = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT) ? 20 : 5;
            bool keyed = false;
            auto nudge = [&](bool pressed, int &value, int delta) { if (pressed) { value += delta; keyed = true; } };
            nudge(keyPressed(KEY_LEFT) || keyPressed(KEY_A) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_LEFT), x, -step);
            nudge(keyPressed(KEY_RIGHT) || keyPressed(KEY_D) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_RIGHT), x, step);
            nudge(keyPressed(KEY_UP) || keyPressed(KEY_W) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_UP), y, -step);
            nudge(keyPressed(KEY_DOWN) || keyPressed(KEY_S) || padPressed(GAMEPAD_BUTTON_LEFT_FACE_DOWN), y, step);
            nudge(keyPressed(KEY_EQUAL) || keyPressed(KEY_KP_ADD) || keyPressed(KEY_PAGE_UP) || padPressed(GAMEPAD_BUTTON_RIGHT_FACE_UP), scale, 5);
            nudge(keyPressed(KEY_MINUS) || keyPressed(KEY_KP_SUBTRACT) || keyPressed(KEY_PAGE_DOWN) || padPressed(GAMEPAD_BUTTON_RIGHT_FACE_LEFT), scale, -5);
            if (keyTap(KEY_R))
            {
                x = y = 0;
                scale = 100;
                keyed = true;
            }
            if (keyed)
            {
                setPiece(g_hudSelected, x, y, scale);
                saveHudIfChanged();
            }

            Rectangle buttons[3];
            hudButtons(s, buttons);
            if (g_clickThisFrame)
            {
                if (CheckCollisionPointRec(mouse, buttons[0]))
                {
                    closeHudEditor();
                    return;
                }
                if (CheckCollisionPointRec(mouse, buttons[1]))
                {
                    setPiece(0, 0, 0, 100);
                    setPiece(1, 0, 0, 100);
                    saveHudIfChanged();
                    return;
                }
                if (CheckCollisionPointRec(mouse, buttons[2]))
                {
                    setPiece(g_hudSelected, 0, 0, 100);
                    saveHudIfChanged();
                    return;
                }
                // The selected piece first (it is drawn on top), then the other.
                const float grab = 10.0f * s;
                for (int k = 0; k < 2; ++k)
                {
                    const int piece = k == 0 ? g_hudSelected : 1 - g_hudSelected;
                    const Rectangle box = hudBoxOnScreen(piece, pic);
                    const Rectangle area{box.x - grab, box.y - grab, box.width + 2.0f * grab, box.height + 2.0f * grab};
                    if (!CheckCollisionPointRec(mouse, area))
                        continue;
                    g_hudSelected = piece;
                    const bool nearLeft = std::fabs(mouse.x - box.x) <= grab, nearRight = std::fabs(mouse.x - (box.x + box.width)) <= grab;
                    const bool nearTop = std::fabs(mouse.y - box.y) <= grab, nearBottom = std::fabs(mouse.y - (box.y + box.height)) <= grab;
                    g_hudDrag = (nearLeft || nearRight) && (nearTop || nearBottom) ? 2 : 1;
                    g_hudDragStart = mouse;
                    getPiece(piece, g_hudStartX, g_hudStartY, g_hudStartScale);
                    break;
                }
            }
            if (g_hudDrag != 0)
            {
                if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT))
                {
                    g_hudDrag = 0;
                    saveHudIfChanged();
                }
                else if (g_hudDrag == 1)
                {
                    const int nx = g_hudStartX + static_cast<int>(std::lround((mouse.x - g_hudDragStart.x) / std::max(1.0f, pic.width) * 1000.0f));
                    const int ny = g_hudStartY + static_cast<int>(std::lround((mouse.y - g_hudDragStart.y) / std::max(1.0f, pic.height) * 1000.0f));
                    setPiece(g_hudSelected, nx, ny, g_hudStartScale);
                }
                else
                {
                    // Corner: the size follows the pointer's distance from the box's centre.
                    int bx = 0, by = 0, bs = 100;
                    getPiece(g_hudSelected, bx, by, bs);
                    setPiece(g_hudSelected, g_hudStartX, g_hudStartY, g_hudStartScale);
                    const Rectangle start = hudBoxOnScreen(g_hudSelected, pic);
                    setPiece(g_hudSelected, bx, by, bs);
                    const Vector2 centre{start.x + start.width * 0.5f, start.y + start.height * 0.5f};
                    const float d0 = std::max(4.0f, std::hypot(g_hudDragStart.x - centre.x, g_hudDragStart.y - centre.y));
                    const float d1 = std::hypot(mouse.x - centre.x, mouse.y - centre.y);
                    setPiece(g_hudSelected, g_hudStartX, g_hudStartY, static_cast<int>(std::lround(g_hudStartScale * d1 / d0 / 5.0f)) * 5);
                }
            }
            const float wheel = GetMouseWheelMove();
            if (wheel != 0.0f && g_hudDrag == 0)
            {
                int piece = g_hudSelected;
                for (int p = 0; p < 2; ++p)
                    if (CheckCollisionPointRec(mouse, hudBoxOnScreen(p, pic)))
                        piece = p;
                g_hudSelected = piece;
                int px = 0, py = 0, ps = 100;
                getPiece(piece, px, py, ps);
                setPiece(piece, px, py, ps + (wheel > 0.0f ? 5 : -5));
                saveHudIfChanged();
            }
        }

        void drawHudEditor()
        {
            const float s = std::clamp(static_cast<float>(GetScreenHeight()) / 720.0f, 0.6f, 3.0f);
            const float sw = static_cast<float>(GetScreenWidth());
            const uint32_t accent = accentRgb();
            const Color accentColor = rgb(accent);
            const Rectangle pic = pictureOnScreen();

            // Outside a battle: the last battle's picture (or nothing yet) stands in for the game.
            if (!g_hudPreviewLive)
            {
                DrawRectangleRec(pic, Color{10, 12, 16, 255});
                if (g_hudPreview.id != 0)
                    DrawTexturePro(g_hudPreview, Rectangle{0.0f, 0.0f, static_cast<float>(g_hudPreview.width), static_cast<float>(g_hudPreview.height)},
                                   pic, Vector2{0.0f, 0.0f}, 0.0f, Color{255, 255, 255, 150});
                const char *note = g_hudPreview.id != 0 ? "Picture from your last battle. The real HUD follows when you play."
                                                        : "Start a battle to see your HUD here. The boxes show where its pieces go.";
                const float size = 16.0f * s;
                text(note, pic.x + (pic.width - textWidth(note, size)) * 0.5f, pic.y + pic.height * 0.5f - size * 0.5f, size,
                     Color{200, 206, 214, 255});
            }
            DrawRectangleLinesEx(pic, 1.0f, Color{255, 255, 255, 50});

            const char *names[2] = {"Radar", "Weapons & health"};
            for (int k = 0; k < 2; ++k)
            {
                const int piece = k == 0 ? 1 - g_hudSelected : g_hudSelected; // selected one on top
                const bool selected = piece == g_hudSelected;
                const Rectangle box = hudBoxOnScreen(piece, pic);
                DrawRectangleRec(box, selected ? rgb(accent, 44) : Color{255, 255, 255, 16});
                DrawRectangleLinesEx(box, selected ? 2.0f * s : 1.0f * s, selected ? accentColor : Color{255, 255, 255, 140});
                int px = 0, py = 0, ps = 100;
                getPiece(piece, px, py, ps);
                char label[64];
                std::snprintf(label, sizeof(label), "%s  %d%%", names[piece], ps);
                const float size = 13.0f * s;
                const float tagW = textWidth(label, size, true) + 14.0f * s;
                const float tagH = size + 8.0f * s;
                float tagY = box.y - tagH - 3.0f * s;
                if (tagY < pic.y)
                    tagY = box.y + box.height + 3.0f * s;
                const float tagX = std::clamp(box.x, 0.0f, sw - tagW);
                DrawRectangleRounded(Rectangle{tagX, tagY, tagW, tagH}, 0.3f, 6, selected ? accentColor : Color{14, 17, 22, 220});
                text(label, tagX + 7.0f * s, tagY + 4.0f * s, size, selected ? inkFor(accent) : WHITE, true);
                if (selected)
                {
                    const float h = 7.0f * s;
                    for (const Vector2 c : {Vector2{box.x, box.y}, Vector2{box.x + box.width, box.y}, Vector2{box.x, box.y + box.height},
                                            Vector2{box.x + box.width, box.y + box.height}})
                    {
                        DrawRectangleRec(Rectangle{c.x - h * 0.5f, c.y - h * 0.5f, h, h}, WHITE);
                        DrawRectangleLinesEx(Rectangle{c.x - h * 0.5f, c.y - h * 0.5f, h, h}, 1.0f, accentColor);
                    }
                }
            }

            // The bar: title, how it works, buttons.
            const Rectangle bar = hudToolbar(s, pic);
            DrawRectangleRec(bar, Color{14, 17, 22, 228});
            DrawRectangle(static_cast<int>(18.0f * s), static_cast<int>(bar.y + 14.0f * s), static_cast<int>(4.0f * s), static_cast<int>(30.0f * s), accentColor);
            text("HUD LAYOUT", 30.0f * s, bar.y + 12.0f * s, 20.0f * s, WHITE, true);
            Rectangle buttons[3];
            hudButtons(s, buttons);
            const char *hint = "Drag a piece to move it  \xC2\xB7  scroll or drag a corner to resize  \xC2\xB7  arrows nudge (Shift: more)  \xC2\xB7  Tab: other piece  \xC2\xB7  R: reset piece  \xC2\xB7  Enter: done";
            const float hintSize = 13.0f * s;
            const float hintRoom = buttons[2].x - 30.0f * s - 16.0f * s;
            if (textWidth(hint, hintSize) > hintRoom)
                hint = "Drag: move  \xC2\xB7  scroll / corner: size  \xC2\xB7  arrows: nudge  \xC2\xB7  Tab: other piece  \xC2\xB7  Enter: done";
            text(hint, 30.0f * s, bar.y + 36.0f * s, hintSize, Color{170, 178, 190, 255});

            const char *labels[3] = {"Done", "Reset all", "Reset piece"};
            const Vector2 mouse = GetMousePosition();
            for (int i = 0; i < 3; ++i)
            {
                const bool hover = CheckCollisionPointRec(mouse, buttons[i]);
                DrawRectangleRounded(buttons[i], 0.35f, 6, i == 0 ? accentColor : (hover ? Color{70, 78, 92, 240} : Color{40, 46, 58, 235}));
                text(labels[i], buttons[i].x + 16.0f * s, buttons[i].y + (buttons[i].height - 16.0f * s) * 0.5f, 16.0f * s,
                     i == 0 ? inkFor(accent) : WHITE, true);
            }
        }
    }

    namespace ui
    {
        void ensureFonts() { loadFonts(); }
        void text(const char *value, float x, float y, float size, Color color, bool bold)
        {
            ps2settings::text(value, x, y, size, color, bold);
        }
        float textWidth(const char *value, float size, bool bold) { return ps2settings::textWidth(value, size, bold); }
        Color rgb(uint32_t value, unsigned char alpha) { return ps2settings::rgb(value, alpha); }
        uint32_t accent() { return accentRgb(); }
        Color inkFor(uint32_t value) { return ps2settings::inkFor(value); }
        bool clicked() { return g_clickThisFrame; }
        bool keyTapped(int key) { return ps2settings::keyTap(key); }
        bool keyRepeat(int key) { return ps2settings::keyPressed(key); }
    }

    Settings &get()
    {
        return g_settings;
    }

    bool quitRequested() noexcept
    {
        return g_quitRequested.load(std::memory_order_relaxed);
    }

    bool menuOpen() noexcept
    {
        return g_menuOpen.load(std::memory_order_relaxed);
    }

    bool emulationPaused() noexcept
    {
        return g_paused.load(std::memory_order_relaxed);
    }

    void loadEarly(const char *iniPath)
    {
        if (iniPath && *iniPath)
            g_iniPath = iniPath;
        load();
        if (g_settings.audioDump != 0)
        {
            // Testing: the SPU2's mix and what the sound card was given, as raw 48 kHz stereo.
            const std::string dir = ps2perf::logDirectory();
            const std::string mix = dir + "/audio_spu2.raw";
            const std::string out = dir + "/audio_out.raw";
#if defined(_WIN32)
            _putenv_s("PS2X_AUDIO_DUMP", mix.c_str());
            _putenv_s("PS2X_AUDIO_OUT_DUMP", out.c_str());
#else
            setenv("PS2X_AUDIO_DUMP", mix.c_str(), 1);
            setenv("PS2X_AUDIO_OUT_DUMP", out.c_str(), 1);
#endif
        }
    }

    void setRendererStatus(bool gpuActive, bool gpuAvailable, const char *deviceName)
    {
        g_gpuActive = gpuActive;
        g_gpuAvailable = gpuAvailable;
        g_gpuDevice = deviceName ? deviceName : "";
        g_rendererHelp = "Vulkan draws the game on your graphics card; Software uses the CPU. Applies on the next start. Now: ";
        g_rendererHelp += gpuActive ? ("Vulkan on " + g_gpuDevice) : std::string("Software (CPU)");
        g_rendererHelp += ".";
    }

    void initialize(const char *iniPath)
    {
        if (iniPath && *iniPath)
            g_iniPath = iniPath;
        load();
        if (g_settings.resolution < 0)
        {
            // Older settings: from the internal resolution; new: the monitor's own lines.
            Settings &s = g_settings;
            if (s.internalScale == 1)
                s.resolution = 0;
            else if (s.internalScale == 2)
                s.resolution = 2;
            else if (s.internalScale == 3)
                s.resolution = 3;
            else
            {
                const int lines = GetMonitorHeight(GetCurrentMonitor());
                s.resolution = lines >= 2160 ? 4 : lines >= 1440 ? 3 : lines >= 1080 ? 2 : lines >= 720 ? 1 : 0;
            }
            save();
        }
        g_appliedWindowMode = 0;
        g_appliedResolution = -1;
        g_appliedAspectForWindow = -1;
        g_appliedSwapInterval = -1; // force the first apply
        // The window starts windowed at its default size: apply the saved size/mode now.
        applyWindowMode();
        applyVsync();
        applyVolume();
    }

    void update(Texture2D *frameTexture)
    {
        g_frameTexture = frameTexture;
        installTapCapture();
        latchTaps();
        if (keyTap(KEY_F11) || g_altEnterThisFrame)
            toggleFullscreen();
        if (keyTap(KEY_F12))
            ps2capture::requestScreenshot();
        if (keyTap(KEY_F2))
        {
            const bool open = !g_menuOpen.load(std::memory_order_relaxed);
            g_menuOpen.store(open, std::memory_order_relaxed);
            g_editingHex = false;
            g_draggingSlider = false;
            if (g_hudEditor)
                closeHudEditor();
            if (open)
            {
                loadFonts();
                g_hudLiveAtOpen = tmho::hudShownRecently(1.0);
            }
        }
        else if (g_menuOpen.load(std::memory_order_relaxed))
        {
            if (g_hudEditor)
                handleHudEditorInput();
            else if (!g_editingHex && (keyTap(KEY_ESCAPE) || padPressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT)))
                g_menuOpen.store(false, std::memory_order_relaxed);
            else
                handleMenuInput();
        }
        // Paused while the menu is open, except for a moment after a HUD layout change so the
        // game draws its HUD in the new place.
        const bool stepping = GetTime() < g_hudStepUntil.load(std::memory_order_relaxed);
        const bool inBackground = g_settings.pauseUnfocused && !IsWindowFocused();
        g_paused.store((g_menuOpen.load(std::memory_order_relaxed) && g_settings.pauseInMenu && !stepping) || inBackground,
                       std::memory_order_relaxed);

        applyWindowMode();
        applyVsync();
        applyFilter();
        applyVolume();
        applyInternalResolution();
    }

    void pictureTargetSize(int screenWidth, int screenHeight, int &outWidth, int &outHeight)
    {
        outWidth = screenWidth;
        outHeight = screenHeight;
    }

    void presentRect(float sourceWidth, float sourceHeight, float screenWidth, float screenHeight,
                     float &outX, float &outY, float &outWidth, float &outHeight)
    {
        (void)sourceWidth;
        (void)sourceHeight;
        float aspect = 4.0f / 3.0f;
        // 16:9 shows the game's widened 3D picture wide; pre-rendered movies stay 4:3.
        if (g_settings.aspect == 1 && !ps2movie::playing())
            aspect = 16.0f / 9.0f;
        if (g_settings.aspect == 2)
        {
            outX = 0.0f;
            outY = 0.0f;
            outWidth = screenWidth;
            outHeight = screenHeight;
            return;
        }
        float width = screenWidth;
        float height = width / aspect;
        if (height > screenHeight)
        {
            height = screenHeight;
            width = height * aspect;
        }
        outX = std::floor((screenWidth - width) * 0.5f);
        outY = std::floor((screenHeight - height) * 0.5f);
        outWidth = std::floor(width);
        outHeight = std::floor(height);
    }

    void draw(double gameSpeedPercent)
    {
        if (g_settings.showFps)
        {
            loadFonts();
            drawFpsOverlay(gameSpeedPercent);
        }
        if (!g_noticeText.empty())
        {
            loadFonts();
            drawNotice();
        }
        if (g_menuOpen.load(std::memory_order_relaxed))
        {
            if (g_hudEditor)
                drawHudEditor();
            else
                drawDrawer();
        }
    }
}

namespace ps2settings
{
    namespace
    {
        std::atomic<bool> g_frameRawBgra{false};
    }

    void setFrameTextureRawBgra(bool raw) noexcept
    {
        g_frameRawBgra.store(raw, std::memory_order_relaxed);
    }

    bool compositorSync() noexcept
    {
        return g_compositorSync;
    }

    void notify(const char *text) noexcept
    {
        try
        {
            g_noticeText = text ? text : "";
            g_noticeUntil = GetTime() + 2.5;
        }
        catch (...)
        {
        }
    }

    bool frameTextureRawBgra() noexcept
    {
        return g_frameRawBgra.load(std::memory_order_relaxed);
    }

    void fixFrameCopy(unsigned char *pixels, size_t pixelCount) noexcept
    {
        if (!pixels || !frameTextureRawBgra())
            return;
        for (size_t i = 0; i < pixelCount; ++i, pixels += 4)
        {
            const unsigned char b = pixels[0];
            pixels[0] = pixels[2];
            pixels[2] = b;
            pixels[3] = 255u;
        }
    }
}
