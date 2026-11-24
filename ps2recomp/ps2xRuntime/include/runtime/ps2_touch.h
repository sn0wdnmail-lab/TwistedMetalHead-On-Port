#ifndef PS2_TOUCH_H
#define PS2_TOUCH_H

// On-screen controls for touch screens (the Android version; PS2X_TOUCH=1 shows them elsewhere,
// driven by the mouse): a D-pad or analog stick, the four face buttons, L1/L2/R1/R2, Select,
// Start and a settings button. Shown during play, hidden while the F2 drawer is open and, in
// "Auto", while a controller is being used.

#include <cstdint>

namespace ps2touch
{
    // Window thread, every frame before drawing: reads the touch points.
    void update();

    // Window thread, inside BeginDrawing/EndDrawing, after the game picture and before the drawer.
    void draw();

    // Any thread (the pad is read on the game thread). Buttons held on the screen, as the pad's
    // button bits (set = held), and the analog stick when it is in use (0..255, 128 = centre).
    uint16_t heldButtons() noexcept;
    bool stick(uint8_t &x, uint8_t &y) noexcept;

    // The controls are shown at the moment (they are drawn and take touches).
    bool visible() noexcept;

    // While they are shown, a free spot for a small label (the FPS counter): just right of the
    // settings button at the top centre (left edge, vertical centre). False when hidden.
    bool besideMenuButton(float &left, float &centreY) noexcept;
}

#endif
