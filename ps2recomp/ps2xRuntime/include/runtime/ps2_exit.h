#pragma once

// Closing the game (V9.1). Whatever closes it - the window's close button, Alt+F4, the taskbar,
// F2 > Quit game, or closing the setup screen - it goes away at once: the window is hidden and
// the sound stops first, then only what must not be lost is written (settings.ini, the logs'
// session summary, and shaders compiled since the last save, given at most a second and a
// half), and the process ends without waiting for the game's threads or the graphics driver.

namespace ps2exit
{
    // Call from the window's (main) thread. Does not return.
    [[noreturn]] void now(int code = 0);

    // Watches the game window's messages for a close request (Windows: the close button, Alt+F4,
    // the taskbar, the system shutting down), so it is never missed. Call once the window exists.
    void watchWindowClose(void *window);
    // A close was asked for (besides raylib's WindowShouldClose()). Always false off Windows.
    bool closeAsked() noexcept;
}
