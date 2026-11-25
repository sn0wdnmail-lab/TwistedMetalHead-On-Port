#include "runtime/ps2_present.h"
#include "runtime/ps2_gl_shader.h"
#include "runtime/ps2_android.h"

#include "runtime/ps2_settings.h"

#include "raylib.h"
#include "rlgl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>

// The picture path of the window (see ps2_present.h).
//
// Conventions: every texture made here stores the picture with its top row at texel row 0, the
// same way the frame texture is uploaded. Passes find their output texel from gl_FragCoord, so
// raylib's (upside-down) render-texture projection does not matter; the final draw uses raylib's
// texture coordinates, whose v = 0 is the top of the destination rectangle.

#if defined(_WIN32)
// (windows.h clashes with raylib's names.)
extern "C" __declspec(dllimport) void *__stdcall LoadLibraryA(const char *name);
extern "C" __declspec(dllimport) void *__stdcall GetProcAddress(void *module, const char *name);
#endif

namespace ps2present
{
    namespace
    {
        // ------------------------------------------------------------ VBlank notes (game thread)
        struct Note
        {
            std::atomic<uint64_t> tick{~0ull};
            std::atomic<double> seconds{0.0};
            std::atomic<bool> newPicture{false};
        };
        constexpr uint32_t kNoteCount = 256u;
        Note g_notes[kNoteCount];

        bool lookupNote(uint64_t tick, double &seconds, bool &newPicture)
        {
            Note &n = g_notes[tick % kNoteCount];
            if (n.tick.load(std::memory_order_acquire) != tick)
                return false;
            seconds = n.seconds.load(std::memory_order_relaxed);
            newPicture = n.newPicture.load(std::memory_order_relaxed);
            return n.tick.load(std::memory_order_acquire) == tick;
        }

        // ------------------------------------------------------------ shaders
        const char *const kResampleFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 srcSize;
uniform vec2 dstSize;
uniform vec2 dir;
uniform float scale;
uniform int nearest;
uniform int swapRB;
uniform float gamma;
out vec4 finalColor;
float lanczos2(float x)
{
    x = abs(x);
    if (x < 1e-4) return 1.0;
    if (x >= 2.0) return 0.0;
    float px = 3.14159265 * x;
    return 2.0 * sin(px) * sin(px * 0.5) / (px * px);
}
void main()
{
    ivec2 o = ivec2(gl_FragCoord.xy);
    float along = dot(vec2(o), dir);
    int other = int(dot(vec2(o), vec2(1.0) - dir));
    float srcLen = dot(srcSize, dir);
    float dstLen = dot(dstSize, dir);
    float center = (along + 0.5) * srcLen / dstLen - 0.5;
    int maxIndex = int(srcLen) - 1;
    vec4 color;
    if (nearest == 1)
    {
        // Sharp (enlarging): each output pixel takes the source pixels it covers, in proportion.
        // Crisp like nearest neighbour, but every source pixel comes out the same size at uneven
        // scales (plain nearest repeats some lines twice and others three times).
        float w = srcLen / dstLen;
        float s0 = along * w;
        float s1 = s0 + w;
        float edge = floor(s1);
        int i0 = clamp(int(floor(s0)), 0, maxIndex);
        color = texelFetch(texture0, dir.x > 0.5 ? ivec2(i0, other) : ivec2(other, i0), 0);
        if (edge > s0 && edge < s1)
        {
            int i1 = clamp(int(edge), 0, maxIndex);
            vec4 next = texelFetch(texture0, dir.x > 0.5 ? ivec2(i1, other) : ivec2(other, i1), 0);
            color = mix(color, next, (s1 - edge) / w);
        }
    }
    else
    {
        float support = 2.0 * scale;
        int first = int(floor(center - support)) + 1;
        int last = int(floor(center + support));
        vec4 sum = vec4(0.0);
        float weightSum = 0.0;
        for (int i = first; i <= last; ++i)
        {
            float w = lanczos2((float(i) - center) / scale);
            int ci = clamp(i, 0, maxIndex);
            sum += w * texelFetch(texture0, dir.x > 0.5 ? ivec2(ci, other) : ivec2(other, ci), 0);
            weightSum += w;
        }
        color = clamp(sum / weightSum, 0.0, 1.0);
    }
    if (swapRB == 1)
        color.rgb = color.bgr;
    if (gamma != 1.0)
        color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(gamma));
    finalColor = vec4(color.rgb, 1.0);
}
)";

        // Sharpening (F2 > Video): each pixel against its four neighbours, strongest on soft detail
        // and gentle where the picture already has hard edges (no bright halos around text).
        const char *const kSharpenFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 size;
