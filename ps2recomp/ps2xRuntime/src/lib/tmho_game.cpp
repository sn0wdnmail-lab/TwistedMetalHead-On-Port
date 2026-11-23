// Twisted Metal: Head-On - Extra Twisted Edition (SCUS-97621) specific behaviour for the PC port:
// true 16:9 widescreen and the field-of-view setting.
//
// Widescreen follows the community PCSX2 widescreen patch for this disc (by Aced14, font fix by
// gamemasterplc): the 3D cameras get a 4:3 wider horizontal field of view with the same vertical
// one, the picture is shown 16:9, and the HUD pieces that would drift or stretch are moved back
// and narrowed. Here the changes are made where the values are handed to the game's own functions,
// so they can be switched on and off while the game runs.
//
// Game functions used (addresses in SCUS_976.21):
//   0x2668A0  setViewportFov(viewport a0, horizontal scale f12, vertical factor f13). Smaller
//             horizontal scale = wider view; the vertical scale is horizontal * factor.
//   0x19E458  placeHudModel(model a0, x f12, depth f13, y f14) - radar and weapon icons.
//   0x222660  drawText(..., x a2, ...);  0x2224A0  drawPlayerText(..., x a2, ...)
//   0x222CE8  setFontSize(font a0, width a1, height a2, ...)
//   0x265E08  cameraLookAt(viewport a0, eye a1, target a2, up a3) - points a viewport's camera
//             at a target and puts it at the eye (the battle chase cameras use it every frame).
// Viewport tables (0xC0 apart): horizontal scale at +0x00, vertical factor at +0x20.

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_settings.h"
#include "tmho_game.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    using Fn = PS2Runtime::RecompiledFunction;

    constexpr uint32_t kSetViewportFov = 0x2668A0u;
    constexpr uint32_t kPlaceHudModel = 0x19E458u;
    constexpr uint32_t kDrawText = 0x222660u;
    constexpr uint32_t kDrawPlayerText = 0x2224A0u;
    constexpr uint32_t kSetFontSize = 0x222CE8u;
    constexpr uint32_t kCameraLookAt = 0x265E08u;
    // The chase cameras behind the player's car (return addresses of their look-at calls).
    constexpr uint32_t kChaseCameraCallers[] = {0x1296ACu, 0x129ED0u};

    constexpr float kWideHorizontal = 0.75f;      // 4:3 -> 16:9: a third more picture each side
    constexpr float kWideVertical = 4.0f / 3.0f;  // keeps the vertical scale (horizontal * factor)
    constexpr float kGameHalfWidth = 320.0f;      // the game's 640-pixel-wide picture

    Fn g_setViewportFov = nullptr;
    Fn g_placeHudModel = nullptr;
    Fn g_drawText = nullptr;
    Fn g_drawPlayerText = nullptr;
    Fn g_setFontSize = nullptr;
    Fn g_cameraLookAt = nullptr;
    uint32_t g_eyeScratch = 0; // guest memory for the moved camera position (the game's own stays as it is)

    PS2Runtime *g_runtime = nullptr;
    std::atomic<bool> g_wideApplied{false}; // what the viewport tables in RAM currently hold

    // The battle camera as the game last set it up (game thread), so a new field of view or aspect
    // ratio can be applied straight away instead of at the next camera set-up.
    struct BattleCamera
    {
        bool valid = false;
        uint32_t viewport = 0;
        float rawHorizontal = 0.0f; // the game's constant, before any change here
        float rawVertical = 0.0f;   // the vertical factor without the widescreen change
    };
    BattleCamera g_battleCamera;
    std::atomic<bool> g_reapplyBattleCamera{false};

    // Viewport tables: 1P, 2P top/bottom, 2P left/right, menus and Sweet Tooth's tour cut-scenes.
    constexpr uint32_t kViewportTables[] = {0x2D0CE0u, 0x2D0DA0u, 0x2D0E60u, 0x2D0F20u, 0x2D0FE0u, 0x2D10A0u};
    float g_tableHorizontal[6] = {};
    float g_tableVertical[6] = {};
    bool g_tablesCaptured = false;

    // Callers that pass the gameplay camera's own constant (501): 1P battles, story mode (two),
    // Sweet Tooth's tour (two) and one more battle set-up. These get widescreen and the FOV setting.
    constexpr uint32_t kGameplayCallers[] = {0x1B3F34u, 0x1B6178u, 0x1B62A4u, 0x16610Cu, 0x1661A8u, 0x18DC48u};
    // Menu cameras with their own constants (vehicle select views): widescreen only.
    constexpr uint32_t kMenuCallers[] = {0x156678u, 0x15669Cu};

    // HUD icons (return address after placeHudModel, x before, x in widescreen).
    struct HudMove
    {
        uint32_t returnAddress;
        uint32_t before;
        uint32_t wide;
    };
    constexpr HudMove kHudMoves[] = {
        {0x13B4F0u, 0xBF2E147Bu, 0xBF800000u}, // 1P radar      -0.68 -> -1.0
        {0x13B59Cu, 0x3F30A3D7u, 0x3F800000u}, // 1P weapon      0.69 ->  1.0
        {0x137134u, 0xBF666666u, 0xBFA51EB8u}, // 2P (top/bottom) radar
        {0x13A5FCu, 0x3F59999Au, 0x3F9EB852u}, // 2P (top/bottom) weapon
        {0x1370ECu, 0xBEDC28F6u, 0xBF200000u}, // 2P (side by side) radar
        {0x13A5BCu, 0x3EE66666u, 0x3F230000u}, // 2P (side by side) weapon
    };

    // Text that sits next to the moved icons (return address, x before, x in widescreen).
    struct TextMove
    {
        uint32_t returnAddress;
        uint32_t before;
        uint32_t wide;
    };
    constexpr TextMove kTextMoves[] = {
        {0x13ACDCu, 0x21Au, 0x230u}, // weapon name
        {0x13AD88u, 0x21Au, 0x230u}, // 1P weapon ammo
        {0x13AD64u, 0x21Au, 0x230u}, // 2P weapon ammo
        {0x1372A4u, 0x3Cu, 0x2Du},   // 1P cars remaining
        {0x137274u, 0x2Cu, 0x21u},   // 2P cars remaining
        {0x139BE8u, 0x8Eu, 0x6Au},   // 1P names of nearby cars
        {0x139BA8u, 0x74u, 0x57u},   // 2P names of nearby cars
        {0x1E6348u, 0x226u, 0x23Du}, // 2P "Wins %d"
    };
    constexpr TextMove kPlayerTextMoves[] = {
        {0x1EF9A0u, 0x50u, 0x8Cu},
        {0x1EF9BCu, 0x50u, 0x8Cu},
        {0x1EF9F4u, 0x50u, 0x8Cu},
        {0x1EFA10u, 0x50u, 0x8Cu},
    };

    bool traceOn()
    {
        static const bool on = []()
        {
            const char *env = std::getenv("PS2X_TMHO_TRACE");
            return env && *env && *env != '0';
        }();
        return on;
    }

    bool widescreen()
    {
        return ps2settings::get().aspect == 1;
    }

    uint32_t floatBits(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    }

    float bitsFloat(uint32_t bits)
    {
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool freshEntry(const R5900Context *ctx, uint32_t address)
    {
        return ctx->pc == address; // a resumed call re-enters at one of the function's labels instead
    }

    uint32_t returnAddress(const R5900Context *ctx)
    {
        return GPR_U32(ctx, 31);
    }

    template <size_t N>
    bool contains(const uint32_t (&list)[N], uint32_t value)
    {
        for (uint32_t entry : list)
            if (entry == value)
                return true;
        return false;
    }

    // ---- viewport tables in RAM

    void captureTables(uint8_t *rdram)
    {
        if (g_tablesCaptured || !rdram)
            return;
        for (size_t i = 0; i < std::size(kViewportTables); ++i)
        {
            std::memcpy(&g_tableHorizontal[i], rdram + kViewportTables[i], 4);
            std::memcpy(&g_tableVertical[i], rdram + kViewportTables[i] + 0x20u, 4);
        }
        g_tablesCaptured = true;
    }

    void writeTables(uint8_t *rdram, bool wide)
    {
        if (!g_tablesCaptured || !rdram)
            return;
        for (size_t i = 0; i < std::size(kViewportTables); ++i)
        {
            const float horizontal = wide ? g_tableHorizontal[i] * kWideHorizontal : g_tableHorizontal[i];
            const float vertical = wide ? g_tableVertical[i] * kWideVertical : g_tableVertical[i];
            std::memcpy(rdram + kViewportTables[i], &horizontal, 4);
            std::memcpy(rdram + kViewportTables[i] + 0x20u, &vertical, 4);
        }
        g_wideApplied.store(wide);
    }

    // ---- wrappers

    float battleHorizontal(float raw)
    {
        return tmho::gameplayHorizontalScale(raw * (widescreen() ? kWideHorizontal : 1.0f));
    }

    void setViewportFov(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (freshEntry(ctx, kSetViewportFov))
        {
            const uint32_t caller = returnAddress(ctx);
            const uint32_t viewport = GPR_U32(ctx, 4);
            const bool gameplay = contains(kGameplayCallers, caller);
            const bool menu = contains(kMenuCallers, caller);
            const float before = ctx->f[12];
            if (gameplay)
            {
                // The vertical factor for these comes from the viewport tables, already adjusted.
                g_battleCamera.valid = true;
                g_battleCamera.viewport = viewport;
                g_battleCamera.rawHorizontal = ctx->f[12];
                g_battleCamera.rawVertical = ctx->f[13] / (g_wideApplied.load() ? kWideVertical : 1.0f);
                ctx->f[12] = battleHorizontal(ctx->f[12]);
            }
            else if (menu)
            {
                ctx->f[12] = ctx->f[12] * (widescreen() ? kWideHorizontal : 1.0f);
            }
            if (!gameplay && caller != 0u && g_battleCamera.valid && viewport == g_battleCamera.viewport)
                g_battleCamera.valid = false; // that viewport now shows something else
            if (traceOn())
                std::fprintf(stderr, "[tmho] setViewportFov vp=%u caller=%06x x=%.2f->%.2f y=%.4f\n", GPR_U32(ctx, 4), caller, before,
                             ctx->f[12], ctx->f[13]);
        }
        g_setViewportFov(rdram, ctx, runtime);
    }

    // Game thread (from the per-frame HUD calls): runs the game's own set-up again for the battle
    // camera with the new values, through the scheduler like any other call the runtime makes.
    void reapplyBattleCamera(const R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_reapplyBattleCamera.load(std::memory_order_relaxed) || !g_reapplyBattleCamera.exchange(false))
            return;
        if (!g_battleCamera.valid || !runtime)
            return;
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::HleCall;
        invocation.context = *ctx;
        invocation.context.pc = kSetViewportFov;
        SET_GPR_U32(&invocation.context, 4, g_battleCamera.viewport);
        SET_GPR_U32(&invocation.context, 29, 0u); // the scheduler gives it a stack
        SET_GPR_U32(&invocation.context, 31, 0u); // returns to the scheduler (caller 0: no change here)
        invocation.context.f[12] = battleHorizontal(g_battleCamera.rawHorizontal);
        invocation.context.f[13] = g_battleCamera.rawVertical * (widescreen() ? kWideVertical : 1.0f);
        if (traceOn())
            std::fprintf(stderr, "[tmho] re-applying the battle camera: vp=%u x=%.2f y=%.4f\n", g_battleCamera.viewport,
                         invocation.context.f[12], invocation.context.f[13]);
        runtime->eeScheduler().queueInvocation(std::move(invocation));
    }

    // The radar and weapon icons are 3D models seen through the battle camera, so a different field
    // of view would shrink them and pull them toward the middle. Moving them nearer by the same
    // ratio keeps their place and size on screen: x' = X*k * x / (z*k).
    float hudDepthScale()
    {
        if (ps2settings::get().fovDegrees <= 0)
            return 1.0f;
        const float gameScale = 501.0f * (widescreen() ? kWideHorizontal : 1.0f);
        return tmho::gameplayHorizontalScale(gameScale) / gameScale;
    }

    void placeHudModel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        reapplyBattleCamera(ctx, runtime);
        if (freshEntry(ctx, kPlaceHudModel))
        {
            const uint32_t caller = returnAddress(ctx);
            bool hud = false;
            for (const HudMove &move : kHudMoves)
                if (move.returnAddress == caller)
                {
                    hud = true;
                    if (widescreen() && move.before == floatBits(ctx->f[12]))
                        ctx->f[12] = bitsFloat(move.wide);
                }
            if (hud)
                ctx->f[13] *= hudDepthScale();
        }
        g_placeHudModel(rdram, ctx, runtime);
    }

    template <size_t N>
    void moveTextX(R5900Context *ctx, const TextMove (&moves)[N])
    {
        const uint32_t caller = returnAddress(ctx);
        const uint32_t x = GPR_U32(ctx, 6);
        for (const TextMove &move : moves)
            if (move.returnAddress == caller && move.before == x)
                SET_GPR_U32(ctx, 6, move.wide);
    }

    void drawText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (freshEntry(ctx, kDrawText))
            reapplyBattleCamera(ctx, runtime);
        if (freshEntry(ctx, kDrawText) && widescreen())
            moveTextX(ctx, kTextMoves);
        g_drawText(rdram, ctx, runtime);
    }

    void drawPlayerText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (freshEntry(ctx, kDrawPlayerText) && widescreen())
            moveTextX(ctx, kPlayerTextMoves);
        g_drawPlayerText(rdram, ctx, runtime);
    }

    void setFontSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (freshEntry(ctx, kSetFontSize) && widescreen())
        {
            // Three quarters of the width (as width/2 + width/4, like the patch), so letters look
            // right once the picture is shown 16:9.
            const int32_t width = static_cast<int32_t>(GPR_U32(ctx, 5));
            SET_GPR_U32(ctx, 5, static_cast<uint32_t>((width >> 1) + (width >> 2)));
        }
        g_setFontSize(rdram, ctx, runtime);
    }

    void cameraLookAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (freshEntry(ctx, kCameraLookAt) && g_eyeScratch != 0u && contains(kChaseCameraCallers, returnAddress(ctx)))
        {
            const int percent = ps2settings::get().cameraDistance;
            const uint8_t *eye = getMemPtr(rdram, GPR_U32(ctx, 5));
            const uint8_t *target = getMemPtr(rdram, GPR_U32(ctx, 6));
            uint8_t *scratch = getMemPtr(rdram, g_eyeScratch);
            if (percent != 100 && eye && target && scratch)
            {
                // Same direction from the target, distance scaled: closer below 100 %, further above.
                float e[4], t[4];
                std::memcpy(e, eye, sizeof(e));
                std::memcpy(t, target, sizeof(t));
                const float k = static_cast<float>(percent) / 100.0f;
                for (int i = 0; i < 3; ++i)
                    e[i] = t[i] + (e[i] - t[i]) * k;
                std::memcpy(scratch, e, sizeof(e));
                SET_GPR_U32(ctx, 5, g_eyeScratch);
                static int traced = 0;
                if (traceOn() && traced < 5)
                {
                    ++traced;
                    std::fprintf(stderr, "[tmho] chase camera caller=%06x target=(%.2f %.2f %.2f) eye moved to (%.2f %.2f %.2f), %d%%\n",
                                 returnAddress(ctx), t[0], t[1], t[2], e[0], e[1], e[2], percent);
                }
            }
        }
        g_cameraLookAt(rdram, ctx, runtime);
    }

    bool hook(PS2Runtime &runtime, uint32_t address, Fn wrapper, Fn &original)
    {
        original = runtime.lookupFunction(address);
        if (!original)
            return false;
        return runtime.replaceFunction(address, wrapper);
    }

    void apply(PS2Runtime &runtime)
    {
        g_runtime = &runtime;
        bool ok = true;
        ok &= hook(runtime, kSetViewportFov, &setViewportFov, g_setViewportFov);
        ok &= hook(runtime, kPlaceHudModel, &placeHudModel, g_placeHudModel);
        ok &= hook(runtime, kDrawText, &drawText, g_drawText);
        ok &= hook(runtime, kDrawPlayerText, &drawPlayerText, g_drawPlayerText);
        ok &= hook(runtime, kSetFontSize, &setFontSize, g_setFontSize);
        ok &= hook(runtime, kCameraLookAt, &cameraLookAt, g_cameraLookAt);
        // 16 bytes in the HLE kernel's unused low memory (callback stacks end at 0x70000).
        g_eyeScratch = 0x0007FF00u;
        uint8_t *rdram = runtime.memory().getRDRAM();
        captureTables(rdram);
        writeTables(rdram, widescreen());
        std::fprintf(stderr, "[tmho] widescreen, field-of-view and camera-distance hooks %s\n", ok ? "installed" : "INCOMPLETE");
    }
}

namespace tmho
{
    float defaultHorizontalFov()
    {
        const float scale = 501.0f * (widescreen() ? kWideHorizontal : 1.0f);
        return 2.0f * std::atan(kGameHalfWidth / scale) * 57.29578f;
    }

    float gameplayHorizontalScale(float scale)
    {
        const int degrees = ps2settings::get().fovDegrees;
        if (degrees <= 0)
            return scale;
        const float radians = static_cast<float>(degrees) * 0.5f / 57.29578f;
        return kGameHalfWidth / std::tan(radians);
    }

    void settingsChanged()
    {
        if (!g_runtime)
            return;
        const bool wide = widescreen();
        if (wide != g_wideApplied.load())
            writeTables(g_runtime->memory().getRDRAM(), wide);
        g_reapplyBattleCamera.store(true);
    }
}

// The generated code in this runtime is this game's, so only the entry point is checked.
PS2_REGISTER_GAME_OVERRIDE("TMHO widescreen and field of view", "", 0x100008u, 0u, &apply)
