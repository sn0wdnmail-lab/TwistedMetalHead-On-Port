#pragma once

#include <cstdint>

// Shows the game picture in the window (window thread, OpenGL).
//
// Every new frame is scaled once to the resolution chosen in F2 > Video (a sharp Lanczos filter,
// or nearest-neighbour for "Sharp" enlargements). At 120 fps / monitor rate it also finds how the
// picture moved since the previous game frame (block matching on the graphics card) and draws
// in-between frames from the two newest game frames, so motion is smooth while the game itself
// keeps running at its own 60 frames a second.
namespace ps2present
{
    // Game thread, at every VBlank: the moment (steady clock, seconds) this VBlank's frame belongs
    // to and whether it shows a new picture.
    void noteVBlank(uint64_t tick, double seconds, bool newPicture) noexcept;

    // Steady-clock seconds, the time base of noteVBlank.
    double now() noexcept;

    // Window thread, after the OpenGL context exists / before it goes away.
    void initialize();
    void shutdown();
    // The shaders built: draw() shows the picture (else the window draws the frame texture itself).
    bool ready() noexcept;

    // A frame arrived in `textureId`: `textureWidth` x `textureHeight` texels, of which the picture
    // is the top-left `width` x `height`. rawBgra: the texture holds B, G, R bytes. tick: its VBlank.
    void submitFrame(unsigned textureId, int textureWidth, int textureHeight, int width, int height, bool rawBgra,
                     uint64_t tick);

    // Draws the picture into the current framebuffer, filling dst (screen pixels, top-left origin).
    void draw(float dstX, float dstY, float dstWidth, float dstHeight);
    // After the frame has been shown: waits for the monitor's refresh when V-Sync is done through
    // the desktop compositor (windowed/borderless on Windows).
    void afterPresent();

    // The frame limit the window loop should use (0 = none, V-Sync only).
    int targetFps() noexcept;

    // Distinct pictures shown per second: game frames, plus in-between frames when they are on.
    double shownFps() noexcept;

    // In-between frames are being drawn now.
    bool interpolating() noexcept;

    // The size the picture is scaled to before it is shown (for the logs).
    void pictureSize(int &width, int &height) noexcept;
}
