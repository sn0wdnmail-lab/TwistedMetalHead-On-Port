#pragma once

// In-game settings drawer (F2): Video / Audio / Game / UI tabs, saved to settings.ini.
// The F1 key stays with the runtime debugger.
//
// Everything here runs on the window (main) thread except menuOpen() and emulationPaused(),
// which the game thread reads to neutralise controller input and to pause the EE scheduler.

#include <cstddef>
#include <cstdint>

struct Texture; // raylib (Texture2D is a typedef of it)
struct Rectangle;

namespace ps2settings
{
    struct Settings
    {
        int windowMode = 0;     // 0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen
        int windowSize = 2;     // old index into the window-size list (read from old settings files only)
        int resWidth = 0;       // resolution: window size (windowed), display mode (fullscreen) or picture
        int resHeight = 0;      // size (borderless); 0 = the monitor's, or the default window size
        int aspect = 0;         // 0 4:3, 1 16:9, 2 stretch to window
        int filter = 1;         // 0 sharp (nearest), 1 smooth (bilinear)
        bool vsync = true;
        bool showFps = false;
        int masterVolume = 100; // 0..100
        bool muteUnfocused = true;
        bool pauseInMenu = true;
        int accentIndex = 0;    // 0..7 preset swatches, 8 = custom
        uint32_t accentCustom = 0x4C8DFFu;
        int menuOpacity = 92;   // 60..100 (%)
        int menuSide = 0;       // 0 left, 1 right
        int renderer = 0;       // 0 Vulkan GPU (falls back to software), 1 software (CPU)
        int internalScale = 0;  // GPU renderer resolution: 0 Auto (from the output size), 1 1x, 2 2x, 3 4x - live
        bool vu1Thread = true;  // GPU renderer: VIF1/VU1/GS feed on their own core (next start)
        bool recordFrames = false; // a screenshot every 0.5 s into Logs\frames (ps2_capture)
        int fovDegrees = 0;        // battle camera horizontal field of view, 0 = the game's own (tmho_game)
        int cameraDistance = 100;  // chase camera distance in percent of the game's own (tmho_game)
        // Battle HUD layout (F2 > HUD, tmho_game): per piece an offset in 0.1 % of the picture's
        // width / height from where the game puts it, and a size in percent.
        int hudRadarX = 0, hudRadarY = 0, hudRadarScale = 100;
        int hudWeaponX = 0, hudWeaponY = 0, hudWeaponScale = 100;
        int cpuLimitPercent = 0;   // testing only: cap the whole game at this share of all CPU cores (0 = off)
        int gpuFlushPrimitives = 0; // tuning (hidden): paraLLEl-GS hands queued work to the GPU after this many primitives (0 = default)
        int gpuTimestamps = 1;      // tuning (hidden): measure GPU time per stage for Graphicslogs.txt (0 = off)
        int gpuFeedBatch = 1;       // tuning (hidden): wake the GS feed thread per batch of packets (0 = per packet)
    };

    // paraLLEl-GS super-sampling rate for internalScale: 2x2 (4) and 4x4 (16) samples give a
    // high-resolution scanout at twice / four times the PS2's resolution. Auto (0) depends on the
    // output size; outputLines is the height the picture is shown at.
    inline uint32_t superSamplingFor(int internalScale, int outputLines = 0)
    {
        if (internalScale == 0)
            return outputLines > 1100 ? 16u : outputLines > 540 ? 4u : 1u;
        return internalScale >= 3 ? 16u : internalScale == 2 ? 4u : 1u;
    }

    Settings &get();

    // Any thread.
    bool menuOpen() noexcept;
    // "Quit game" (F2 > Game) was confirmed: the main loop closes the game as if the window closed.
    bool quitRequested() noexcept;
    bool emulationPaused() noexcept;

    // Before the window exists: reads settings.ini so the renderer can be chosen.
    void loadEarly(const char *iniPath);

    // What the GS renderer actually is (the menu shows it and greys out what this build lacks).
    void setRendererStatus(bool gpuActive, bool gpuAvailable, const char *deviceName);

    // Window thread, once after InitWindow/InitAudioDevice: loads the file and applies the window,
    // V-Sync and volume settings.
    void initialize(const char *iniPath);

    // Window thread, every frame before drawing: F2 toggling, menu input, window/audio state.
    // frameTexture: the texture the game picture is drawn from (for the scaling filter).
    void update(Texture *frameTexture);

    // Destination rectangle for the game picture (aspect setting applied).
    void presentRect(float sourceWidth, float sourceHeight, float screenWidth, float screenHeight,
                     float &outX, float &outY, float &outWidth, float &outHeight);

    // The size the game picture is drawn at before it is shown: the screen, or in borderless mode
    // the resolution chosen in F2 > Video (the picture is then scaled to the screen).
    void pictureTargetSize(int screenWidth, int screenHeight, int &outWidth, int &outHeight);

    // Window thread, inside BeginDrawing/EndDrawing after the game picture: FPS overlay and drawer.
    // gameSpeedPercent: emulation speed (100 = full speed), negative if unknown.
    void draw(double gameSpeedPercent);

    // The game picture's texture holds the GPU's raw BGRA bytes (drawn through a shader that swaps
    // red and blue). A copy read back from that texture (4 bytes a pixel) is put right with
    // fixFrameCopy, which does nothing when the texture holds ordinary RGBA.
    void setFrameTextureRawBgra(bool raw) noexcept;
    bool frameTextureRawBgra() noexcept;
    void fixFrameCopy(unsigned char *pixels, size_t pixelCount) noexcept;
}
