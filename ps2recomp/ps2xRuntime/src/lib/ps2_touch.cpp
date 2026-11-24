// On-screen controls for touch screens (see runtime/ps2_touch.h).

#include "runtime/ps2_touch.h"

#include "runtime/ps2_android.h"
#include "runtime/ps2_settings.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ps2touch
{
    namespace
    {
        // The pad's button bits (ps2_pad.cpp).
        constexpr uint16_t kLeft = 0x0080u, kDown = 0x0040u, kRight = 0x0020u, kUp = 0x0010u;
        constexpr uint16_t kStart = 0x0008u, kSelect = 0x0001u;
        constexpr uint16_t kSquare = 0x8000u, kCross = 0x4000u, kCircle = 0x2000u, kTriangle = 0x1000u;
        constexpr uint16_t kR1 = 0x0800u, kL1 = 0x0400u, kR2 = 0x0200u, kL2 = 0x0100u;

        std::atomic<uint16_t> g_held{0};
        std::atomic<uint16_t> g_stickXY{0x8080u}; // x | y << 8
        std::atomic<bool> g_stickActive{false};
        std::atomic<bool> g_visible{false};

        double g_lastControllerUse = -1000.0; // GetTime() of the last controller input
        double g_lastTouch = -1000.0;
        uint16_t g_previousHeld = 0;
        bool g_menuButtonDown = false;
        Vector2 g_knob{0.0f, 0.0f}; // analog stick knob offset from its centre, in pixels

        bool forcedByEnvironment()
        {
            static const bool on = []()
            {
                const char *env = std::getenv("PS2X_TOUCH");
                return env && *env && *env != '0';
            }();
            return on;
        }

        struct Layout
        {
            float u = 1.0f; // 1% of the screen height, times the size setting
            Vector2 left{};  // D-pad / stick centre
            float leftRadius = 0.0f;
            Vector2 face{};  // face button cluster centre
            float faceRadius = 0.0f, faceSpread = 0.0f;
            Rectangle l1{}, l2{}, r1{}, r2{}, select{}, start{};
            Vector2 menu{};
            float menuRadius = 0.0f;
        };

        Layout layout()
        {
            const float w = static_cast<float>(GetScreenWidth());
            const float h = static_cast<float>(GetScreenHeight());
            const float size = static_cast<float>(std::clamp(ps2settings::get().touchSize, 60, 150)) / 100.0f;
            Layout l;
            l.u = h * 0.01f * size;
            const float u = l.u;
            const float margin = std::max(3.0f * u, h * 0.03f);
            l.leftRadius = 19.0f * u;
            l.left = {margin + l.leftRadius + 2.0f * u, h - margin - l.leftRadius};
            l.faceRadius = 7.2f * u;
            l.faceSpread = 12.0f * u;
            l.face = {w - margin - l.faceSpread - l.faceRadius - 2.0f * u, h - margin - l.faceSpread - l.faceRadius};
            const float shoulderW = 21.0f * u, shoulderH = 9.0f * u, gap = 2.0f * u;
            l.l2 = {margin + 2.0f * u, margin, shoulderW, shoulderH};
            l.l1 = {margin + 2.0f * u, margin + shoulderH + gap, shoulderW, shoulderH};
            l.r2 = {w - margin - 2.0f * u - shoulderW, margin, shoulderW, shoulderH};
            l.r1 = {w - margin - 2.0f * u - shoulderW, margin + shoulderH + gap, shoulderW, shoulderH};
            const float pillW = 15.0f * u, pillH = 6.5f * u;
            l.select = {w * 0.5f - pillW - 2.5f * u, h - margin - pillH, pillW, pillH};
            l.start = {w * 0.5f + 2.5f * u, h - margin - pillH, pillW, pillH};
            l.menuRadius = 5.0f * u;
            l.menu = {w * 0.5f, margin + l.menuRadius};
            return l;
        }

        Vector2 faceCentre(const Layout &l, int which) // 0 triangle, 1 circle, 2 cross, 3 square
        {
            switch (which)
            {
            case 0: return {l.face.x, l.face.y - l.faceSpread};
            case 1: return {l.face.x + l.faceSpread, l.face.y};
            case 2: return {l.face.x, l.face.y + l.faceSpread};
            default: return {l.face.x - l.faceSpread, l.face.y};
            }
        }

        constexpr uint16_t kFaceBits[4] = {kTriangle, kCircle, kCross, kSquare};

        float distance(Vector2 a, Vector2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

        bool inside(Rectangle r, Vector2 p, float pad)
        {
            return p.x >= r.x - pad && p.x <= r.x + r.width + pad && p.y >= r.y - pad && p.y <= r.y + r.height + pad;
        }

        bool controllerUsedNow()
        {
            for (int pad = 0; pad < 4; ++pad)
            {
                if (!IsGamepadAvailable(pad))
                    continue;
                for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB; ++button)
                    if (IsGamepadButtonDown(pad, button))
                        return true;
                for (int axis = GAMEPAD_AXIS_LEFT_X; axis <= GAMEPAD_AXIS_RIGHT_Y; ++axis)
                    if (std::fabs(GetGamepadAxisMovement(pad, axis)) > 0.5f)
                        return true;
            }
            return false;
        }

        // Touch points this frame (the mouse, when testing on a PC).
        int touchPoints(Vector2 *points, int capacity)
        {
            if (!ps2android::active())
            {
                if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) || capacity < 1)
                    return 0;
                points[0] = GetMousePosition();
                return 1;
            }
            const int count = std::min(GetTouchPointCount(), capacity);
            for (int i = 0; i < count; ++i)
                points[i] = GetTouchPosition(i);
            return count;
        }

        Color shade(float alpha, unsigned char value = 255)
        {
            return Color{value, value, value, static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)};
        }

        void centredText(const char *text, Vector2 centre, float size, Color color)
        {
            const int fontSize = std::max(10, static_cast<int>(size));
            const int width = MeasureText(text, fontSize);
            DrawText(text, static_cast<int>(centre.x - static_cast<float>(width) * 0.5f), static_cast<int>(centre.y - static_cast<float>(fontSize) * 0.5f),
                     fontSize, color);
        }

        void faceSymbol(int which, Vector2 c, float r, Color color)
        {
            const float s = r * 0.42f;
            const float thick = std::max(2.0f, r * 0.1f);
            switch (which)
            {
            case 0: // triangle
            {
                const Vector2 a{c.x, c.y - s}, b{c.x - s * 0.95f, c.y + s * 0.65f}, d{c.x + s * 0.95f, c.y + s * 0.65f};
                DrawLineEx(a, b, thick, color);
                DrawLineEx(b, d, thick, color);
                DrawLineEx(d, a, thick, color);
                break;
            }
            case 1: // circle
                DrawRing(c, s * 0.8f - thick * 0.5f, s * 0.8f + thick * 0.5f, 0.0f, 360.0f, 36, color);
                break;
            case 2: // cross
                DrawLineEx({c.x - s * 0.75f, c.y - s * 0.75f}, {c.x + s * 0.75f, c.y + s * 0.75f}, thick, color);
                DrawLineEx({c.x - s * 0.75f, c.y + s * 0.75f}, {c.x + s * 0.75f, c.y - s * 0.75f}, thick, color);
                break;
            default: // square
                DrawRectangleLinesEx({c.x - s * 0.7f, c.y - s * 0.7f, s * 1.4f, s * 1.4f}, thick, color);
                break;
            }
        }

        void pill(Rectangle r, const char *label, bool held, float alpha, float u)
        {
            DrawRectangleRounded(r, 0.5f, 12, shade(alpha * (held ? 0.42f : 0.14f)));
            DrawRectangleRoundedLinesEx(r, 0.5f, 12, std::max(1.5f, u * 0.25f), shade(alpha * 0.55f));
            centredText(label, {r.x + r.width * 0.5f, r.y + r.height * 0.5f}, r.height * 0.45f, shade(alpha * 0.9f));
        }
    }

    void update()
    {
        const ps2settings::Settings &settings = ps2settings::get();
        const double now = GetTime();
        if (controllerUsedNow())
            g_lastControllerUse = now;

        Vector2 points[10];
        const int count = touchPoints(points, 10);
        if (count > 0)
            g_lastTouch = now;

        const bool touchScreen = ps2android::active() || forcedByEnvironment();
        bool show = false;
        if (settings.touchControls == 1)
            show = true;
        else if (settings.touchControls == 0)
            show = touchScreen && g_lastTouch >= g_lastControllerUse; // a controller hides them until the screen is touched
        if (ps2settings::menuOpen())
            show = false;
        g_visible.store(show, std::memory_order_relaxed);
        if (!show)
        {
            g_held.store(0, std::memory_order_relaxed);
            g_stickActive.store(false, std::memory_order_relaxed);
            g_previousHeld = 0;
            g_menuButtonDown = false;
            return;
        }

        const Layout l = layout();
        uint16_t held = 0;
        bool stickTouched = false;
        bool menuTouched = false;
        for (int i = 0; i < count; ++i)
        {
            const Vector2 p = points[i];
            // Left thumb: D-pad (8 directions) or analog stick.
            const float fromLeft = distance(p, l.left);
            if (fromLeft <= l.leftRadius * 1.35f)
            {
                const float dx = p.x - l.left.x, dy = p.y - l.left.y;
                if (settings.touchLeft == 1)
                {
                    stickTouched = true;
                    const float scale = fromLeft > l.leftRadius ? l.leftRadius / fromLeft : 1.0f;
                    g_knob = {dx * scale, dy * scale};
                }
                else if (fromLeft > l.leftRadius * 0.22f)
                {
                    if (dx > fromLeft * 0.38f)
                        held |= kRight;
                    if (dx < -fromLeft * 0.38f)
                        held |= kLeft;
                    if (dy > fromLeft * 0.38f)
                        held |= kDown;
                    if (dy < -fromLeft * 0.38f)
                        held |= kUp;
                }
                continue;
            }
            // Face buttons: every button the finger is on (two at once between them).
            if (distance(p, l.face) <= l.faceSpread + l.faceRadius * 1.6f)
            {
                for (int b = 0; b < 4; ++b)
                    if (distance(p, faceCentre(l, b)) <= l.faceRadius * 1.2f)
                        held |= kFaceBits[b];
                continue;
            }
            const float pad = l.u * 1.0f;
            if (inside(l.l1, p, pad))
                held |= kL1;
            else if (inside(l.l2, p, pad))
                held |= kL2;
            else if (inside(l.r1, p, pad))
                held |= kR1;
            else if (inside(l.r2, p, pad))
                held |= kR2;
            else if (inside(l.select, p, pad))
                held |= kSelect;
            else if (inside(l.start, p, pad))
                held |= kStart;
            else if (distance(p, l.menu) <= l.menuRadius * 1.5f)
                menuTouched = true;
        }

        if (settings.touchLeft == 1 && stickTouched)
        {
            const float x = 128.0f + g_knob.x / l.leftRadius * 127.0f;
            const float y = 128.0f + g_knob.y / l.leftRadius * 127.0f;
            const uint16_t xy = static_cast<uint16_t>(static_cast<uint8_t>(std::clamp(x, 0.0f, 255.0f))) |
                                static_cast<uint16_t>(static_cast<uint8_t>(std::clamp(y, 0.0f, 255.0f)) << 8);
            g_stickXY.store(xy, std::memory_order_relaxed);
            g_stickActive.store(true, std::memory_order_relaxed);
        }
        else
        {
            g_knob = {0.0f, 0.0f};
            g_stickActive.store(false, std::memory_order_relaxed);
        }

        // The settings button opens the drawer when the finger comes off it.
        if (g_menuButtonDown && !menuTouched && count == 0)
            ps2settings::openMenu();
        g_menuButtonDown = menuTouched;

        if (settings.touchVibrate && (held & ~g_previousHeld) != 0)
            ps2android::vibrate(12);
        g_previousHeld = held;
        g_held.store(held, std::memory_order_relaxed);
    }

    void draw()
    {
        if (!g_visible.load(std::memory_order_relaxed))
            return;
        const ps2settings::Settings &settings = ps2settings::get();
        const float alpha = static_cast<float>(std::clamp(settings.touchOpacity, 20, 100)) / 100.0f;
        const Layout l = layout();
        const float u = l.u;
        const uint16_t held = g_held.load(std::memory_order_relaxed);
        const float line = std::max(1.5f, u * 0.25f);

        // Left thumb.
        DrawCircleV(l.left, l.leftRadius, shade(alpha * 0.10f));
        DrawRing(l.left, l.leftRadius - line, l.leftRadius, 0.0f, 360.0f, 64, shade(alpha * 0.45f));
        if (settings.touchLeft == 1)
        {
            const Vector2 knob{l.left.x + g_knob.x, l.left.y + g_knob.y};
            DrawCircleV(knob, l.leftRadius * 0.42f, shade(alpha * (g_stickActive.load() ? 0.45f : 0.25f)));
            DrawRing(knob, l.leftRadius * 0.42f - line, l.leftRadius * 0.42f, 0.0f, 360.0f, 48, shade(alpha * 0.6f));
        }
        else
        {
            const float arm = l.leftRadius * 0.34f;
            const float reach = l.leftRadius * 0.86f;
            struct Arm { uint16_t bit; Rectangle rect; Vector2 a, b, c; };
            const Vector2 o = l.left;
            const float t = arm * 0.55f; // arrow size
            const Arm arms[4] = {
                {kUp, {o.x - arm, o.y - reach, arm * 2.0f, reach - arm * 0.2f}, {o.x, o.y - reach + t * 0.5f}, {o.x - t, o.y - reach + t * 1.6f}, {o.x + t, o.y - reach + t * 1.6f}},
                {kDown, {o.x - arm, o.y + arm * 0.2f, arm * 2.0f, reach - arm * 0.2f}, {o.x, o.y + reach - t * 0.5f}, {o.x + t, o.y + reach - t * 1.6f}, {o.x - t, o.y + reach - t * 1.6f}},
                {kLeft, {o.x - reach, o.y - arm, reach - arm * 0.2f, arm * 2.0f}, {o.x - reach + t * 0.5f, o.y}, {o.x - reach + t * 1.6f, o.y + t}, {o.x - reach + t * 1.6f, o.y - t}},
                {kRight, {o.x + arm * 0.2f, o.y - arm, reach - arm * 0.2f, arm * 2.0f}, {o.x + reach - t * 0.5f, o.y}, {o.x + reach - t * 1.6f, o.y - t}, {o.x + reach - t * 1.6f, o.y + t}},
            };
            for (const Arm &a : arms)
            {
                const bool on = (held & a.bit) != 0;
                DrawRectangleRounded(a.rect, 0.3f, 8, shade(alpha * (on ? 0.45f : 0.16f)));
                DrawTriangle(a.a, a.b, a.c, shade(alpha * 0.75f));
            }
        }

        // Face buttons.
        for (int b = 0; b < 4; ++b)
        {
            const Vector2 c = faceCentre(l, b);
            const bool on = (held & kFaceBits[b]) != 0;
            DrawCircleV(c, l.faceRadius, shade(alpha * (on ? 0.45f : 0.14f)));
            DrawRing(c, l.faceRadius - line, l.faceRadius, 0.0f, 360.0f, 48, shade(alpha * 0.55f));
            faceSymbol(b, c, l.faceRadius, shade(alpha * 0.9f));
        }

        pill(l.l2, "L2", (held & kL2) != 0, alpha, u);
        pill(l.l1, "L1", (held & kL1) != 0, alpha, u);
        pill(l.r2, "R2", (held & kR2) != 0, alpha, u);
        pill(l.r1, "R1", (held & kR1) != 0, alpha, u);
        pill(l.select, "SELECT", (held & kSelect) != 0, alpha, u);
        pill(l.start, "START", (held & kStart) != 0, alpha, u);

        // Settings (the F2 drawer): three lines in a circle.
        DrawCircleV(l.menu, l.menuRadius, shade(alpha * (g_menuButtonDown ? 0.45f : 0.14f)));
        DrawRing(l.menu, l.menuRadius - line, l.menuRadius, 0.0f, 360.0f, 40, shade(alpha * 0.55f));
        const float bar = l.menuRadius * 0.95f, gap = l.menuRadius * 0.38f, thick = std::max(2.0f, l.menuRadius * 0.14f);
        for (int i = -1; i <= 1; ++i)
            DrawLineEx({l.menu.x - bar * 0.5f, l.menu.y + gap * static_cast<float>(i)}, {l.menu.x + bar * 0.5f, l.menu.y + gap * static_cast<float>(i)}, thick,
                       shade(alpha * 0.9f));
    }

    uint16_t heldButtons() noexcept { return g_held.load(std::memory_order_relaxed); }

    bool stick(uint8_t &x, uint8_t &y) noexcept
    {
        if (!g_stickActive.load(std::memory_order_relaxed))
            return false;
        const uint16_t xy = g_stickXY.load(std::memory_order_relaxed);
        x = static_cast<uint8_t>(xy & 0xFFu);
        y = static_cast<uint8_t>(xy >> 8);
        return true;
    }

    bool visible() noexcept { return g_visible.load(std::memory_order_relaxed); }

    bool besideMenuButton(float &left, float &centreY) noexcept
    {
        if (!visible())
            return false;
        const Layout l = layout();
        left = l.menu.x + l.menuRadius + 2.0f * l.u;
        centreY = l.menu.y;
        return true;
    }
}