uniform float amount;
out vec4 finalColor;
vec3 px(ivec2 p) { return texelFetch(texture0, clamp(p, ivec2(0), ivec2(size) - 1), 0).rgb; }
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 c = px(p);
    vec3 n = px(p + ivec2(0, 1));
    vec3 s = px(p - ivec2(0, 1));
    vec3 e = px(p + ivec2(1, 0));
    vec3 w = px(p - ivec2(1, 0));
    vec3 lo = min(c, min(min(n, s), min(e, w)));
    vec3 hi = max(c, max(max(n, s), max(e, w)));
    vec3 room = clamp(min(lo, vec3(1.0) - hi) / max(hi, vec3(1e-4)), 0.0, 1.0);
    vec3 lobe = -sqrt(room) * mix(0.08, 0.2, amount);
    vec3 result = (c + (n + s + e + w) * lobe) / (vec3(1.0) + 4.0 * lobe);
    finalColor = vec4(clamp(result, 0.0, 1.0), 1.0);
}
)";

        // Colour picture -> luma at the first motion level (a 4 x 4 box of bilinear taps per texel).
        const char *const kLumaFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 srcSize;
uniform vec2 dstSize;
out vec4 finalColor;
void main()
{
    vec2 scale = srcSize / dstSize;
    vec2 base = (gl_FragCoord.xy - 0.5) * scale;
    float sum = 0.0;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            vec2 p = base + (vec2(x, y) + 0.5) * scale * 0.25;
            sum += dot(texture(texture0, p / srcSize).rgb, vec3(0.299, 0.587, 0.114));
        }
    finalColor = vec4(sum / 16.0, 0.0, 0.0, 1.0);
}
)";

        const char *const kHalfFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 srcSize;
out vec4 finalColor;
void main()
{
    ivec2 o = ivec2(gl_FragCoord.xy) * 2;
    ivec2 m = ivec2(srcSize) - 1;
    float s = texelFetch(texture0, min(o, m), 0).r + texelFetch(texture0, min(o + ivec2(1, 0), m), 0).r +
              texelFetch(texture0, min(o + ivec2(0, 1), m), 0).r + texelFetch(texture0, min(o + ivec2(1, 1), m), 0).r;
    finalColor = vec4(s * 0.25, 0.0, 0.0, 1.0);
}
)";

        // Block matching, coarse to fine. Output xy: how far the picture moved from frame A to frame
        // B at this texel of B, in this level's texels (B(x) = A(x - f)); z: the match cost.
        const char *const kFlowFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform sampler2D lumaA;
uniform sampler2D coarse;
uniform vec2 size;
uniform vec2 coarseSize;
uniform int coarsest;
uniform int searchRadius;
uniform int patchRadius;
uniform int subpixel;
out vec4 finalColor;
float lumB(ivec2 p) { return texelFetch(texture0, clamp(p, ivec2(0), ivec2(size) - 1), 0).r; }
float lumA(vec2 p) { return texture(lumaA, p / size).r; }
float cost(ivec2 p, vec2 f)
{
    float s = 0.0;
    for (int dy = -patchRadius; dy <= patchRadius; ++dy)
        for (int dx = -patchRadius; dx <= patchRadius; ++dx)
        {
            ivec2 q = p + ivec2(dx, dy);
            s += abs(lumB(q) - lumA(vec2(q) + 0.5 - f));
        }
    float n = float((2 * patchRadius + 1) * (2 * patchRadius + 1));
    return s / n + 0.0004 * length(f);
}
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec2 best = vec2(0.0);
    float bestCost = cost(p, best);
    if (coarsest == 1)
    {
        for (int y = -searchRadius; y <= searchRadius; ++y)
            for (int x = -searchRadius; x <= searchRadius; ++x)
            {
                vec2 f = vec2(x, y);
                float c = cost(p, f);
                if (c < bestCost) { bestCost = c; best = f; }
            }
    }
    else
    {
        ivec2 cp = p / 2;
        ivec2 cm = ivec2(coarseSize) - 1;
        ivec2 offsets[5] = ivec2[5](ivec2(0, 0), ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1));
        for (int i = 0; i < 5; ++i)
        {
            vec2 f = texelFetch(coarse, clamp(cp + offsets[i], ivec2(0), cm), 0).xy * 2.0;
            float c = cost(p, f);
            if (c < bestCost) { bestCost = c; best = f; }
        }
        vec2 center = best;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                if (x == 0 && y == 0) continue;
                vec2 f = center + vec2(x, y);
                float c = cost(p, f);
                if (c < bestCost) { bestCost = c; best = f; }
            }
        if (subpixel == 1)
        {
            center = best;
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                {
                    if (x == 0 && y == 0) continue;
                    vec2 f = center + 0.5 * vec2(x, y);
                    float c = cost(p, f);
                    if (c < bestCost) { bestCost = c; best = f; }
                }
        }
    }
    finalColor = vec4(best, bestCost, 1.0);
}
)";

        // 3 x 3 median of the motion field (each component), to drop odd vectors.
        const char *const kMedianFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec2 size;
