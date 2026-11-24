#pragma once
// Screenshot recording for bug reports and testing (F2 > Game > Record screenshots, or
// PS2X_RECORD_FRAMES=1): every half second a JPEG of the whole window - the game picture with the
// FPS counter and any menu on top - goes to Logs\frames\<session start>\. The window thread only
// copies the pixels; scaling, encoding and writing run on a worker thread. Pictures older than
// ten minutes are deleted as new ones arrive.

namespace ps2capture
{
    // Window thread, after everything is drawn and before EndDrawing().
    void afterDraw(bool recording) noexcept;

    // F12: the next game picture is saved as a full-size PNG in Screenshots\ next to the game,
    // without the FPS counter or menus.
    void requestScreenshot() noexcept;
    // Window thread, right after the game picture is drawn (before any overlay): the window
    // rectangle the picture covers, in screen coordinates.
    void afterPicture(float x, float y, float width, float height) noexcept;
    // Writes what is still queued and stops the worker.
    void shutdown() noexcept;
}
