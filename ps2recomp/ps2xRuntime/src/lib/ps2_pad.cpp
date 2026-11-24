#include "runtime/ps2_settings.h"
#include "runtime/ps2_touch.h"
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

static const auto g_padScriptEpoch = std::chrono::steady_clock::now();
// VBlanks since boot (EeScheduler.cpp): PS2X_PAD_SCRIPT_CLOCK=game times the script in game time,
// so the same script works on fast and slow machines.
extern std::atomic<uint64_t> g_ps2xVsyncCount;

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    // While the F2 settings drawer is open its keys and buttons drive the menu, not the game.
    if (ps2settings::menuOpen())
        return true;

    uint16_t btns = 0xFFFFu;
    constexpr int kGamepad = 0;
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    // PS2 button bits in the order of ps2settings::PadBind (F2 > Controls remaps keys and buttons).
    static constexpr uint16_t kBindBits[ps2settings::BindCount] = {
        PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_CROSS, PAD_CIRCLE, PAD_SQUARE, PAD_TRIANGLE,
        PAD_L1, PAD_R1, PAD_L2, PAD_R2, PAD_START, PAD_SELECT, PAD_L3, PAD_R3};
    if (useGamepad)
    {
        for (int b = 0; b < ps2settings::BindCount; ++b)
            if (IsGamepadButtonDown(kGamepad, ps2settings::bindPadButton(b)))
                clearBit(kBindBits[b]);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }
    // The keyboard works alongside a controller.
    for (int b = 0; b < ps2settings::BindCount; ++b)
        if (ps2settings::bindKeyDown(b))
            clearBit(kBindBits[b]);
    // On-screen controls (touch screens).
    {
        btns &= static_cast<uint16_t>(~ps2touch::heldButtons());
        uint8_t stickX = kPadStickCenter, stickY = kPadStickCenter;
        if (ps2touch::stick(stickX, stickY))
        {
            data[6] = stickX;
            data[7] = stickY;
        }
    }

    // PS2X_PAD_SCRIPT="t:button[:hold],..." presses buttons at fixed times (seconds since start),
    // for unattended testing. Buttons: cross circle square triangle start select up down left right l1 r1 l2 r2.
    {
        struct ScriptedPress { double at; double hold; uint16_t mask; };
        static const std::vector<ScriptedPress> script = []()
        {
            std::vector<ScriptedPress> out;
            const char *env = std::getenv("PS2X_PAD_SCRIPT");
            if (!env)
                return out;
            static const std::pair<const char *, uint16_t> names[] = {
                {"select", PAD_SELECT}, {"start", PAD_START}, {"up", PAD_UP}, {"right", PAD_RIGHT},
                {"down", PAD_DOWN}, {"left", PAD_LEFT}, {"l2", PAD_L2}, {"r2", PAD_R2}, {"l1", PAD_L1},
                {"r1", PAD_R1}, {"triangle", PAD_TRIANGLE}, {"circle", PAD_CIRCLE}, {"cross", PAD_CROSS},
                {"square", PAD_SQUARE}};
            std::string text(env);
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t end = text.find(',', pos);
                if (end == std::string::npos)
                    end = text.size();
                const std::string item = text.substr(pos, end - pos);
                pos = end + 1;
                const size_t c1 = item.find(':');
                if (c1 == std::string::npos)
                    continue;
                const size_t c2 = item.find(':', c1 + 1);
                const std::string button = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
                for (const auto &[name, bit] : names)
                {
                    if (button == name)
                    {
                        out.push_back({std::atof(item.c_str()), c2 == std::string::npos ? 0.25 : std::atof(item.c_str() + c2 + 1),
                                       bit});
                    }
                }
            }
            return out;
        }();
        if (!script.empty())
        {
            static const bool gameClock = []()
            {
                const char *env = std::getenv("PS2X_PAD_SCRIPT_CLOCK");
                return env && std::strcmp(env, "game") == 0;
            }();
            const double now = gameClock ? static_cast<double>(g_ps2xVsyncCount.load(std::memory_order_relaxed)) / 59.94
                                         : std::chrono::duration<double>(std::chrono::steady_clock::now() - g_padScriptEpoch).count();
            for (const ScriptedPress &press : script)
            {
                if (now >= press.at && now < press.at + press.hold)
                    btns &= static_cast<uint16_t>(~press.mask);
            }
        }
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);

    // PS2X_TRACE_PAD=1 prints button changes (input bring-up).
    static const bool tracePad = []()
    {
        const char *env = std::getenv("PS2X_TRACE_PAD");
        return env && *env && *env != '0';
    }();
    static uint16_t lastButtons = 0xFFFFu;
    if (tracePad && btns != lastButtons)
    {
        std::fprintf(stderr, "[pad] buttons=%04x\n", btns);
        lastButtons = btns;
    }
    return true;
}
