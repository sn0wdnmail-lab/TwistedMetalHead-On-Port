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
        int resWidth = 0;       // older files only: window size / display mode (now from `resolution`)
        int resHeight = 0;
        int resolution = -1;    // picture resolution: 0 480p (1x), 1 720p, 2 1080p, 3 1440p, 4 2160p; -1 = not chosen yet
        int frameRate = 0;      // 0 60 fps (the game's frames), 1 120 fps, 2 the monitor's rate (in-between frames)
        int aspect = 0;         // 0 4:3, 1 16:9, 2 stretch to window
        int filter = 1;         // 0 sharp (nearest), 1 smooth (bilinear)
        bool vsync = true;
        bool showFps = false;
        int masterVolume = 100; // 0..100
        bool muteUnfocused = true;
        bool pauseInMenu = true;
        bool pauseUnfocused = false; // stop the game while its window is in the background
        int accentIndex = 0;    // 0..7 preset swatches, 8 = custom
        uint32_t accentCustom = 0x4C8DFFu;
        int menuOpacity = 92;   // 60..100 (%)
        int menuSide = 0;       // 0 left, 1 right
        int renderer = 0;       // 0 Vulkan GPU (falls back to software), 1 software (CPU)
        int internalScale = 0;  // older files only (internal_res): 0 Auto, 1 1x, 2 2x, 3 4x
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
        int audioDump = 0;          // testing (hidden): write the SPU2 mix and the sound card's audio to Logs\audio_*.raw
        int gsDownloadPrediction = 1; // tuning (hidden): answer small per-frame GS downloads from the frame before
    };

    // Lines of each picture resolution (F2 > Video > Resolution).
    inline int resolutionLines(int resolution)
    {
        static const int lines[] = {480, 720, 1080, 1440, 2160};
        return lines[resolution < 0 ? 2 : resolution > 4 ? 4 : resolution];
    }

    // paraLLEl-GS super-sampling for a picture resolution: 2x2 (4) and 4x4 (16) samples draw the
    // game at twice / four times the PS2's 448 lines (896 / 1792), which is then scaled to the
    // chosen lines. 720p and 1080p share 2x (light enough for weak graphics cards), 1440p and
    // 2160p use 4x.
    inline uint32_t superSamplingForResolution(int resolution)
    {
        return resolution <= 0 ? 1u : resolution <= 2 ? 4u : 16u;
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
    // True when V-Sync is done by waiting for the desktop compositor after each frame (Windows,
    // windowed or borderless); see ps2present::afterPresent.
    bool compositorSync() noexcept;
    void fixFrameCopy(unsigned char *pixels, size_t pixelCount) noexcept;

    // A short message at the top of the window for a few seconds ("Screenshot saved: ...").
    // Window thread.
    void notify(const char *text) noexcept;
}