out vec4 finalColor;
#define S2(a, b) { vec2 t = min(a, b); b = max(a, b); a = t; }
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 m = ivec2(size) - 1;
    vec2 v[9];
    int k = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            v[k++] = texelFetch(texture0, clamp(p + ivec2(x, y), ivec2(0), m), 0).xy;
    S2(v[1], v[2]); S2(v[4], v[5]); S2(v[7], v[8]);
    S2(v[0], v[1]); S2(v[3], v[4]); S2(v[6], v[7]);
    S2(v[1], v[2]); S2(v[4], v[5]); S2(v[7], v[8]);
    S2(v[0], v[3]); S2(v[5], v[8]); S2(v[4], v[7]);
    S2(v[3], v[6]); S2(v[1], v[4]); S2(v[2], v[5]);
    S2(v[4], v[7]); S2(v[4], v[2]); S2(v[6], v[4]);
    S2(v[4], v[2]);
    finalColor = vec4(v[4], texelFetch(texture0, p, 0).z, 1.0);
}
)";

        // An in-between frame at time t (0 = frame A, 1 = frame B). Each pixel follows the motion
        // field, and falls back to the still picture where that agrees better (HUD, text) and to
        // the nearer frame where neither agrees (things appearing, scene cuts).
        const char *const kInterpFs = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform sampler2D colorA;
