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
#include "ps2_movie_state.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    using Fn = PS2Runtime::RecompiledFunction;

    constexpr uint32_t kSetViewportFov = 0x2668A0u;
    constexpr uint32_t kPlaceHudModel = 0x19E458u;
    constexpr uint32_t kPlaceHudInstance = 0x19E580u; // each frame: a HUD model's instance in front of its camera
    constexpr uint32_t kHudCameras = 0x606740u;       // 0x140-byte views; +0x100 points at the view's camera
    constexpr uint32_t kDrawText = 0x222660u;
    constexpr uint32_t kDrawPlayerText = 0x2224A0u;
    constexpr uint32_t kSetFontSize = 0x222CE8u;
    constexpr uint32_t kCameraLookAt = 0x265E08u;
    constexpr uint32_t kPlayMovie = 0x100618u;        // (context, file name, ...): LEGAL.PSS / ESPLOGO.PSS at start
    constexpr uint32_t kStartFrontendMovie = 0x226840u; // (movie object, path): the intro behind the title
    // The chase cameras behind the player's car (return addresses of their look-at calls).
    constexpr uint32_t kChaseCameraCallers[] = {0x1296ACu, 0x129ED0u};

    constexpr float kWideHorizontal = 0.75f;      // 4:3 -> 16:9: a third more picture each side
    constexpr float kWideVertical = 4.0f / 3.0f;  // keeps the vertical scale (horizontal * factor)
    constexpr float kGameHalfWidth = 320.0f;      // the game's 640-pixel-wide picture

    Fn g_setViewportFov = nullptr;
    Fn g_placeHudModel = nullptr;
    Fn g_placeHudInstance = nullptr;
    Fn g_drawText = nullptr;
    Fn g_drawPlayerText = nullptr;
    Fn g_setFontSize = nullptr;
    Fn g_cameraLookAt = nullptr;
    Fn g_playMovie = nullptr;
    Fn g_startFrontendMovie = nullptr;
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

    // ---- Battle HUD layout (F2 > HUD) and the field-of-view correction
    // A HUD model's place on screen is linear in its x / y and follows the battle camera's scale
    // (its size follows scale / depth): x lands at 320 + (2/3) * scale * x of the 640-pixel picture.
    // The 2/3 comes from the widescreen patch's own pairs of model and text positions.
    constexpr uint32_t kRadarModel1P = 0x13B4F0u;
    constexpr uint32_t kWeaponModel1P = 0x13B59Cu;
    // The weapon panels (1P, 2P top/bottom, 2P side by side): drawn as the game placed them, then slid.
    constexpr uint32_t kWeaponModels[] = {kWeaponModel1P, 0x13A5FCu, 0x13A5BCu};
    // The rest of the 1P battle HUD's models (centre markers): field-of-view correction only.
    constexpr uint32_t kCentreModels1P[] = {0x13B60Cu, 0x13B650u, 0x13B694u, 0x13B6D0u};
    // The texts that belong to each piece (return address after drawText).
    constexpr uint32_t kRadarTexts[] = {0x1372A4u /* cars left */, 0x139BE8u /* target's name */};
    constexpr uint32_t kWeaponTexts[] = {0x13ACDCu /* weapon name */, 0x13AD88u /* ammo */};
    // The HUD's numbers: 1P and 2P ammo, 1P and 2P cars left.
    constexpr uint32_t kNumberTexts[] = {0x13AD88u, 0x13AD64u, 0x1372A4u, 0x137274u};
    // The game's fonts: 0x44-byte records with width +0x08, height +0x0A and the two spacings.
    constexpr uint32_t kFontTable = 0x3999A0u;
    constexpr uint32_t kFontRecordSize = 0x44u;
    // The two pieces' model positions (4:3 x, widescreen x, y) as the game places them.
    struct PieceBase
    {
        float x43, x169, y;
    };
    constexpr PieceBase kPieceBase[] = {{-0.68f, -1.0f, 0.575f}, {0.69f, 1.0f, -0.6f}};
    // Each piece's box around its model's centre at the default size, in picture heights
    // (left, top, right, bottom), measured on screen: the same in 4:3 and 16:9.
    constexpr float kPieceExtent[][4] = {{-0.143f, -0.164f, 0.222f, 0.097f}, {-0.136f, -0.132f, 0.115f, 0.125f}};

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

    // PS2X_HUD_TRACE=1: every distinct HUD call site once (models and text), for mapping the HUD.
    bool hudTraceOn()
    {
        static const bool on = []()
        {
            const char *env = std::getenv("PS2X_HUD_TRACE");
            return env && *env && *env != '0';
        }();
        return on;
    }

    void hudTrace(const char *what, uint32_t caller, const char *details)
    {
        static std::mutex mutex;
        static std::set<std::pair<uint32_t, std::string>> seen;
        std::lock_guard<std::mutex> lock(mutex);
        if (seen.size() < 400 && seen.insert({caller, details}).second)
            std::fprintf(stderr, "[hud] %s caller=%06x %s\n", what, caller, details);
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

    // The battle camera's scale with the field-of-view setting, relative to the game's own. A HUD
    // model's place follows the camera's scale (x and y), its size follows scale / depth, so with
    // a wider view the HUD would shrink and slide toward the middle: x / k, y / k and depth * k
    // keep both. (Only moving it nearer, as before, kept the size but not the place.)
    float hudFovRatio()
    {
        if (ps2settings::get().fovDegrees <= 0)
            return 1.0f;
        const float gameScale = 501.0f * (widescreen() ? kWideHorizontal : 1.0f);
        return tmho::gameplayHorizontalScale(gameScale) / gameScale;
    }

    // Picture pixels (of 640) per model unit across, and frame lines (of 448) per unit up, with
    // the game's own camera. PS2X_HUD_KX / PS2X_HUD_KY override them (calibration).
    float hudUnitsX()
    {
        static const float overrideX = []() { const char *e = std::getenv("PS2X_HUD_KX"); return e ? static_cast<float>(std::atof(e)) : 0.0f; }();
        const float base = overrideX > 0.0f ? overrideX : (2.0f / 3.0f) * 501.0f;
        return base * (widescreen() ? kWideHorizontal : 1.0f);
    }

    float hudUnitsY()
    {
        static const float overrideY = []() { const char *e = std::getenv("PS2X_HUD_KY"); return e ? static_cast<float>(std::atof(e)) : 0.0f; }();
        return overrideY > 0.0f ? overrideY : 230.0f;
    }

    struct PieceLayout
    {
        float dx = 0.0f; // fraction of the picture's width
        float dy = 0.0f; // fraction of its height (down)
        float scale = 1.0f;
    };

    // In 16:9 the default layout puts the radar and the weapon panel right at the screen edges
    // (a margin of 1.5 % of the picture's height on every side) instead of where the widescreen
    // patch leaves them; the player's offsets (F2 > HUD) are added to this.
    void edgeOffset(tmho::HudPiece piece, float &dx, float &dy)
    {
        dx = dy = 0.0f;
        if (!widescreen())
            return;
        const tmho::HudBox box = tmho::hudDefaultBox(piece);
        const float marginY = 0.015f, marginX = 0.015f * 9.0f / 16.0f;
        if (piece == tmho::HudPiece::Radar)
        {
            dx = marginX - box.x;
            dy = marginY - box.y;
        }
        else
        {
            dx = 1.0f - marginX - (box.x + box.width);
            dy = 1.0f - marginY - (box.y + box.height);
        }
    }

    PieceLayout pieceLayout(tmho::HudPiece piece)
    {
        const ps2settings::Settings &s = ps2settings::get();
        PieceLayout layout;
        edgeOffset(piece, layout.dx, layout.dy);
        if (piece == tmho::HudPiece::Radar)
        {
            layout.dx += static_cast<float>(s.hudRadarX) / 1000.0f;
            layout.dy += static_cast<float>(s.hudRadarY) / 1000.0f;
            layout.scale = static_cast<float>(s.hudRadarScale) / 100.0f;
        }
        else
        {
            layout.dx += static_cast<float>(s.hudWeaponX) / 1000.0f;
            layout.dy += static_cast<float>(s.hudWeaponY) / 1000.0f;
            layout.scale = static_cast<float>(s.hudWeaponScale) / 100.0f;
        }
        return layout;
    }

    // Where a piece's model centre lands with the default layout: picture pixels across (of 640)
    // and frame lines down (of 448).
    void pieceAnchor(tmho::HudPiece piece, float &x, float &y)
    {
        const PieceBase &base = kPieceBase[static_cast<int>(piece)];
        x = 320.0f + hudUnitsX() * (widescreen() ? base.x169 : base.x43);
        y = 224.0f - hudUnitsY() * base.y;
    }

    std::atomic<double> g_hudShownAt{-1.0}; // steady-clock seconds of the last radar draw

    double steadySeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // The game places each HUD model once, when the HUD is set up (a battle starts), and draws it
    // from the model's own record every frame. A layout or field-of-view change in the middle of a
    // battle therefore rewrites those records, if they still hold what was put there.
    struct HudModelEntry
    {
        uint32_t caller = 0;
        uint32_t record = 0;               // guest address of the model (x +0x10, depth +0x14, y +0x18)
        float baseX = 0.0f, baseDepth = 0.0f, baseY = 0.0f; // the game's values (widescreen-moved)
        uint32_t written[3] = {};          // what is in the record now (bit patterns)
        uint32_t copy = 0;                 // where the game's set-up copied the three values (0 unknown, 1 none)
    };
    HudModelEntry g_hudModels[12];
    std::atomic<bool> g_relayoutPending{false};

    // The player's layout and the field-of-view correction for one model (base -> placed values).
    // Returns false for models this does not touch.
    bool layoutModel(uint32_t caller, float &x, float &depth, float &y)
    {
        bool hud = false;
        for (const HudMove &move : kHudMoves)
            if (move.returnAddress == caller)
                hud = true;
        const bool radar = caller == kRadarModel1P;
        if (radar || caller == kWeaponModel1P)
        {
            // Moved by a share of the picture, sized around its own centre (nearer = bigger; the
            // place on screen does not depend on depth).
            const PieceLayout layout = pieceLayout(radar ? tmho::HudPiece::Radar : tmho::HudPiece::Weapon);
            x += layout.dx * 640.0f / hudUnitsX();
            y -= layout.dy * 448.0f / hudUnitsY();
            depth /= std::max(layout.scale, 0.1f);
        }
        if (radar || caller == kWeaponModel1P || contains(kCentreModels1P, caller))
        {
            // Field of view: keep every 1P HUD model's place and size.
            const float k = hudFovRatio();
            x /= k;
            y /= k;
            depth *= k;
            return true;
        }
        if (hud)
        {
            depth *= hudFovRatio(); // 2P HUD: size only, as before
            return true;
        }
        return false;
    }

    void rememberModel(uint32_t caller, uint32_t record, float baseX, float baseDepth, float baseY, float x, float depth, float y)
    {
        // One slot per model: the game sets the battle HUD up more than once (two weapon panels
        // 0x26E0 bytes apart), and each needs moving when the layout changes.
        HudModelEntry *slot = nullptr;
        for (HudModelEntry &entry : g_hudModels)
            if (!slot && entry.caller == 0u)
                slot = &entry;
        for (HudModelEntry &entry : g_hudModels)
            if (entry.caller != 0u && entry.record == record)
                slot = &entry;
        if (!slot)
        {
            static size_t next = 0; // all in use (HUDs of earlier battles): reuse the oldest
            slot = &g_hudModels[next];
            next = (next + 1) % std::size(g_hudModels);
        }
        slot->caller = caller;
        slot->record = record;
        slot->baseX = baseX;
        slot->baseDepth = baseDepth;
        slot->baseY = baseY;
        slot->written[0] = floatBits(x);
        slot->written[1] = floatBits(depth);
        slot->written[2] = floatBits(y);
        slot->copy = 0u;
    }

    // The HUD set-up copies a model's place into the instance it draws from (0x26D0 bytes below the
    // radar's record, for example). Found once by its exact three values, near the record.
    uint32_t findModelCopy(uint8_t *rdram, const HudModelEntry &entry)
    {
        constexpr uint32_t kWindow = 512u * 1024u;
        const uint32_t own = entry.record + 0x10u;
        const uint32_t from = own > kWindow ? (own - kWindow) & ~3u : 0u;
        const uint32_t to = std::min<uint32_t>(own + kWindow, 32u * 1024u * 1024u - 12u);
        const uint8_t *base = getMemPtr(rdram, 0u);
        if (!base)
            return 1u;
        for (uint32_t a = from; a <= to; a += 4u)
        {
            if (a == own || std::memcmp(base + a, entry.written, sizeof(entry.written)) != 0)
                continue;
            return a;
        }
        return 1u;
    }

    // Game thread: puts the HUD models where the current settings want them.
    void relayoutHud(uint8_t *rdram)
    {
        if (!g_relayoutPending.load(std::memory_order_relaxed) || !g_relayoutPending.exchange(false))
            return;
        for (HudModelEntry &entry : g_hudModels)
        {
            if (entry.caller == 0u)
                continue;
            uint8_t *record = getMemPtr(rdram, entry.record + 0x10u);
            if (!record)
                continue;
            uint32_t now[3];
            std::memcpy(now, record, sizeof(now));
            if (hudTraceOn())
                std::fprintf(stderr, "[hud] relayout %06x record %08x now %.3f %.3f %.3f written %.3f %.3f %.3f\n", entry.caller, entry.record,
                             bitsFloat(now[0]), bitsFloat(now[1]), bitsFloat(now[2]), bitsFloat(entry.written[0]), bitsFloat(entry.written[1]),
                             bitsFloat(entry.written[2]));
            if (std::memcmp(now, entry.written, sizeof(now)) != 0)
            {
                entry.caller = 0u; // the game has reused or moved it: leave it alone
                continue;
            }
            float x = entry.baseX, depth = entry.baseDepth, y = entry.baseY;
            if (!layoutModel(entry.caller, x, depth, y))
                continue;
            if (entry.copy == 0u)
                entry.copy = findModelCopy(rdram, entry);
            uint8_t *copy = entry.copy > 1u ? getMemPtr(rdram, entry.copy) : nullptr;
            if (copy && std::memcmp(copy, entry.written, sizeof(entry.written)) != 0)
                copy = nullptr; // changed since: not ours to touch any more
            entry.written[0] = floatBits(x);
            entry.written[1] = floatBits(depth);
            entry.written[2] = floatBits(y);
            std::memcpy(record, entry.written, sizeof(entry.written));
            if (copy)
                std::memcpy(copy, entry.written, sizeof(entry.written));
            else
                entry.copy = 1u;
        }
    }

    void rememberDesign(uint32_t caller, uint32_t holder, float gameX, float gameDepth, float gameY);

    void placeHudModel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        reapplyBattleCamera(ctx, runtime);
        if (freshEntry(ctx, kPlaceHudModel))
        {
            const uint32_t caller = returnAddress(ctx);
            rememberDesign(caller, GPR_U32(ctx, 4), ctx->f[12], ctx->f[13], ctx->f[14]);
            bool hud = false;
            for (const HudMove &move : kHudMoves)
                if (move.returnAddress == caller)
                {
                    hud = true;
                    if (widescreen() && move.before == floatBits(ctx->f[12]))
                        ctx->f[12] = bitsFloat(move.wide);
                }
            const float baseX = ctx->f[12], baseDepth = ctx->f[13], baseY = ctx->f[14];
            float x = baseX, depth = baseDepth, y = baseY;
            if (layoutModel(caller, x, depth, y))
            {
                ctx->f[12] = x;
                ctx->f[13] = depth;
                ctx->f[14] = y;
                rememberModel(caller, GPR_U32(ctx, 4), baseX, baseDepth, baseY, x, depth, y);
            }
            if (hudTraceOn())
            {
                char details[128];
                std::snprintf(details, sizeof(details), "x=%.3f depth=%.3f y=%.3f record=%08x%s", ctx->f[12], ctx->f[13], ctx->f[14],
                              GPR_U32(ctx, 4), hud ? " (moved)" : "");
                hudTrace("model", caller, details);
            }
        }
        g_placeHudModel(rdram, ctx, runtime);
    }

    // ---- Moved HUD models keep the look the game gave them
    // Each frame the game puts a HUD model's instance at camera + rotation * (x * depth / 1.5,
    // depth, y * depth / 2), facing the way the camera does (func_0019E580), so its place on screen
    // is x / 1.5 and y / 2 across and up from the middle. The weapon panel's parts lie at very
    // different depths behind that point (the frame about one panel-depth further back, the
    // weapon icon further still), so placing it elsewhere, or nearer to make it bigger, shows it
    // from another angle: the icon drifts off the middle of its frame (by a third of the icon in
    // the 16:9 corner). The weapon panels are therefore drawn exactly as the game placed them -
    // its x, y and depth - and the picture of it is then scaled (m) and slid (tx, tz) on screen as
    // a whole. In camera space that is the linear map (X, D, Y) -> (m X + tx D, D, m Y + tz D),
    // folded into the instance's matrix: right and up axes times m, depth axis tilted by
    // (tx, tz). The model's origin lands where the layout puts it, at the size it asks for.
    struct DesignedModel
    {
        uint32_t holder = 0; // guest address of the HUD model (its place: x +0x10, depth +0x14, y +0x18)
        float x = 0.0f, depth = 0.0f, y = 0.0f; // where the game itself put it (before any change here)
    };
    DesignedModel g_designedModels[8];

    void rememberDesign(uint32_t caller, uint32_t holder, float gameX, float gameDepth, float gameY)
    {
        for (DesignedModel &model : g_designedModels)
            if (model.holder == holder)
                model.holder = 0u;
        if (!contains(kWeaponModels, caller) || !(gameDepth > 0.0f))
            return;
        for (DesignedModel &model : g_designedModels)
            if (model.holder == 0u)
            {
                model.holder = holder;
                model.x = gameX;
                model.depth = gameDepth;
                model.y = gameY;
                return;
            }
    }

    bool readFloats(uint8_t *rdram, uint32_t address, float *out, int count)
    {
        const uint8_t *p = getMemPtr(rdram, address);
        if (!p || address + static_cast<uint32_t>(count) * 4u > PS2_RAM_SIZE)
            return false;
        std::memcpy(out, p, static_cast<size_t>(count) * 4u);
        return true;
    }

    bool writeFloats(uint8_t *rdram, uint32_t address, const float *in, int count)
    {
        uint8_t *p = getMemPtr(rdram, address);
        if (!p || address + static_cast<uint32_t>(count) * 4u > PS2_RAM_SIZE)
            return false;
        std::memcpy(p, in, static_cast<size_t>(count) * 4u);
        return true;
    }

    uint32_t readWord(uint8_t *rdram, uint32_t address)
    {
        uint32_t word = 0;
        if (const uint8_t *p = getMemPtr(rdram, address))
            std::memcpy(&word, p, 4);
        return word;
    }

    // Does what func_0019E580 does, for one of the weapon panels, with the scale and slide above.
    // Returns false (leaving it to the game) when the model cannot be handled here.
    bool placeAsDesigned(uint8_t *rdram, R5900Context *ctx, const DesignedModel &design)
    {
        const uint32_t holder = GPR_U32(ctx, 4);
        const uint32_t instance = GPR_U32(ctx, 5);
        float turn[3];
        // A model with its own turn has its matrix rebuilt from angles afterwards, which would undo this.
        if (!readFloats(rdram, holder + 0x20u, turn, 3) || turn[0] != 0.0f || turn[1] != 0.0f || turn[2] != 0.0f)
            return false;
        const uint32_t description = readWord(rdram, holder + 0x30u);
        if (description < 0x00100000u || description >= PS2_RAM_SIZE)
            return false;
        const uint32_t view = readWord(rdram, description + 0x50u);
        if (view > 16u)
            return false;
        const uint32_t camera = readWord(rdram, kHudCameras + view * 0x140u + 0x100u);
        float rows[3][4], eye[4];
        if (camera < 0x00100000u || camera >= PS2_RAM_SIZE || !readFloats(rdram, camera + 0x40u, &rows[0][0], 12) ||
            !readFloats(rdram, camera + 0x20u, eye, 4))
            return false;
        // Where this frame's call wants it (the layout's place and size) ...
        const float depth = ctx->f[12], x = ctx->f[13], y = ctx->f[14];
        if (!(depth > 0.0f))
            return false;
        // ... and the scale and slide that put the game's own picture of it there.
        const float m = design.depth / depth;
        const float tx = (x - m * design.x) / 1.5f;
        const float tz = (y - m * design.y) * 0.5f;
        // The origin goes where the game would put it for this x and y: (D x / 1.5, D, D y / 2) in
        // camera space with the game's depth D. The drawing code reaches it through the matrix:
        // it takes the instance's place relative to the camera into model space with the
        // transpose of the matrix (q = A p, fine for a pure rotation) and draws model + q through
        // A, so the origin ends up at p^T A^T A. With A = Ac R (Ac the map above, R the camera's
        // rotation) the place to store is p = R^T (Ac^T Ac)^-1 d.
        const float d[3] = {design.depth / 1.5f * x, design.depth, design.depth * 0.5f * y};
        const float g[3][3] = {{m * m + tx * tx, tx, tx * tz}, {tx, 1.0f, tz}, {tx * tz, tz, m * m + tz * tz}};
        const float det = g[0][0] * (g[1][1] * g[2][2] - g[1][2] * g[2][1]) - g[0][1] * (g[1][0] * g[2][2] - g[1][2] * g[2][0]) +
                          g[0][2] * (g[1][0] * g[2][1] - g[1][1] * g[2][0]);
        if (!(std::fabs(det) > 1e-6f))
            return false;
        float local[3]; // Cramer's rule: column i replaced by d
        for (int i = 0; i < 3; ++i)
        {
            float c[3][3];
            std::memcpy(c, g, sizeof(c));
            for (int r = 0; r < 3; ++r)
                c[r][i] = d[r];
            local[i] = (c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) - c[0][1] * (c[1][0] * c[2][2] - c[1][2] * c[2][0]) +
                        c[0][2] * (c[1][0] * c[2][1] - c[1][1] * c[2][0])) / det;
        }
        float position[4];
        for (int i = 0; i < 3; ++i)
            position[i] = rows[0][i] * local[0] + rows[1][i] * local[1] + rows[2][i] * local[2] + eye[i];
        position[3] = 0.0f;
        float axes[3][4];
        for (int i = 0; i < 3; ++i)
        {
            axes[0][i] = m * rows[0][i];
            axes[1][i] = rows[1][i] + tx * rows[0][i] + tz * rows[2][i];
            axes[2][i] = m * rows[2][i];
        }
        return writeFloats(rdram, instance + 0x40u, axes[0], 3) && writeFloats(rdram, instance + 0x50u, axes[1], 3) &&
               writeFloats(rdram, instance + 0x60u, axes[2], 3) && writeFloats(rdram, instance + 0x20u, position, 4);
    }

    void placeHudInstance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (freshEntry(ctx, kPlaceHudInstance))
        {
            const uint32_t holder = GPR_U32(ctx, 4);
            for (const DesignedModel &design : g_designedModels)
                if (design.holder != 0u && design.holder == holder && placeAsDesigned(rdram, ctx, design))
                {
                    ctx->pc = returnAddress(ctx);
                    return;
                }
        }
        g_placeHudInstance(rdram, ctx, runtime);
    }

    // A piece's text follows its model: moved by the same share of the picture and sized around
    // the model's centre, with the font scaled for this one call.
    struct FontScale
    {
        uint8_t *record = nullptr;
        uint32_t index = 0;
        uint16_t scaled[4] = {};
    };

    // A font record (one of eight) while calls draw with a size of ours in it.
    struct FontRecordUse
    {
        uint16_t game[4] = {};    // the game's own width, height, line step and advance
        uint16_t written[4] = {}; // what we last put in the record
        uint32_t calls = 0;       // calls drawing with our size (more than one when one stopped part-way)
    };
    FontRecordUse g_fontUses[8];

    // A text call that stopped part-way through its string for a checkpoint (a VBlank or timer
    // interrupt) and resumes later inside the function. Its font keeps the size set for it until
    // the call really returns; restored at once, the rest of the string came out at the game's own
    // size and place - the corner counter glitching now and then.
    struct PendingFont
    {
        const R5900Context *ctx = nullptr;
        FontScale font;
        double since = 0.0;
    };
    std::vector<PendingFont> g_pendingFonts;

    // A call is done with its size: the record gets back the size of a call that is still
    // drawing (stopped part-way), else the game's own - unless the game set a size meanwhile.
    void restoreFont(const FontScale &font)
    {
        if (!font.record)
            return;
        FontRecordUse &use = g_fontUses[font.index];
        if (use.calls > 0u)
            --use.calls;
        if (std::memcmp(font.record + 0x08, use.written, sizeof(use.written)) != 0)
            return;
        const uint16_t *back = use.game;
        if (use.calls > 0u)
            for (size_t i = g_pendingFonts.size(); i-- > 0;)
                if (g_pendingFonts[i].font.record == font.record)
                {
                    back = g_pendingFonts[i].font.scaled;
                    break;
                }
        std::memcpy(use.written, back, sizeof(use.written));
        std::memcpy(font.record + 0x08, use.written, sizeof(use.written));
    }

    // A call that never came back (its thread ended) must not leave the font at its size for good.
    void dropStalePendingFonts()
    {
        const double now = steadySeconds();
        for (size_t i = g_pendingFonts.size(); i-- > 0;)
            if (now - g_pendingFonts[i].since > 1.0)
            {
                const FontScale font = g_pendingFonts[i].font;
                g_pendingFonts.erase(g_pendingFonts.begin() + static_cast<std::ptrdiff_t>(i));
                restoreFont(font);
            }
    }

    FontScale layoutText(uint8_t *rdram, R5900Context *ctx)
    {
        FontScale font;
        const uint32_t caller = returnAddress(ctx);
        float widthScale = 1.0f, heightScale = 1.0f;
        // In 16:9 every font is drawn three quarters as wide (setFontSize). The HUD's glyphs are
        // already drawn at about a quarter of their texture's size, and narrowed further a "1"
        // falls between the texels the GS samples and disappears (ammo, cars left): the HUD's
        // numbers keep their 4:3 width.
        if (widescreen() && contains(kNumberTexts, caller))
            widthScale = 4.0f / 3.0f;
        const bool radar = contains(kRadarTexts, caller);
        if (radar || contains(kWeaponTexts, caller))
        {
            const tmho::HudPiece piece = radar ? tmho::HudPiece::Radar : tmho::HudPiece::Weapon;
            const PieceLayout layout = pieceLayout(piece);
            if (layout.dx != 0.0f || layout.dy != 0.0f || layout.scale != 1.0f)
            {
                float anchorX = 0.0f, anchorLine = 0.0f;
                pieceAnchor(piece, anchorX, anchorLine);
                const float anchorY = anchorLine * 0.5f; // text y counts the 224 lines of a field
                const float x = static_cast<float>(static_cast<int32_t>(GPR_U32(ctx, 6)));
                const float y = static_cast<float>(static_cast<int32_t>(GPR_U32(ctx, 7)));
                const float newX = anchorX + (x - anchorX) * layout.scale + layout.dx * 640.0f;
                const float newY = anchorY + (y - anchorY) * layout.scale + layout.dy * 224.0f;
                SET_GPR_U32(ctx, 6, static_cast<uint32_t>(static_cast<int32_t>(std::lround(newX))));
                SET_GPR_U32(ctx, 7, static_cast<uint32_t>(static_cast<int32_t>(std::lround(newY))));
            }
            widthScale *= layout.scale;
            heightScale *= layout.scale;
        }
        // Tests: PS2X_TEST_SCALE_TEXT=<factor> sizes every text call (menus too).
        static const float testScale = []()
        {
            const char *env = std::getenv("PS2X_TEST_SCALE_TEXT");
            return env ? static_cast<float>(std::atof(env)) : 0.0f;
        }();
        if (testScale > 0.0f)
        {
            widthScale *= testScale;
            heightScale *= testScale;
        }
        // The font's record for this one call: width +0x08, height +0x0A, line step +0x0C, advance +0x0E.
        const uint32_t index = GPR_U32(ctx, 4);
        if (index >= 8u)
            return font;
        FontRecordUse &use = g_fontUses[index];
        const bool sized = widthScale != 1.0f || heightScale != 1.0f;
        if (!sized && use.calls == 0u)
            return font;
        uint8_t *record = getMemPtr(rdram, kFontTable + index * kFontRecordSize);
        if (!record)
            return font;
        uint16_t current[4];
        std::memcpy(current, record + 0x08, sizeof(current));
        // A call stopped part-way may have its size in the record: this call is sized from the
        // game's own values all the same (and an unsized call draws at the game's size).
        const bool oursInRecord = use.calls != 0u && std::memcmp(current, use.written, sizeof(current)) == 0;
        if (!oursInRecord)
        {
            if (!sized)
                return font;
            std::memcpy(use.game, current, sizeof(current));
        }
        font.record = record;
        font.index = index;
        const float factor[4] = {widthScale, heightScale, heightScale, widthScale};
        for (int i = 0; i < 4; ++i)
            font.scaled[i] = static_cast<uint16_t>(std::clamp(std::lround(use.game[i] * factor[i]), 1L, 65535L));
        std::memcpy(use.written, font.scaled, sizeof(use.written));
        std::memcpy(record + 0x08, use.written, sizeof(use.written));
        ++use.calls;
        return font;
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

    void traceText(uint8_t *rdram, const R5900Context *ctx, const char *what)
    {
        char text[24] = {};
        if (const uint8_t *str = getMemPtr(rdram, GPR_U32(ctx, 8)))
            for (size_t i = 0; i + 1 < sizeof(text) && str[i] >= 0x20 && str[i] < 0x7F; ++i)
                text[i] = static_cast<char>(str[i]);
        char details[160];
        std::snprintf(details, sizeof(details), "a0=%u a1=%08x x=%d y=%d t1=%08x \"%s\"", GPR_U32(ctx, 4), GPR_U32(ctx, 5),
                      static_cast<int32_t>(GPR_U32(ctx, 6)), static_cast<int32_t>(GPR_U32(ctx, 7)), GPR_U32(ctx, 9), text);
        hudTrace(what, returnAddress(ctx), details);
    }

    void drawText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (freshEntry(ctx, kDrawText) && hudTraceOn())
            traceText(rdram, ctx, "text");
        if (freshEntry(ctx, kDrawText))
            reapplyBattleCamera(ctx, runtime);
        FontScale font;
        if (freshEntry(ctx, kDrawText))
        {
            const uint32_t caller = returnAddress(ctx);
            if (contains(kRadarTexts, caller) || contains(kWeaponTexts, caller))
            {
                g_hudShownAt.store(steadySeconds(), std::memory_order_relaxed);
                relayoutHud(rdram);
            }
            if (widescreen())
                moveTextX(ctx, kTextMoves);
            if (!g_pendingFonts.empty())
                dropStalePendingFonts();
            font = layoutText(rdram, ctx);
            // Tests: PS2X_TEST_TEXT_INTERRUPT=1 makes every moved HUD text stop part-way for a
            // checkpoint (as a VBlank arriving during it does now and then).
            static const bool interruptTexts = std::getenv("PS2X_TEST_TEXT_INTERRUPT") != nullptr;
            if (interruptTexts && font.record)
                runtime->eeScheduler().postEvent(EeEvent{EeEventType::ExternalWake, 0xFFFFFFFFu, ~0ull});
        }
        try
        {
            g_drawText(rdram, ctx, runtime);
        }
        catch (...)
        {
            if (font.record)
                g_pendingFonts.push_back({ctx, font, steadySeconds()});
            throw;
        }
        if (runtime->eeScheduler().unwindingForCheckpoint())
        {
            // Stopped for a checkpoint; the string is finished when the call resumes.
            if (font.record)
            {
                g_pendingFonts.push_back({ctx, font, steadySeconds()});
                static uint32_t kept = 0;
                if (++kept <= 3u || kept % 1000u == 0u)
                    std::fprintf(stderr, "[tmho] HUD text interrupted part-way (%u so far): its size is kept until it finishes\n", kept);
            }
            return;
        }
        if (font.record)
        {
            restoreFont(font);
            return;
        }
        // A resumed call that has now returned: its font goes back.
        for (size_t i = g_pendingFonts.size(); i-- > 0;)
            if (g_pendingFonts[i].ctx == ctx)
            {
                const FontScale pending = g_pendingFonts[i].font;
                g_pendingFonts.erase(g_pendingFonts.begin() + static_cast<std::ptrdiff_t>(i));
                restoreFont(pending);
                break;
            }
    }

    void drawPlayerText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (freshEntry(ctx, kDrawPlayerText) && hudTraceOn())
            traceText(rdram, ctx, "ptext");
        if (freshEntry(ctx, kDrawPlayerText) && widescreen())
            moveTextX(ctx, kPlayerTextMoves);
        g_drawPlayerText(rdram, ctx, runtime);
    }

    void setFontSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (freshEntry(ctx, kSetFontSize) && hudTraceOn())
        {
            char details[96];
            std::snprintf(details, sizeof(details), "font=%u w=%d h=%d", GPR_U32(ctx, 4), static_cast<int32_t>(GPR_U32(ctx, 5)),
                          static_cast<int32_t>(GPR_U32(ctx, 6)));
            hudTrace("font", returnAddress(ctx), details);
        }
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
            const int angle = ps2settings::get().cameraAngle;
            const uint8_t *eye = getMemPtr(rdram, GPR_U32(ctx, 5));
            const uint8_t *target = getMemPtr(rdram, GPR_U32(ctx, 6));
            const uint8_t *upVector = getMemPtr(rdram, GPR_U32(ctx, 7));
            uint8_t *scratch = getMemPtr(rdram, g_eyeScratch);
            if ((percent != 100 || angle != 0) && eye && target && scratch)
            {
                float e[4], t[4];
                std::memcpy(e, eye, sizeof(e));
                std::memcpy(t, target, sizeof(t));
                float v[3] = {e[0] - t[0], e[1] - t[1], e[2] - t[2]};
                float pitchBefore = 0.0f, pitchAfter = 0.0f;
                if (angle != 0)
                {
                    // Camera angle: the camera swings up (or down) around the car, at the same
                    // distance, along the game's own "up" for this view.
                    float up[3] = {0.0f, 1.0f, 0.0f};
                    if (upVector)
                    {
                        float u[4];
                        std::memcpy(u, upVector, sizeof(u));
                        const float length = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
                        if (length > 1e-6f)
                            for (int i = 0; i < 3; ++i)
                                up[i] = u[i] / length;
                    }
                    const float along = v[0] * up[0] + v[1] * up[1] + v[2] * up[2];
                    const float h[3] = {v[0] - along * up[0], v[1] - along * up[1], v[2] - along * up[2]};
                    const float flat = std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
                    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (flat > 1e-4f && length > 1e-4f)
                    {
                        // Never lower than 2 degrees above the car (the camera would go into the
                        // ground) or higher than 80 (straight above, the view turns over), unless
                        // the game's own camera already is.
                        constexpr float kLowest = 2.0f / 57.29578f, kHighest = 80.0f / 57.29578f;
                        pitchBefore = std::atan2(along, flat);
                        pitchAfter = std::clamp(pitchBefore + static_cast<float>(angle) / 57.29578f, std::min(pitchBefore, kLowest),
                                                std::max(pitchBefore, kHighest));
                        const float c = std::cos(pitchAfter), sn = std::sin(pitchAfter);
                        for (int i = 0; i < 3; ++i)
                            v[i] = length * (c * h[i] / flat + sn * up[i]);
                    }
                }
                // Distance: same direction from the target, scaled (closer below 100 %, further above).
                const float k = static_cast<float>(percent) / 100.0f;
                for (int i = 0; i < 3; ++i)
                    e[i] = t[i] + v[i] * k;
                std::memcpy(scratch, e, sizeof(e));
                SET_GPR_U32(ctx, 5, g_eyeScratch);
                static int traced = 0;
                if (traceOn() && traced < 5)
                {
                    ++traced;
                    float u[4] = {0, 0, 0, 0};
                    if (upVector)
                        std::memcpy(u, upVector, sizeof(u));
                    float e0[4];
                    std::memcpy(e0, eye, sizeof(e0));
                    std::fprintf(stderr, "[tmho] chase camera caller=%06x target=(%.2f %.2f %.2f %.2f) eye (%.2f %.2f %.2f %.2f) up (%.3f %.3f %.3f %.3f) -> eye (%.2f %.2f %.2f), %d%%, pitch %.1f -> %.1f deg\n",
                                 returnAddress(ctx), t[0], t[1], t[2], t[3], e0[0], e0[1], e0[2], e0[3], u[0], u[1], u[2], u[3], e[0], e[1], e[2], percent,
                                 pitchBefore * 57.29578f, pitchAfter * 57.29578f);
                }
            }
        }
        g_cameraLookAt(rdram, ctx, runtime);
    }

    // F2 > Game > Skip intro movies: the two logo movies, and the intro the first time it plays.
    bool endsWith(const std::string &text, const char *suffix)
    {
        const size_t n = std::strlen(suffix);
        return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
    }

    std::string guestString(uint8_t *rdram, uint32_t address)
    {
        std::string out;
        if (const uint8_t *p = getMemPtr(rdram, address))
            for (size_t i = 0; i < 64u && p[i] >= 0x20u && p[i] < 0x7Fu; ++i)
                out.push_back(static_cast<char>(std::toupper(p[i])));
        return out;
    }

    void playMovie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (freshEntry(ctx, kPlayMovie) && ps2settings::get().skipIntro)
        {
            const std::string name = guestString(rdram, GPR_U32(ctx, 5));
            if (endsWith(name, "LEGAL.PSS") || endsWith(name, "ESPLOGO.PSS"))
                ps2movie::requestSkip(10.0);
        }
        g_playMovie(rdram, ctx, runtime);
    }

    void startFrontendMovie(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static bool introSeen = false;
        if (freshEntry(ctx, kStartFrontendMovie) && !introSeen)
        {
            const std::string path = guestString(rdram, GPR_U32(ctx, 5));
            if (endsWith(path, "INTRO.PSS") || endsWith(path, "INTROLOS.PSS"))
            {
                introSeen = true;
                if (ps2settings::get().skipIntro)
                    ps2movie::requestSkip(10.0);
            }
        }
        g_startFrontendMovie(rdram, ctx, runtime);
    }

    bool hook(PS2Runtime &runtime, uint32_t address, Fn wrapper, Fn &original)
    {
        original = runtime.lookupFunction(address);
        if (!original)
            return false;
        return runtime.replaceFunction(address, wrapper);
    }

    // As hook(), and a call that resumes inside the function after a checkpoint comes through the
    // wrapper too (it finds the original from its label), so the wrapper sees the call finish.
    bool hookWithResumes(PS2Runtime &runtime, uint32_t address, Fn wrapper, Fn &original)
    {
        if (!hook(runtime, address, wrapper, original))
            return false;
        for (uint32_t pc = address + 4u; runtime.hasFunction(pc) && runtime.lookupFunction(pc) == original; pc += 4u)
            runtime.replaceFunction(pc, wrapper);
        return true;
    }

    void apply(PS2Runtime &runtime)
    {
        g_runtime = &runtime;
        bool ok = true;
        ok &= hook(runtime, kSetViewportFov, &setViewportFov, g_setViewportFov);
        ok &= hook(runtime, kPlaceHudModel, &placeHudModel, g_placeHudModel);
        ok &= hook(runtime, kPlaceHudInstance, &placeHudInstance, g_placeHudInstance);
        ok &= hookWithResumes(runtime, kDrawText, &drawText, g_drawText);
        ok &= hook(runtime, kDrawPlayerText, &drawPlayerText, g_drawPlayerText);
        ok &= hook(runtime, kSetFontSize, &setFontSize, g_setFontSize);
        ok &= hook(runtime, kCameraLookAt, &cameraLookAt, g_cameraLookAt);
        ok &= hook(runtime, kPlayMovie, &playMovie, g_playMovie);
        ok &= hook(runtime, kStartFrontendMovie, &startFrontendMovie, g_startFrontendMovie);
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

    HudBox hudDefaultBox(HudPiece piece)
    {
        const int index = static_cast<int>(piece);
        float anchorX = 0.0f, anchorLine = 0.0f;
        pieceAnchor(piece, anchorX, anchorLine);
        // Picture heights to picture widths: 16:9 is 16/9 heights wide, 4:3 is 4/3.
        const float widthInHeights = widescreen() ? 16.0f / 9.0f : 4.0f / 3.0f;
        const float *extent = kPieceExtent[index];
        HudBox box;
        box.x = anchorX / 640.0f + extent[0] / widthInHeights;
        box.y = anchorLine / 448.0f + extent[1];
        box.width = (extent[2] - extent[0]) / widthInHeights;
        box.height = extent[3] - extent[1];
        return box;
    }

    HudBox hudBox(HudPiece piece)
    {
        const HudBox base = hudDefaultBox(piece);
        const PieceLayout layout = pieceLayout(piece);
        float anchorX = 0.0f, anchorLine = 0.0f;
        pieceAnchor(piece, anchorX, anchorLine);
        const float cx = anchorX / 640.0f;
        const float cy = anchorLine / 448.0f;
        HudBox box;
        box.x = cx + (base.x - cx) * layout.scale + layout.dx;
        box.y = cy + (base.y - cy) * layout.scale + layout.dy;
        box.width = base.width * layout.scale;
        box.height = base.height * layout.scale;
        return box;
    }

    void hudLayoutChanged()
    {
        g_relayoutPending.store(true);
    }

    bool hudShownRecently(double seconds)
    {
        const double at = g_hudShownAt.load(std::memory_order_relaxed);
        return at >= 0.0 && steadySeconds() - at <= seconds;
    }

    void settingsChanged()
    {
        if (!g_runtime)
            return;
        const bool wide = widescreen();
        if (wide != g_wideApplied.load())
            writeTables(g_runtime->memory().getRDRAM(), wide);
        g_reapplyBattleCamera.store(true);
        g_relayoutPending.store(true);
    }
}

// The generated code in this runtime is this game's, so only the entry point is checked.
PS2_REGISTER_GAME_OVERRIDE("TMHO widescreen and field of view", "", 0x100008u, 0u, &apply)
