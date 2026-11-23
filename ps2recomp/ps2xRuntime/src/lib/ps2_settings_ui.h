#ifndef PS2_SETTINGS_UI_H
#define PS2_SETTINGS_UI_H

// The F2 drawer's look (fonts, accent colour, text), shared with the game setup screen.
// Internal to the runtime: include only where raylib is already in use.

#include "raylib.h"

#include <cstdint>

namespace ps2settings::ui
{
    void ensureFonts();
    void text(const char *value, float x, float y, float size, Color color, bool bold);
    float textWidth(const char *value, float size, bool bold);
    Color rgb(uint32_t value, unsigned char alpha);
    uint32_t accent();      // the accent colour chosen in the UI tab
    Color inkFor(uint32_t value); // readable text colour on top of a colour
    // Input for this frame (latched by ps2settings::update), including taps released before the frame.
    bool clicked();
    bool keyTapped(int key); // no auto-repeat (Enter, Space...)
    bool keyRepeat(int key); // auto-repeats while held (arrow keys)
}

#endif