uniform sampler2D flow;
uniform vec2 flowSize;
uniform float t;
out vec4 finalColor;
void main()
{
    vec2 uv = fragTexCoord;
    vec2 f = texture(flow, uv).xy / flowSize;
    vec3 a = texture(colorA, uv - t * f).rgb;
    vec3 b = texture(texture0, uv + (1.0 - t) * f).rgb;
    vec3 aS = texture(colorA, uv).rgb;
    vec3 bS = texture(texture0, uv).rgb;
    float dM = dot(abs(a - b), vec3(1.0 / 3.0));
    float dS = dot(abs(aS - bS), vec3(1.0 / 3.0));
    float still = smoothstep(-0.01, 0.01, dM - dS);
    vec3 blend = mix(mix(a, b, t), mix(aS, bS, t), still);
    vec3 nearA = mix(a, aS, still);
    vec3 nearB = mix(b, bS, still);
    vec3 nearest = t < 0.5 ? nearA : nearB;
    float d = mix(dM, dS, still);
    finalColor = vec4(mix(blend, nearest, smoothstep(0.06, 0.2, d)), 1.0);
}
)";

        struct Program
        {
            Shader shader{};
            int loc[12]{};
        };

        enum ResampleLoc { RsSrcSize, RsDstSize, RsDir, RsScale, RsNearest, RsSwap, RsGamma };
        enum SharpenLoc { ShSize, ShAmount };
        enum LumaLoc { LmSrcSize, LmDstSize };
        enum HalfLoc { HfSrcSize };
        enum FlowLoc { FlLumaA, FlCoarse, FlSize, FlCoarseSize, FlCoarsest, FlSearch, FlPatch, FlSubpixel };
        enum MedianLoc { MdSize };
        enum InterpLoc { InColorA, InFlow, InFlowSize, InT };

        Program g_resample, g_luma, g_half, g_flow, g_median, g_interp;
        Program g_sharpen; // optional: without it the picture is just not sharpened
        bool g_sharpenReady = false;
        bool g_ready = false;
        bool g_failed = false;

        bool build(Program &p, const char *fs, std::initializer_list<const char *> names)
        {
            p.shader = LoadShaderFromMemory(nullptr, ps2gl::fragmentSource(fs).c_str());
            if (p.shader.id == 0 || p.shader.id == rlGetShaderIdDefault())
                return false;
            int i = 0;
            for (const char *name : names)
                p.loc[i++] = GetShaderLocation(p.shader, name);
            return true;
        }

        // ------------------------------------------------------------ targets
        RenderTexture2D makeTarget(int width, int height, bool halfFloat)
        {
            RenderTexture2D t{};
            t.id = rlLoadFramebuffer();
            if (t.id == 0)
                return t;
            rlEnableFramebuffer(t.id);
            const int format = halfFloat ? RL_PIXELFORMAT_UNCOMPRESSED_R16G16B16A16 : RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            t.texture.id = rlLoadTexture(nullptr, width, height, format, 1);
            t.texture.width = width;
            t.texture.height = height;
            t.texture.mipmaps = 1;
            t.texture.format = format;
            rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
            const bool ok = rlFramebufferComplete(t.id);
            rlDisableFramebuffer();
            if (!ok)
            {
                UnloadRenderTexture(t);
                return RenderTexture2D{};
            }
            SetTextureWrap(t.texture, TEXTURE_WRAP_CLAMP);
            SetTextureFilter(t.texture, TEXTURE_FILTER_BILINEAR);
            return t;
        }

        void release(RenderTexture2D &t)
        {
            if (t.id != 0)
                UnloadRenderTexture(t);
            t = RenderTexture2D{};
        }

        bool sizeIs(const RenderTexture2D &t, int width, int height)
        {
            return t.id != 0 && t.texture.width == width && t.texture.height == height;
        }

        void ensure(RenderTexture2D &t, int width, int height, bool halfFloat)
        {
            if (sizeIs(t, width, height))
                return;
            release(t);
            t = makeTarget(width, height, halfFloat);
        }

        // Draws one full-target pass: texture0 = source, the program's uniforms already set.
        void pass(RenderTexture2D &target, const Texture2D &source)
        {
            DrawTexturePro(source, Rectangle{0.0f, 0.0f, static_cast<float>(source.width), static_cast<float>(source.height)},
                           Rectangle{0.0f, 0.0f, static_cast<float>(target.texture.width), static_cast<float>(target.texture.height)},
                           Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        }

        void setVec2(const Program &p, int loc, float x, float y)
        {
            const float v[2] = {x, y};
            SetShaderValue(p.shader, p.loc[loc], v, SHADER_UNIFORM_VEC2);
        }
        void setFloat(const Program &p, int loc, float x) { SetShaderValue(p.shader, p.loc[loc], &x, SHADER_UNIFORM_FLOAT); }
        void setInt(const Program &p, int loc, int x) { SetShaderValue(p.shader, p.loc[loc], &x, SHADER_UNIFORM_INT); }

        // ------------------------------------------------------------ frames
        constexpr int kLevels = 4;   // motion levels: 1/1 of the luma picture down to 1/8
        constexpr int kSlots = 4;    // game frames kept for in-between frames

        struct Keyframe
        {
            RenderTexture2D color{};
            RenderTexture2D luma[kLevels]{};
            RenderTexture2D flow{};  // motion from the keyframe before this one, at luma level 0
            double seconds = 0.0;
            uint64_t sequence = 0;
            uint64_t picture = 0;    // game picture shown (repeats of one picture share it)
            bool hasLuma = false;
            bool hasFlow = false;
            bool valid = false;
        };

        Keyframe g_slots[kSlots];
        int g_newest = -1;            // slot of the newest keyframe
        uint64_t g_sequence = 0;
        uint64_t g_pictures = 0;      // new game pictures seen
        RenderTexture2D g_wide{};     // horizontal pass output
        RenderTexture2D g_tall{};     // vertical pass output when the picture is sharpened after it
        RenderTexture2D g_flowLevel[kLevels]{};
        int g_pictureWidth = 0, g_pictureHeight = 0;
        int g_lumaWidth[kLevels]{}, g_lumaHeight[kLevels]{};

        // Pacing of in-between frames.
        double g_interval = 1.0 / 60.0;    // between game frames (smoothed)
        double g_latency = 0.010;          // VBlank -> frame here (recent maximum, decays)
        double g_lag = 0.0;                // how far behind real time the shown moment runs
        double g_lastKeySeconds = -1.0;

        // Shown pictures per second.
        double g_fpsWindowStart = 0.0;
        int g_fpsCount = 0;
        double g_shownFps = -1.0;
        uint64_t g_lastShownA = ~0ull, g_lastShownB = ~0ull;
        float g_lastShownT = -1.0f;
        bool g_interpolatingNow = false;
        // Copies the performance log reads from its own thread.
        std::atomic<int> g_statPictureWidth{0}, g_statPictureHeight{0};
        std::atomic<double> g_statShownFps{-1.0};
        std::atomic<bool> g_statInterpolating{false};

        // How often the window actually shows a frame (V-Sync and the frame cap decide).
        double g_lastDrawSeconds = 0.0;
        double g_drawInterval = 1.0 / 60.0;

        void countShown(uint64_t a, uint64_t b, float t)
        {
            const double nowSeconds = now();
            if (g_fpsWindowStart == 0.0)
                g_fpsWindowStart = nowSeconds;
            if (a != g_lastShownA || b != g_lastShownB || std::fabs(t - g_lastShownT) > 1e-4f)
            {
                ++g_fpsCount;
                g_lastShownA = a;
                g_lastShownB = b;
                g_lastShownT = t;
            }
            const double elapsed = nowSeconds - g_fpsWindowStart;
            if (elapsed >= 0.5)
            {
                g_shownFps = g_fpsCount / elapsed;
                g_statShownFps.store(g_shownFps, std::memory_order_relaxed);
                g_fpsCount = 0;
                g_fpsWindowStart = nowSeconds;
            }
        }

        int presetLines(int preset)
        {
            static const int lines[] = {480, 720, 1080, 1440, 2160};
            return lines[std::clamp(preset, 0, 4)];
        }

        bool interpolationWanted()
        {
            return ps2settings::get().frameRate != 0;
        }

        // The size every frame is scaled to: the chosen resolution, kept to the shape the picture
        // has on screen, and never larger than that rectangle (a larger choice is the same as
        // drawing big and shrinking, done here in one step).
        void wantedPictureSize(int sourceWidth, int sourceHeight, int &width, int &height)
        {
            float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
            const float screenWidth = static_cast<float>(std::max(1, GetScreenWidth()));
            const float screenHeight = static_cast<float>(std::max(1, GetScreenHeight()));
            ps2settings::presentRect(static_cast<float>(sourceWidth), static_cast<float>(sourceHeight), screenWidth, screenHeight, x, y,
                                     w, h);
            w = std::max(w, 16.0f);
            h = std::max(h, 16.0f);
            const float lines = static_cast<float>(presetLines(ps2settings::get().resolution));
            const float useHeight = std::min(lines, h);
            height = std::max(16, static_cast<int>(std::lround(useHeight)));
            width = std::max(16, static_cast<int>(std::lround(useHeight * w / h)));
        }

        void dropHistory()
        {
            for (Keyframe &k : g_slots)
            {
                k.valid = false;
                k.hasLuma = false;
                k.hasFlow = false;
            }
            g_newest = -1;
            g_lastKeySeconds = -1.0;
        }

        void resizeTargets(int width, int height)
        {
            if (width == g_pictureWidth && height == g_pictureHeight)
                return;
            g_pictureWidth = width;
            g_pictureHeight = height;
            g_statPictureWidth.store(width, std::memory_order_relaxed);
            g_statPictureHeight.store(height, std::memory_order_relaxed);
            dropHistory();
            // Motion is measured on a luma picture about 270 lines high.
            const float shrink = std::max(1.0f, static_cast<float>(height) / 270.0f);
            g_lumaWidth[0] = std::max(8, static_cast<int>(std::lround(width / shrink)));
            g_lumaHeight[0] = std::max(8, static_cast<int>(std::lround(height / shrink)));
            for (int l = 1; l < kLevels; ++l)
            {
                g_lumaWidth[l] = std::max(4, (g_lumaWidth[l - 1] + 1) / 2);
                g_lumaHeight[l] = std::max(4, (g_lumaHeight[l - 1] + 1) / 2);
            }
            for (Keyframe &k : g_slots)
            {
                ensure(k.color, width, height, false);
                for (int l = 0; l < kLevels; ++l)
                    release(k.luma[l]);
                release(k.flow);
            }
            for (int l = 0; l < kLevels; ++l)
                release(g_flowLevel[l]);
        }

        void ensureMotionTargets(Keyframe &k)
        {
            for (int l = 0; l < kLevels; ++l)
                ensure(k.luma[l], g_lumaWidth[l], g_lumaHeight[l], false);
            ensure(k.flow, g_lumaWidth[0], g_lumaHeight[0], true);
            for (int l = 0; l < kLevels; ++l)
                ensure(g_flowLevel[l], g_lumaWidth[l], g_lumaHeight[l], true);
        }

        void scaleInto(RenderTexture2D &target, const Texture2D &source, int width, int height, bool rawBgra)
        {
            const bool sharp = ps2settings::get().filter == 0;
            ensure(g_wide, target.texture.width, height, false);
            const float scaleX = std::max(1.0f, static_cast<float>(width) / static_cast<float>(target.texture.width));
            const float scaleY = std::max(1.0f, static_cast<float>(height) / static_cast<float>(target.texture.height));
            // Sharp enlarges with nearest neighbour; shrinking always filters.
            const int nearestX = (sharp && target.texture.width >= width) ? 1 : 0;
            const int nearestY = (sharp && target.texture.height >= height) ? 1 : 0;

            BeginTextureMode(g_wide);
            BeginShaderMode(g_resample.shader);
            setVec2(g_resample, RsSrcSize, static_cast<float>(width), static_cast<float>(height));
            setVec2(g_resample, RsDstSize, static_cast<float>(g_wide.texture.width), static_cast<float>(g_wide.texture.height));
            setVec2(g_resample, RsDir, 1.0f, 0.0f);
            setFloat(g_resample, RsScale, scaleX);
            setInt(g_resample, RsNearest, nearestX);
            setInt(g_resample, RsSwap, rawBgra ? 1 : 0);
            setFloat(g_resample, RsGamma, 1.0f);
            pass(g_wide, source);
            EndShaderMode();
            EndTextureMode();

            const int sharpening = std::clamp(ps2settings::get().sharpening, 0, 100);
            const bool sharpen = g_sharpenReady && sharpening > 0;
            if (sharpen)
                ensure(g_tall, target.texture.width, target.texture.height, false);
            RenderTexture2D &vertical = (sharpen && g_tall.id != 0) ? g_tall : target;
            BeginTextureMode(vertical);
            BeginShaderMode(g_resample.shader);
            setVec2(g_resample, RsSrcSize, static_cast<float>(g_wide.texture.width), static_cast<float>(g_wide.texture.height));
            setVec2(g_resample, RsDstSize, static_cast<float>(target.texture.width), static_cast<float>(target.texture.height));
            setVec2(g_resample, RsDir, 0.0f, 1.0f);
            setFloat(g_resample, RsScale, scaleY);
            setInt(g_resample, RsNearest, nearestY);
            setInt(g_resample, RsSwap, 0);
            // F2 > Video > Brightness: a gamma curve on the finished picture (100% = 1.0).
            setFloat(g_resample, RsGamma, std::pow(100.0f / static_cast<float>(std::clamp(ps2settings::get().brightness, 70, 150)), 1.3f));
            pass(vertical, g_wide.texture);
            EndShaderMode();
            EndTextureMode();

            if (&vertical == &g_tall)
            {
                BeginTextureMode(target);
                BeginShaderMode(g_sharpen.shader);
                setVec2(g_sharpen, ShSize, static_cast<float>(target.texture.width), static_cast<float>(target.texture.height));
                setFloat(g_sharpen, ShAmount, static_cast<float>(sharpening) / 100.0f);
                pass(target, g_tall.texture);
                EndShaderMode();
                EndTextureMode();
            }
        }

        void buildLuma(Keyframe &k)
        {
            // The luma taps are bilinear (the colour picture may be left on nearest from drawing).
            SetTextureFilter(k.color.texture, TEXTURE_FILTER_BILINEAR);
            BeginTextureMode(k.luma[0]);
            BeginShaderMode(g_luma.shader);
            setVec2(g_luma, LmSrcSize, static_cast<float>(k.color.texture.width), static_cast<float>(k.color.texture.height));
            setVec2(g_luma, LmDstSize, static_cast<float>(g_lumaWidth[0]), static_cast<float>(g_lumaHeight[0]));
            pass(k.luma[0], k.color.texture);
            EndShaderMode();
            EndTextureMode();
            for (int l = 1; l < kLevels; ++l)
            {
                BeginTextureMode(k.luma[l]);
                BeginShaderMode(g_half.shader);
                setVec2(g_half, HfSrcSize, static_cast<float>(g_lumaWidth[l - 1]), static_cast<float>(g_lumaHeight[l - 1]));
                pass(k.luma[l], k.luma[l - 1].texture);
                EndShaderMode();
                EndTextureMode();
            }
            k.hasLuma = true;
        }

        // Motion from keyframe a to keyframe b, coarse to fine, into b.flow.
        void buildFlow(Keyframe &a, Keyframe &b)
        {
            for (int l = kLevels - 1; l >= 0; --l)
            {
                const bool coarsest = l == kLevels - 1;
                BeginTextureMode(g_flowLevel[l]);
                BeginShaderMode(g_flow.shader);
                SetShaderValueTexture(g_flow.shader, g_flow.loc[FlLumaA], a.luma[l].texture);
                // (Unused at the coarsest level, but never bound to the target being drawn.)
                SetShaderValueTexture(g_flow.shader, g_flow.loc[FlCoarse],
                                      coarsest ? a.luma[l].texture : g_flowLevel[l + 1].texture);
                setVec2(g_flow, FlSize, static_cast<float>(g_lumaWidth[l]), static_cast<float>(g_lumaHeight[l]));
                if (!coarsest)
                    setVec2(g_flow, FlCoarseSize, static_cast<float>(g_lumaWidth[l + 1]), static_cast<float>(g_lumaHeight[l + 1]));
                setInt(g_flow, FlCoarsest, coarsest ? 1 : 0);
                setInt(g_flow, FlSearch, 4);
                setInt(g_flow, FlPatch, l == 0 ? 1 : 2);
                setInt(g_flow, FlSubpixel, l == 0 ? 1 : 0);
                pass(g_flowLevel[l], b.luma[l].texture);
                EndShaderMode();
                EndTextureMode();
            }
            BeginTextureMode(b.flow);
            BeginShaderMode(g_median.shader);
            setVec2(g_median, MdSize, static_cast<float>(g_lumaWidth[0]), static_cast<float>(g_lumaHeight[0]));
            pass(b.flow, g_flowLevel[0].texture);
            EndShaderMode();
            EndTextureMode();
            b.hasFlow = true;
        }

        int slotBefore(int slot) { return (slot + kSlots - 1) % kSlots; }

        void drawPlain(const Keyframe &k, float dstX, float dstY, float dstWidth, float dstHeight)
        {
            const bool sharp = ps2settings::get().filter == 0;
            const bool enlarging = dstWidth > k.color.texture.width + 0.5f || dstHeight > k.color.texture.height + 0.5f;
            SetTextureFilter(k.color.texture, (sharp && enlarging) ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);
            DrawTexturePro(k.color.texture,
                           Rectangle{0.0f, 0.0f, static_cast<float>(k.color.texture.width), static_cast<float>(k.color.texture.height)},
                           Rectangle{dstX, dstY, dstWidth, dstHeight}, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        }
    }

    void noteVBlank(uint64_t tick, double seconds, bool newPicture) noexcept
    {
        Note &n = g_notes[tick % kNoteCount];
        n.tick.store(~0ull, std::memory_order_relaxed);
        n.seconds.store(seconds, std::memory_order_relaxed);
        n.newPicture.store(newPicture, std::memory_order_relaxed);
        n.tick.store(tick, std::memory_order_release);
    }

    double now() noexcept
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void initialize()
    {
        if (g_ready || g_failed)
            return;
        bool ok = build(g_resample, kResampleFs, {"srcSize", "dstSize", "dir", "scale", "nearest", "swapRB", "gamma"});
        ok = ok && build(g_luma, kLumaFs, {"srcSize", "dstSize"});
        ok = ok && build(g_half, kHalfFs, {"srcSize"});
        ok = ok && build(g_flow, kFlowFs, {"lumaA", "coarse", "size", "coarseSize", "coarsest", "searchRadius", "patchRadius", "subpixel"});
        ok = ok && build(g_median, kMedianFs, {"size"});
        ok = ok && build(g_interp, kInterpFs, {"colorA", "flow", "flowSize", "t"});
        g_sharpenReady = ok && build(g_sharpen, kSharpenFs, {"size", "amount"});
        if (!ok)
        {
            std::fprintf(stderr, "[present] shaders unavailable: the picture is drawn directly (no scaling filter, no in-between frames)\n");
            g_failed = true;
            return;
        }
        g_ready = true;
    }

    void shutdown()
    {
        for (Keyframe &k : g_slots)
        {
            release(k.color);
            for (RenderTexture2D &t : k.luma)
                release(t);
            release(k.flow);
        }
        release(g_wide);
        release(g_tall);
        for (RenderTexture2D &t : g_flowLevel)
            release(t);
        for (Program *p : {&g_resample, &g_luma, &g_half, &g_flow, &g_median, &g_interp, &g_sharpen})
            if (p->shader.id != 0 && p->shader.id != rlGetShaderIdDefault())
                UnloadShader(p->shader);
        g_ready = false;
    }

    void submitFrame(unsigned textureId, int textureWidth, int textureHeight, int width, int height, bool rawBgra, uint64_t tick)
    {
        if (!g_ready || textureId == 0 || width <= 0 || height <= 0)
            return;
        double seconds = 0.0;
        bool newPicture = true;
        const double arrival = now();
        if (!lookupNote(tick, seconds, newPicture))
        {
            seconds = arrival;
            newPicture = true;
        }
        const bool interpolate = interpolationWanted();
        // In-between frames are made between new pictures only; without them every frame is shown
        // as soon as it arrives.
        if (interpolate && !newPicture && g_newest >= 0)
            return;

        int pictureWidth = 0, pictureHeight = 0;
        wantedPictureSize(width, height, pictureWidth, pictureHeight);
        resizeTargets(pictureWidth, pictureHeight);

        const int slot = (g_newest + 1 + kSlots) % kSlots;
        Keyframe &k = g_slots[slot];
        if (k.color.id == 0)
            return;
        Texture2D source{};
        source.id = textureId;
        source.width = textureWidth;
        source.height = textureHeight;
        source.mipmaps = 1;
        source.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        scaleInto(k.color, source, width, height, rawBgra);
        if (newPicture || g_pictures == 0)
            ++g_pictures;
        k.seconds = seconds;
        k.sequence = ++g_sequence;
        k.picture = g_pictures;
        k.valid = true;
        k.hasLuma = false;
        k.hasFlow = false;

        if (interpolate)
        {
            ensureMotionTargets(k);
            buildLuma(k);
            const int prev = g_newest;
            if (prev >= 0 && g_slots[prev].valid && g_slots[prev].hasLuma && sizeIs(g_slots[prev].luma[0], g_lumaWidth[0], g_lumaHeight[0]))
                buildFlow(g_slots[prev], k);
            // Pacing: how often new pictures come, and how late they get here.
            if (g_lastKeySeconds > 0.0)
            {
                const double gap = seconds - g_lastKeySeconds;
                if (gap > 0.004 && gap < 0.1)
                    g_interval += (gap - g_interval) * 0.1;
            }
            g_latency = std::max(g_latency * 0.995, std::clamp(arrival - seconds, 0.0, 0.1));
        }
        g_lastKeySeconds = seconds;
        g_newest = slot;
    }

    void draw(float dstX, float dstY, float dstWidth, float dstHeight)
    {
        g_interpolatingNow = false;
        g_statInterpolating.store(false, std::memory_order_relaxed);
        const double drawSeconds = now();
        if (g_lastDrawSeconds > 0.0)
        {
            const double gap = drawSeconds - g_lastDrawSeconds;
            if (gap > 0.001 && gap < 0.1)
                g_drawInterval += (gap - g_drawInterval) * 0.05;
        }
        g_lastDrawSeconds = drawSeconds;
        if (!g_ready || g_newest < 0)
            return;
        const Keyframe &newest = g_slots[g_newest];
        // In-between frames only help when the window shows frames more often than the game makes
        // them (a 60 Hz screen with V-Sync gains nothing over the game's own 60).
        if (!interpolationWanted() || g_drawInterval > g_interval * 0.8)
        {
            drawPlain(newest, dstX, dstY, dstWidth, dstHeight);
            countShown(newest.picture, newest.picture, 1.0f);
            return;
        }

        // The moment shown runs behind real time by one game frame plus the hand-over delay, so
        // the game frame after it is (nearly always) already here.
        const double wantedLag = g_interval + g_latency + 0.002;
        g_lag = g_lag == 0.0 ? wantedLag : g_lag + (wantedLag - g_lag) * 0.05;
        const double shownAt = drawSeconds - g_lag;

        // Newest keyframe at or before the moment shown (a), and the one after it (b).
        int b = g_newest;
        int a = -1;
        for (int step = 0; step < kSlots; ++step)
        {
            const int candidate = (g_newest - step + kSlots) % kSlots;
            const Keyframe &k = g_slots[candidate];
            if (!k.valid)
                break;
            if (k.seconds <= shownAt)
            {
                a = candidate;
                break;
            }
            b = candidate;
        }
        if (a < 0 || a == b)
        {
            // Nothing newer than the moment shown (the game stalled or is paused), or nothing older.
            const Keyframe &k = a < 0 ? g_slots[b] : g_slots[a];
            drawPlain(k, dstX, dstY, dstWidth, dstHeight);
            countShown(k.picture, k.picture, 1.0f);
            return;
        }
        const Keyframe &ka = g_slots[a];
        const Keyframe &kb = g_slots[b];
        const double span = kb.seconds - ka.seconds;
        float t = span > 0.0 ? static_cast<float>((shownAt - ka.seconds) / span) : 1.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        if (!kb.hasFlow || span > 0.1 || slotBefore(b) != a)
        {
            // No motion known between them (a pause, a loading stall, a size change).
            const Keyframe &k = t < 0.5f ? ka : kb;
            drawPlain(k, dstX, dstY, dstWidth, dstHeight);
            countShown(k.picture, k.picture, 1.0f);
            return;
        }

        // Same look as a plain frame: "Sharp" keeps whole pixels when the picture is enlarged.
        const bool sharp = ps2settings::get().filter == 0;
        const bool enlarging = dstWidth > kb.color.texture.width + 0.5f || dstHeight > kb.color.texture.height + 0.5f;
        const int filter = (sharp && enlarging) ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR;
        SetTextureFilter(kb.color.texture, filter);
        SetTextureFilter(ka.color.texture, filter);
        BeginShaderMode(g_interp.shader);
        SetShaderValueTexture(g_interp.shader, g_interp.loc[InColorA], ka.color.texture);
        SetShaderValueTexture(g_interp.shader, g_interp.loc[InFlow], kb.flow.texture);
        setVec2(g_interp, InFlowSize, static_cast<float>(g_lumaWidth[0]), static_cast<float>(g_lumaHeight[0]));
        setFloat(g_interp, InT, t);
        DrawTexturePro(kb.color.texture,
                       Rectangle{0.0f, 0.0f, static_cast<float>(kb.color.texture.width), static_cast<float>(kb.color.texture.height)},
                       Rectangle{dstX, dstY, dstWidth, dstHeight}, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        EndShaderMode();
        g_interpolatingNow = true;
        g_statInterpolating.store(true, std::memory_order_relaxed);
        countShown(ka.picture, kb.picture, t);
    }

    bool ready() noexcept
    {
        return g_ready;
    }

    int targetFps() noexcept
    {
        const int mode = ps2settings::get().frameRate;
        if (mode == 1 || (mode == 2 && ps2android::active())) // a phone has no "Monitor" choice
            return 120;
        if (mode == 2)
        {
            // Capped a little above the monitor's rate: V-Sync (when it works) sets the pace, and
            // when the driver ignores it the window still does not draw thousands of frames.
            const int refresh = GetMonitorRefreshRate(GetCurrentMonitor());
            return refresh > 0 ? refresh + std::max(2, refresh / 50) : 144;
        }
        return 60;
    }

    void afterPresent()
    {
#if defined(_WIN32)
        if (!ps2settings::compositorSync())
            return;
        using DwmFlushFn = long(__stdcall *)();
        static DwmFlushFn flush = []() -> DwmFlushFn
        {
            void *module = LoadLibraryA("dwmapi.dll");
            return module ? reinterpret_cast<DwmFlushFn>(GetProcAddress(module, "DwmFlush")) : nullptr;
        }();
        if (flush)
            flush();
#endif
    }

    // These three are read by the performance log's own thread.
    double shownFps() noexcept
    {
        return g_statShownFps.load(std::memory_order_relaxed);
    }

    bool interpolating() noexcept
    {
        return g_statInterpolating.load(std::memory_order_relaxed);
    }

    void pictureSize(int &width, int &height) noexcept
    {
        width = g_statPictureWidth.load(std::memory_order_relaxed);
        height = g_statPictureHeight.load(std::memory_order_relaxed);
    }
}
