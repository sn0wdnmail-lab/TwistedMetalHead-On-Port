#include <chrono>
#include "iop_spu2.h"

#include "ps2x/iop/iop_audio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace ps2x::iop
{
    namespace
    {
        // Host audio buffer: interleaved stereo int16 frames made by the SPU2 (game thread) and
        // pulled by the sound card's callback.
        //
        // The game's clock and the sound card's never quite agree, and the game can also run a
        // little slow for a while in very busy scenes. A plain FIFO then runs dry (gaps of
        // silence) or overflows (cut audio) every few seconds - the crackle of earlier versions.
        // Here the playback tempo follows how much is buffered (about 70 ms is kept):
        //   - tiny differences (under 0.5%) are taken up by resampling, which nobody can hear;
        //   - larger ones (the game running at 90% for a few seconds) by time stretching: short
        //     pieces of the sound are repeated or skipped where the waveform lines up (WSOLA),
        //     which changes the tempo but not the pitch.
        constexpr size_t kCapacity = 1u << 15;      // frames (680 ms)
        constexpr double kTargetFrames = 3360.0;    // 70 ms
        constexpr size_t kPrimeFrames = 2880u;      // start (again) once 60 ms are buffered
        constexpr int kFadeFrames = 96;             // 2 ms ramps instead of clicks
        constexpr double kResampleLimit = 0.005;    // beyond this tempo change, time stretching

        // Time stretching: output pieces of kHop frames, each joined to the last with a
        // kOverlap crossfade at the input offset (within +-kSearch) that matches best.
        constexpr int kHop = 960;      // 20 ms
        constexpr int kOverlap = 384;  // 8 ms
        constexpr int kSearch = 480;   // +-10 ms

        std::mutex g_ringMutex;
        int16_t g_ring[kCapacity * 2u];
        uint64_t g_writeFrame = 0;   // frames ever written
        uint64_t g_readFrame = 0;    // oldest frame still needed

        struct Player
        {
            bool primed = false;
            float gain = 0.0f;                  // fade in after (re)starting
            float lastLeft = 0.0f, lastRight = 0.0f;
            double fillAverage = -1.0;
            double integral = 0.0;              // learned long-term tempo offset (clock drift)
            // How fast the game makes sound (frames per second of real time), smoothed: when it
            // runs slow for a while the tempo follows it straight away.
            // Measured against the sound card's own clock (frames it has taken), not the PC's.
            double produceRate = 48000.0;
            uint64_t ratePulled = 0;            // frames handed to the sound card so far
            static constexpr int kMarks = 6;    // a 1.5 s window, in quarter seconds
            uint64_t markPulled[kMarks]{};
            uint64_t markWrite[kMarks]{};
            int markCount = 0;
            int slowVotes = 0;
            bool following = false;             // tempo set from produceRate
            double tempo = 1.0;                 // input frames per output frame
            double fraction = 0.0;              // resampling: position between g_readFrame and the next frame
            // Time stretching.
            bool stretching = false;
            int stretchVotes = 0;
            uint64_t segmentStart = 0;          // input frame the current piece starts at
            double nominal = 0.0;               // where it would start at exactly this tempo
            int segmentPos = kHop;              // frames of the current piece already played
            float segment[kHop][2]{};
        } g_player;

        std::atomic<uint64_t> g_statPushed{0}, g_statPulled{0}, g_statSilent{0}, g_statUnderruns{0}, g_statDropped{0};
        std::atomic<uint64_t> g_statCallbacks{0}, g_statStretched{0};
        std::atomic<int64_t> g_statBuffered{0};
        std::atomic<int32_t> g_statRate{0};

        inline float frameAt(uint64_t frame, int channel)
        {
            return static_cast<float>(g_ring[(frame % kCapacity) * 2u + static_cast<size_t>(channel)]);
        }

        inline float monoAt(uint64_t frame)
        {
            const size_t at = (frame % kCapacity) * 2u;
            return static_cast<float>(g_ring[at]) + static_cast<float>(g_ring[at + 1u]);
        }

        // Catmull-Rom between frames i and i+1 at t (needs i-1 .. i+2).
        inline float cubic(uint64_t i, float t, int channel)
        {
            const float p0 = frameAt(i == 0 ? 0 : i - 1, channel);
            const float p1 = frameAt(i, channel);
            const float p2 = frameAt(i + 1, channel);
            const float p3 = frameAt(i + 2, channel);
            return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
        }

        inline int16_t toPcm(float value)
        {
            return static_cast<int16_t>(std::clamp(std::lround(value), -32768l, 32767l));
        }

        // Similarity of the kOverlap frames at `candidate` with those at `reference` (normalized
        // cross-correlation of the mono signal), using every `step`-th frame.
        float similarity(uint64_t candidate, uint64_t reference, int step)
        {
            float cross = 0.0f, energy = 1.0f;
            for (int i = 0; i < kOverlap; i += step)
            {
                const float c = monoAt(candidate + static_cast<uint64_t>(i));
                cross += c * monoAt(reference + static_cast<uint64_t>(i));
                energy += c * c;
            }
            return cross / std::sqrt(energy);
        }

        // Makes the next stretched piece: kHop frames starting near g_player.nominal, crossfaded
        // over its first kOverlap frames from the natural continuation of the previous piece.
        // False when the input does not reach far enough yet.
        bool makeSegment()
        {
            Player &p = g_player;
            const uint64_t continuation = p.segmentStart + static_cast<uint64_t>(kHop);
            const double nextNominal = p.nominal + p.tempo * kHop;
            const int64_t centre = static_cast<int64_t>(std::llround(nextNominal));
            const int64_t lowest = std::max<int64_t>(centre - kSearch, static_cast<int64_t>(g_readFrame) + 1);
            // The search reaches as far ahead as the input allows (the whole piece must be here).
            const int64_t highest = std::min<int64_t>(centre + kSearch, static_cast<int64_t>(g_writeFrame) - kHop - 2);
            if (continuation + kOverlap + 2u >= g_writeFrame || lowest > highest)
                return false;
            // Coarse search on every 3rd frame and offset, then refine around the best.
            int64_t best = std::clamp<int64_t>(centre, lowest, highest);
            float bestScore = -1e30f;
            for (int64_t c = lowest; c <= highest; c += 3)
            {
                const float score = similarity(static_cast<uint64_t>(c), continuation, 3);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = c;
                }
            }
            const int64_t coarse = best;
            bestScore = -1e30f;
            for (int64_t c = std::max(lowest, coarse - 2); c <= std::min(highest, coarse + 2); ++c)
            {
                const float score = similarity(static_cast<uint64_t>(c), continuation, 1);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = c;
                }
            }
            const uint64_t start = static_cast<uint64_t>(best);
            for (int i = 0; i < kHop; ++i)
            {
                float left = frameAt(start + static_cast<uint64_t>(i), 0);
                float right = frameAt(start + static_cast<uint64_t>(i), 1);
                if (i < kOverlap)
                {
                    // Raised-cosine crossfade from the previous piece's continuation.
                    const float w = 0.5f - 0.5f * std::cos(3.14159265f * (static_cast<float>(i) + 0.5f) / kOverlap);
                    left = left * w + frameAt(continuation + static_cast<uint64_t>(i), 0) * (1.0f - w);
                    right = right * w + frameAt(continuation + static_cast<uint64_t>(i), 1) * (1.0f - w);
                }
                p.segment[i][0] = left;
                p.segment[i][1] = right;
            }
            p.segmentStart = start;
            p.nominal = nextNominal;
            p.segmentPos = 0;
            // Nothing before the next search window or the next continuation is needed again.
            const double nextCentre = nextNominal + p.tempo * kHop;
            const uint64_t keep = std::min<uint64_t>(start + static_cast<uint64_t>(kHop),
                                                     static_cast<uint64_t>(std::max(0.0, nextCentre - kSearch - 4.0)));
            if (keep > g_readFrame)
                g_readFrame = std::min(keep, g_writeFrame);
            g_statStretched.fetch_add(kHop, std::memory_order_relaxed);
            return true;
        }

        // Input frames not yet played (roughly, in either mode).
        double bufferedNow()
        {
            const Player &p = g_player;
            if (p.stretching)
            {
                const double position = p.nominal + p.segmentPos * p.tempo;
                return static_cast<double>(g_writeFrame) - position;
            }
            return static_cast<double>(g_writeFrame - g_readFrame) - p.fraction;
        }
    }

    std::atomic<uint64_t> g_statClipped{0};
    std::atomic<int32_t> g_effectsVolumeQ15{32768};
    std::atomic<int32_t> g_musicVolumeQ15{32768};
    std::atomic<int32_t> g_movieVolumeQ15{32768};

    void setMixVolumes(int32_t effectsQ15, int32_t musicQ15, int32_t movieQ15) noexcept
    {
        g_effectsVolumeQ15.store(std::clamp<int32_t>(effectsQ15, 0, 32768), std::memory_order_relaxed);
        g_musicVolumeQ15.store(std::clamp<int32_t>(musicQ15, 0, 32768), std::memory_order_relaxed);
        g_movieVolumeQ15.store(std::clamp<int32_t>(movieQ15, 0, 32768), std::memory_order_relaxed);
    }
    std::atomic<uint64_t> g_statNoise{0};
    std::atomic<uint64_t> g_statPitchMod{0};

    namespace
    {
        // PS2X_AUDIO_DUMP / PS2X_AUDIO_OUT_DUMP (testing, settings.ini audio_dump=1): raw 48 kHz
        // s16le stereo files of the SPU2's mix and of what the sound card was given. Capped at 15
        // minutes each; the sound card's starts once the game makes sound.
        struct Dump
        {
            FILE *file = nullptr;
            uint64_t frames = 0;
            explicit Dump(const char *variable)
            {
                const char *path = std::getenv(variable);
                if (path && *path)
                    file = std::fopen(path, "wb");
            }
            void write(const int16_t *data, size_t count)
            {
                constexpr uint64_t kMaxFrames = 48000ull * 60ull * 15ull;
                if (!file || frames >= kMaxFrames)
                    return;
                count = static_cast<size_t>(std::min<uint64_t>(count, kMaxFrames - frames));
                std::fwrite(data, sizeof(int16_t) * 2u, count, file);
                frames += count;
                if (frames >= kMaxFrames)
                {
                    std::fclose(file);
                    file = nullptr;
                }
            }
        };
    }

    void pushAudioFrames(const int16_t *interleaved, size_t frames)
    {
        static Dump dump("PS2X_AUDIO_DUMP");
        dump.write(interleaved, frames);
        std::lock_guard<std::mutex> lock(g_ringMutex);
        if (frames > kCapacity / 2u)
        {
            interleaved += (frames - kCapacity / 2u) * 2u;
            frames = kCapacity / 2u;
        }
        const uint64_t buffered = g_writeFrame - g_readFrame;
        if (buffered + frames + 8u > kCapacity)
        {
            // Nobody is listening (or the sound card stalled): keep the newest audio, about the
            // usual amount of it, and start again from there.
            const uint64_t keep = static_cast<uint64_t>(kTargetFrames);
            const uint64_t drop = buffered + frames > keep ? std::min<uint64_t>(buffered, buffered + frames - keep) : 0u;
            g_readFrame += drop;
            g_player.primed = false;
            g_player.stretching = false;
            g_player.fraction = 0.0;
            g_statDropped.fetch_add(drop, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < frames; ++i)
        {
            const size_t at = ((g_writeFrame + i) % kCapacity) * 2u;
            g_ring[at] = interleaved[i * 2u];
            g_ring[at + 1u] = interleaved[i * 2u + 1u];
        }
        g_writeFrame += frames;
        g_statPushed.fetch_add(frames, std::memory_order_relaxed);
        g_statBuffered.store(static_cast<int64_t>(g_writeFrame - g_readFrame), std::memory_order_relaxed);
    }

    size_t pullAudioFrames(int16_t *interleaved, size_t frames)
    {
        std::lock_guard<std::mutex> lock(g_ringMutex);
        Player &p = g_player;
        g_statCallbacks.fetch_add(1u, std::memory_order_relaxed);
        {
            // A sound card that asks for sound far faster than it can play it (seen once at start-up
            // on Windows) is reported, as it keeps a CPU core busy for nothing.
            static auto since = std::chrono::steady_clock::now();
            static uint64_t asked = 0;
            static int reports = 0;
            asked += frames;
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
            if (elapsed >= 1.0)
            {
                if (asked > static_cast<uint64_t>(elapsed * 48000.0 * 3.0) && reports < 20)
                {
                    ++reports;
                    std::fprintf(stderr, "[audio] the sound card asked for %.1f s of sound in %.1f s\n", asked / 48000.0, elapsed);
                }
                since = std::chrono::steady_clock::now();
                asked = 0;
            }
        }
        size_t produced = 0;
        size_t i = 0;
        p.ratePulled += frames;
        if (p.markCount == 0 || (p.markCount > 1 && g_writeFrame == p.markWrite[(p.markCount - 1) % Player::kMarks] &&
                                 g_writeFrame == p.markWrite[(p.markCount - 2) % Player::kMarks]))
        {
            // Starting, or the game has made no sound for half a second (loading): start afresh.
            p.markCount = 1;
            p.markPulled[0] = p.ratePulled;
            p.markWrite[0] = g_writeFrame;
            p.produceRate = 48000.0;
            p.slowVotes = 0;
            p.following = false;
        }
        if (p.ratePulled - p.markPulled[(p.markCount - 1) % Player::kMarks] >= 12000u)
        {
            // A new quarter-second mark; the rate over the window back to the oldest kept mark
            // (the game hands sound over in bursts, so shorter spans are noisy).
            const int slot = p.markCount % Player::kMarks;
            p.markPulled[slot] = p.ratePulled;
            p.markWrite[slot] = g_writeFrame;
            ++p.markCount;
            const int oldest = p.markCount > Player::kMarks ? p.markCount % Player::kMarks : 0;
            const double dt = static_cast<double>(p.ratePulled - p.markPulled[oldest]) / 48000.0;
            if (dt > 0.4)
            {
                p.produceRate = static_cast<double>(g_writeFrame - p.markWrite[oldest]) / dt;
                // Following the game's pace once it has been more than 2% off for a while.
                const bool off = std::fabs(p.produceRate / 48000.0 - 1.0) > 0.025;
                p.slowVotes = off ? std::min(p.slowVotes + 1, 8) : std::max(p.slowVotes - 1, 0);
                if (p.slowVotes >= 2)
                    p.following = true;
                else if (p.slowVotes == 0)
                    p.following = false;
            }
        }
        if (!p.primed && g_writeFrame - g_readFrame >= kPrimeFrames)
        {
            p.primed = true;
            p.gain = 0.0f;
            p.fillAverage = -1.0;
            p.fraction = 0.0;
            p.stretching = false;
            p.stretchVotes = 0;
        }
        if (p.primed)
        {
            // Tempo: a slow loop on how much is buffered (smoothed over about half a second), with
            // an integral that learns a lasting difference (the game running slow, clock drift).
            const double fill = bufferedNow();
            const double alpha = std::min(1.0, static_cast<double>(frames) / (48000.0 * 0.5));
            p.fillAverage = p.fillAverage < 0.0 ? fill : p.fillAverage + (fill - p.fillAverage) * alpha;
            const double error = std::clamp((p.fillAverage - kTargetFrames) / kTargetFrames, -1.0, 1.5);
            const double seconds = static_cast<double>(frames) / 48000.0;
            // Gentle near the target, firmer when far off.
            const double proportional = error * 0.01 + std::copysign(std::max(0.0, std::fabs(error) - 0.45) * 0.2, error);
            if (p.following)
            {
                // The game runs slow (or fast) for now: play at its pace, steered toward the target.
                p.tempo = std::clamp(p.produceRate / 48000.0 * (1.0 + proportional), 0.5, 1.2);
            }
            else
            {
                // Clock drift: a small integral, never more than resampling takes.
                p.integral = std::clamp(p.integral + error * 0.02 * seconds, -kResampleLimit * 0.8, kResampleLimit * 0.8);
                p.tempo = std::clamp(1.0 + proportional + p.integral, 0.6, 1.2);
            }
            g_statRate.store(static_cast<int32_t>(std::lround((p.tempo - 1.0) * 10000.0)), std::memory_order_relaxed);
            static const bool trace = std::getenv("PS2X_AUDIO_TRACE") != nullptr;
            static uint64_t traceCount = 0;
            if (trace && (traceCount++ % 50u) == 0u)
                std::fprintf(stderr, "[audio] fill %.0f avg %.0f err %+.3f int %+.4f rate %.0f%s tempo %+.4f %s read %llu write %llu\n", fill,
                             p.fillAverage, error, p.integral, p.produceRate, p.following ? " FOLLOW" : "", p.tempo - 1.0,
                             p.stretching ? "STRETCH" : "resample", (unsigned long long)g_readFrame, (unsigned long long)g_writeFrame);

            // Resampling or time stretching (with some hysteresis).
            const double away = std::fabs(p.tempo - 1.0);
            if (!p.stretching)
            {
                p.stretchVotes = away > 0.008 ? p.stretchVotes + 1 : 0;
                if (p.stretchVotes >= 3)
                {
                    // Start stretching where playback is: as if a piece had just ended here.
                    p.stretching = true;
                    p.segmentStart = g_readFrame - std::min<uint64_t>(g_readFrame, static_cast<uint64_t>(kHop));
                    p.nominal = static_cast<double>(g_readFrame) - p.tempo * kHop;
                    p.segmentPos = kHop;
                    p.fraction = 0.0;
                    p.stretchVotes = 0;
                }
            }
            else
            {
                p.stretchVotes = away < 0.003 ? p.stretchVotes + 1 : 0;
            }

            for (; i < frames; ++i)
            {
                float left, right;
                if (p.stretching)
                {
                    if (p.segmentPos >= kHop)
                    {
                        // Back to plain playback once the tempo has settled near 1, at the
                        // natural continuation of the last piece.
                        if (p.stretchVotes >= 25)
                        {
                            p.stretching = false;
                            p.stretchVotes = 0;
                            g_readFrame = std::min(p.segmentStart + static_cast<uint64_t>(kHop), g_writeFrame);
                            p.fraction = 0.0;
                            --i;
                            continue;
                        }
                        if (!makeSegment())
                        {
                            p.primed = false;
                            p.stretching = false;
                            g_readFrame = std::min(p.segmentStart + static_cast<uint64_t>(kHop), g_writeFrame);
                            g_statUnderruns.fetch_add(1u, std::memory_order_relaxed);
                            break;
                        }
                    }
                    left = p.segment[p.segmentPos][0];
                    right = p.segment[p.segmentPos][1];
                    ++p.segmentPos;
                }
                else
                {
                    // Frames i .. i+2 must be here (Catmull-Rom).
                    if (g_readFrame + 2u >= g_writeFrame)
                    {
                        p.primed = false;
                        g_statUnderruns.fetch_add(1u, std::memory_order_relaxed);
                        break;
                    }
                    const float t = static_cast<float>(p.fraction);
                    left = cubic(g_readFrame, t, 0);
                    right = cubic(g_readFrame, t, 1);
                    p.fraction += std::clamp(p.tempo, 1.0 - kResampleLimit, 1.0 + kResampleLimit);
                    const double whole = std::floor(p.fraction);
                    g_readFrame += static_cast<uint64_t>(whole);
                    p.fraction -= whole;
                }
                if (p.gain < 1.0f)
                {
                    p.gain = std::min(1.0f, p.gain + 1.0f / kFadeFrames);
                    left *= p.gain;
                    right *= p.gain;
                }
                interleaved[i * 2u] = toPcm(left);
                interleaved[i * 2u + 1u] = toPcm(right);
                p.lastLeft = left;
                p.lastRight = right;
                ++produced;
            }
        }
        // Ran dry (or not started yet): fade what was playing out instead of cutting it.
        for (; i < frames; ++i)
        {
            p.lastLeft *= 0.97f;
            p.lastRight *= 0.97f;
            interleaved[i * 2u] = toPcm(p.lastLeft);
            interleaved[i * 2u + 1u] = toPcm(p.lastRight);
        }
        g_statPulled.fetch_add(frames, std::memory_order_relaxed);
        g_statSilent.fetch_add(frames - produced, std::memory_order_relaxed);
        static Dump outDump("PS2X_AUDIO_OUT_DUMP");
        if (g_statPushed.load(std::memory_order_relaxed) != 0u)
            outDump.write(interleaved, frames);
        g_statBuffered.store(static_cast<int64_t>(bufferedNow()), std::memory_order_relaxed);
        return produced;
    }

    AudioStats audioStats() noexcept
    {
        AudioStats s;
        s.pushedFrames = g_statPushed.load(std::memory_order_relaxed);
        s.pulledFrames = g_statPulled.load(std::memory_order_relaxed);
        s.silentFrames = g_statSilent.load(std::memory_order_relaxed);
        s.underruns = g_statUnderruns.load(std::memory_order_relaxed);
        s.droppedFrames = g_statDropped.load(std::memory_order_relaxed);
        s.clippedSamples = g_statClipped.load(std::memory_order_relaxed);
        s.noiseVoiceWrites = g_statNoise.load(std::memory_order_relaxed);
        s.pitchModWrites = g_statPitchMod.load(std::memory_order_relaxed);
        s.bufferedFrames = g_statBuffered.load(std::memory_order_relaxed);
        s.rateTenthsPermille = g_statRate.load(std::memory_order_relaxed);
        s.callbacks = g_statCallbacks.load(std::memory_order_relaxed);
        s.stretchedFrames = g_statStretched.load(std::memory_order_relaxed);
        return s;
    }
}

namespace ps2x::iop::detail
{
    namespace
    {
        // Core register offsets (relative to the core base).
        constexpr uint32_t kPmon = 0x180u;
        constexpr uint32_t kVmixl = 0x188u;
        constexpr uint32_t kVmixr = 0x190u;
        constexpr uint32_t kAttr = 0x19Au;
        constexpr uint32_t kIrqa = 0x19Cu;
        constexpr uint32_t kKon = 0x1A0u;
        constexpr uint32_t kKoff = 0x1A4u;
        constexpr uint32_t kTsa = 0x1A8u;
        constexpr uint32_t kData = 0x1ACu;
        constexpr uint32_t kAdmas = 0x1B0u;
        constexpr uint32_t kVoiceAddr = 0x1C0u;
        constexpr uint32_t kEndx = 0x340u;
        constexpr uint32_t kStatx = 0x344u;
        constexpr uint32_t kIrqInfo = 0x7C2u;
        constexpr uint16_t kAttrIrqEnable = 0x40u;

        constexpr int32_t kAdpcmFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};

        [[nodiscard]] uint32_t masterVolumeBase(int core) { return core ? 0x788u : 0x760u; }

        [[nodiscard]] int32_t volumeOf(uint16_t raw)
        {
            if (raw & 0x8000u)
                return 0x7FFF; // sweep mode (only where no sweep state is kept)
            return static_cast<int16_t>(static_cast<uint16_t>(raw << 1));
        }

        // 4-tap interpolation weights (sum 1 << 15) for 256 positions between the middle two
        // taps: a Gaussian (sigma 0.6 samples) like the SPU2's own table - smooth, without the
        // aliasing hiss that straight-line interpolation adds to pitched-up sounds.
        struct GaussTable
        {
            int32_t w[256][4];
            GaussTable()
            {
                for (int i = 0; i < 256; ++i)
                {
                    const double f = i / 256.0;
                    const double d[4] = {1.0 + f, f, 1.0 - f, 2.0 - f};
                    double raw[4];
                    double sum = 0.0;
                    for (int k = 0; k < 4; ++k)
                    {
                        raw[k] = std::exp(-(d[k] * d[k]) / (2.0 * 0.6 * 0.6));
                        sum += raw[k];
                    }
                    int32_t total = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        w[i][k] = static_cast<int32_t>(std::lround(raw[k] / sum * 32768.0));
                        total += w[i][k];
                    }
                    w[i][1] += 32768 - total;
                }
            }
        };
        const GaussTable g_gauss;

        // The final mix into 16 bits. The hardware simply cuts off what does not fit, which
        // crackles when a loud scene (several explosions at once) goes over; here the peaks above
        // -2.5 dB are rounded off smoothly toward full scale instead. Quieter sound is untouched.
        [[nodiscard]] int16_t softClip(int32_t value)
        {
            constexpr int32_t kKnee = 24576;
            const int32_t magnitude = value < 0 ? -value : value;
            if (magnitude <= kKnee)
                return static_cast<int16_t>(value);
            const float over = static_cast<float>(magnitude - kKnee) / static_cast<float>(32767 - kKnee);
            const int32_t rounded = static_cast<int32_t>(std::lround(kKnee + (32767 - kKnee) * std::tanh(over)));
            return static_cast<int16_t>(value < 0 ? -rounded : rounded);
        }

        // EVOL/AVOL/BVOL are plain signed 16-bit volumes (no sweep mode).
        [[nodiscard]] int32_t plainVolume(uint16_t raw)
        {
            return static_cast<int16_t>(raw);
        }

        [[nodiscard]] int16_t clamp16(int32_t value)
        {
            return static_cast<int16_t>(std::clamp<int32_t>(value, -32768, 32767));
        }

        const bool g_traceSpu = []()
        {
            const char *env = std::getenv("PS2X_TRACE_SPU2");
            return env && *env && *env != '0';
        }();
    }

    Spu2::Spu2()
        : m_blockWriteStamp(RamHalfwords >> kStampShift, 0u), m_ram(RamHalfwords, 0u)
    {
        reset();
    }

    void Spu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        std::fill(m_blockWriteStamp.begin(), m_blockWriteStamp.end(), 0u);
        m_sampleCount = 1;
        m_regs.fill(0u);
        for (Core &core : m_cores)
        {
            core = Core{};
        }
        m_cycle = 0;
        m_cycleValid = false;
        m_irqPending = false;
    }

    uint32_t Spu2::coreAddress(int core, uint32_t highOffset) const
    {
        const uint32_t base = coreBase(core) + highOffset;
        return ((static_cast<uint32_t>(reg(base)) & 0xFu) << 16) | reg(base + 2u);
    }

    uint32_t Spu2::voiceAddress(int core, int voice, uint32_t field) const
    {
        return coreAddress(core, kVoiceAddr + static_cast<uint32_t>(voice) * 0xCu + field);
    }

    void Spu2::setVoiceAddress(int core, int voice, uint32_t field, uint32_t address)
    {
        const uint32_t base = coreBase(core) + kVoiceAddr + static_cast<uint32_t>(voice) * 0xCu + field;
        reg(base) = static_cast<uint16_t>((address >> 16) & 0xFu);
        reg(base + 2u) = static_cast<uint16_t>(address);
    }

    uint16_t Spu2::read16(uint32_t offset) const
    {
        offset &= 0x7FEu;
        if (offset < 0x760u)
        {
            const int core = offset >= 0x400u ? 1 : 0;
            const uint32_t local = offset - coreBase(core);
            const Core &c = m_cores[static_cast<size_t>(core)];
            if (local < 0x180u)
            {
                const Voice &v = c.voices[local >> 4];
                if ((local & 0xFu) == 0xAu) // ENVX
                    return static_cast<uint16_t>(v.envelope);
            }
            else if (local >= kVoiceAddr && local < kVoiceAddr + 24u * 0xCu)
            {
                const uint32_t voice = (local - kVoiceAddr) / 0xCu;
                const uint32_t field = (local - kVoiceAddr) % 0xCu;
                if (field == 8u || field == 0xAu) // NAX
                {
                    const Voice &v = c.voices[voice];
                    const uint32_t nax = (v.block + 1u + static_cast<uint32_t>(v.sampleIndex >> 2)) & (RamHalfwords - 1u);
                    return field == 8u ? static_cast<uint16_t>(nax >> 16) : static_cast<uint16_t>(nax);
                }
            }
            else if (local == kEndx)
                return static_cast<uint16_t>(c.endx);
            else if (local == kEndx + 2u)
                return static_cast<uint16_t>(c.endx >> 16);
            else if (local == kTsa)
                return static_cast<uint16_t>(c.tsa >> 16);
            else if (local == kTsa + 2u)
                return static_cast<uint16_t>(c.tsa);
        }
        return reg(offset);
    }

    void Spu2::write16(uint32_t offset, uint16_t value)
    {
        offset &= 0x7FEu;
        if (g_traceSpu)
            std::fprintf(stderr, "[spu2] w %03x = %04x\n", offset, value);
        if (offset >= 0x760u)
        {
            if (offset == kIrqInfo)
            {
                reg(offset) &= static_cast<uint16_t>(~value);
                return;
            }
            reg(offset) = value;
            return;
        }

        const int core = offset >= 0x400u ? 1 : 0;
        const uint32_t local = offset - coreBase(core);
        Core &c = m_cores[static_cast<size_t>(core)];
        if (local >= kVoiceAddr && local < kVoiceAddr + 24u * 0xCu)
        {
            reg(offset) = value;
            const uint32_t voice = (local - kVoiceAddr) / 0xCu;
            const uint32_t field = (local - kVoiceAddr) % 0xCu;
            Voice &v = c.voices[voice];
            if (field == 0u || field == 2u)
                v.ssa = voiceAddress(core, static_cast<int>(voice), 0u);
            else if (field == 4u || field == 6u)
            {
                v.lsa = voiceAddress(core, static_cast<int>(voice), 4u);
                v.lsaFromSoftware = true;
            }
            return;
        }

        switch (local)
        {
        case kKon:
            keyOn(core, value);
            return;
        case kKon + 2u:
            keyOn(core, static_cast<uint32_t>(value & 0xFFu) << 16);
            return;
        case kKoff:
            keyOff(core, value);
            return;
        case kKoff + 2u:
            keyOff(core, static_cast<uint32_t>(value & 0xFFu) << 16);
            return;
        case kEndx:
            c.endx &= 0xFF0000u;
            return;
        case kEndx + 2u:
            c.endx &= 0xFFFFu;
            return;
        case kTsa:
        case kTsa + 2u:
            reg(offset) = value;
            c.tsa = coreAddress(core, kTsa);
            return;
        case kData:
            m_ram[c.tsa & (RamHalfwords - 1u)] = value;
            stampWrite(c.tsa, 1u);
            checkIrq(c.tsa, 1u);
            c.tsa = (c.tsa + 1u) & (RamHalfwords - 1u);
            return;
        case kAttr:
            reg(offset) = value;
            if ((value & kAttrIrqEnable) == 0u)
                reg(kIrqInfo) &= static_cast<uint16_t>(~(4u << core));
            if (value & 0x8000u)
                ; // core enable
            return;
        case kStatx:
            return;
        default:
            if ((local == 0x184u || local == 0x186u) && value != 0u && reg(offset) != value)
                ps2x::iop::g_statNoise.fetch_add(1u, std::memory_order_relaxed); // NON: noise generator (not emulated)
            if ((local == kPmon || local == kPmon + 2u) && value != 0u && reg(offset) != value)
                ps2x::iop::g_statPitchMod.fetch_add(1u, std::memory_order_relaxed); // PMON (not emulated)
            reg(offset) = value;
            return;
        }
    }

    void Spu2::keyOn(int core, uint32_t mask)
    {
        Core &c = m_cores[static_cast<size_t>(core)];
        for (int i = 0; i < 24; ++i)
        {
            if ((mask & (1u << i)) == 0u)
                continue;
            Voice &v = c.voices[static_cast<size_t>(i)];
            v.ssa = voiceAddress(core, i, 0u);
            v.block = v.ssa;
            v.lsaFromSoftware = false;
            v.counter = 0;
            v.sampleIndex = 0;
            v.hist1 = v.hist2 = 0;
            v.on = true;
            v.phase = 1;
            v.envelope = 0;
            v.envelopeWait = 0;
            v.keyOnStamp = m_sampleCount;
            v.streaming = false;
            c.endx &= ~(1u << i);
            decodeBlock(core, v);
            v.history[0] = v.history[1] = v.history[2] = 0;
            v.history[3] = v.samples[0];
            if (g_traceSpu)
                std::fprintf(stderr, "[spu2] kon core%d v%d ssa=%05x hdr=%04x pitch=%04x s0=%d\n", core, i, v.ssa,
                             m_ram[v.ssa], reg(coreBase(core) + static_cast<uint32_t>(i) * 0x10u + 4u), v.samples[0]);
        }
    }

    void Spu2::keyOff(int core, uint32_t mask)
    {
        Core &c = m_cores[static_cast<size_t>(core)];
        for (int i = 0; i < 24; ++i)
        {
            if ((mask & (1u << i)) == 0u)
                continue;
            Voice &v = c.voices[static_cast<size_t>(i)];
            if (v.phase != 0)
            {
                v.phase = 4;
                v.envelopeWait = 0;
            }
        }
    }

    void Spu2::stampWrite(uint32_t start, uint32_t halfwords)
    {
        if (m_blockWriteStamp.empty() || halfwords == 0u)
            return;
        const uint32_t first = (start & (RamHalfwords - 1u)) >> kStampShift;
        const uint32_t blocks = ((halfwords + (start & ((1u << kStampShift) - 1u))) >> kStampShift) + 1u;
        const uint32_t count = static_cast<uint32_t>(m_blockWriteStamp.size());
        for (uint32_t i = 0; i < blocks && i < count; ++i)
            m_blockWriteStamp[(first + i) % count] = m_sampleCount;
    }

    void Spu2::decodeBlock(int /*core*/, Voice &v)
    {
        const uint32_t base = v.block & (RamHalfwords - 1u);
        checkIrq(base, 8u);
        // Written after the voice started: the sound driver is refilling it as it plays (a stream).
        if (!v.streaming && !m_blockWriteStamp.empty() &&
            static_cast<int32_t>(m_blockWriteStamp[base >> kStampShift] - v.keyOnStamp) > 0)
        {
            v.streaming = true;
            static const bool trace = std::getenv("PS2X_TRACE_STREAMS") != nullptr;
            if (trace)
                std::fprintf(stderr, "[spu2] stream voice at %05x (keyed on %u samples ago)\n", base, m_sampleCount - v.keyOnStamp);
        }
        const uint16_t header = m_ram[base];
        const int shift = std::min(header & 0xF, 12);
        const int filter = std::min((header >> 4) & 0xF, 4);
        v.flags = static_cast<uint8_t>(header >> 8);
        if ((v.flags & 4u) && !v.lsaFromSoftware)
            v.lsa = base;
        for (int i = 0; i < 28; ++i)
        {
            const uint16_t word = m_ram[(base + 1u + static_cast<uint32_t>(i >> 2)) & (RamHalfwords - 1u)];
            const int nibble = (word >> ((i & 3) * 4)) & 0xF;
            int32_t sample = static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(nibble << 12))) >> shift;
            sample += (v.hist1 * kAdpcmFilter[filter][0] + v.hist2 * kAdpcmFilter[filter][1]) >> 6;
            sample = std::clamp<int32_t>(sample, -32768, 32767);
            v.hist2 = v.hist1;
            v.hist1 = sample;
            v.samples[i] = static_cast<int16_t>(sample);
        }
    }

    void Spu2::nextSample(int core, int index, Voice &v)
    {
        if (++v.sampleIndex < 28)
            return;
        v.sampleIndex = 0;
        Core &c = m_cores[static_cast<size_t>(core)];
        if (v.flags & 1u) // loop end
        {
            c.endx |= 1u << index;
            v.block = v.lsa;
            if ((v.flags & 2u) == 0u) // no repeat: the voice is silenced
            {
                v.phase = 0;
                v.envelope = 0;
                v.on = false;
            }
        }
        else
        {
            v.block = (v.block + 8u) & (RamHalfwords - 1u);
        }
        decodeBlock(core, v);
    }

    void Spu2::stepEnvelope(int core, int index, Voice &v)
    {
        const uint32_t base = coreBase(core) + static_cast<uint32_t>(index) * 0x10u;
        const uint16_t adsr1 = reg(base + 6u);
        const uint16_t adsr2 = reg(base + 8u);

        int shift = 0;
        int step = 0;
        bool exponential = false;
        bool decrease = false;
        switch (v.phase)
        {
        case 1: // attack
            shift = (adsr1 >> 10) & 0x1F;
            step = 7 - ((adsr1 >> 8) & 3);
            exponential = (adsr1 & 0x8000u) != 0u;
            break;
        case 2: // decay
            shift = (adsr1 >> 4) & 0xF;
            step = -8;
            exponential = true;
            decrease = true;
            break;
        case 3: // sustain
        {
            const int rate = (adsr2 >> 6) & 0x7F;
            if (rate == 0x7F)
                return;
            shift = rate >> 2;
            decrease = (adsr2 & 0x4000u) != 0u;
            step = decrease ? -8 + (rate & 3) : 7 - (rate & 3);
            exponential = (adsr2 & 0x8000u) != 0u;
            break;
        }
        case 4: // release
            shift = adsr2 & 0x1F;
            step = -8;
            exponential = (adsr2 & 0x20u) != 0u;
            decrease = true;
            break;
        default:
            return;
        }

        if (v.envelopeWait > 0)
        {
            --v.envelopeWait;
            return;
        }

        int32_t cycles = 1 << std::max(0, shift - 11);
        int32_t delta = step << std::max(0, 11 - shift);
        if (exponential && !decrease && v.envelope > 0x6000)
            cycles *= 4;
        if (exponential && decrease)
            delta = (delta * v.envelope) >> 15;
        v.envelopeWait = cycles - 1;
        v.envelope = std::clamp<int32_t>(v.envelope + delta, 0, 0x7FFF);

        if (v.phase == 1 && v.envelope >= 0x7FFF)
        {
            v.phase = 2;
            v.envelopeWait = 0;
        }
        else if (v.phase == 2 && v.envelope <= static_cast<int32_t>(((adsr1 & 0xFu) + 1u) * 0x800u))
        {
            v.phase = 3;
            v.envelopeWait = 0;
        }
        else if (v.phase == 4 && v.envelope <= 0)
        {
            v.phase = 0;
            v.envelope = 0;
        }
    }

    void Spu2::checkIrq(uint32_t start, uint32_t count)
    {
        for (int core = 0; core < 2; ++core)
        {
            if ((reg(coreBase(core) + kAttr) & kAttrIrqEnable) == 0u)
                continue;
            const uint32_t irqa = coreAddress(core, kIrqa);
            if (((irqa - start) & (RamHalfwords - 1u)) < count)
            {
                if ((reg(kIrqInfo) & (4u << core)) == 0u)
                {
                    reg(kIrqInfo) |= static_cast<uint16_t>(4u << core);
                    m_irqPending = true;
                    if (g_traceSpu)
                        std::fprintf(stderr, "[spu2] irq core%d irqa=%05x\n", core, irqa);
                }
            }
        }
    }

    uint64_t Spu2::dmaToSpu(int core, const uint16_t *data, size_t halfwords)
    {
        Core &c = m_cores[static_cast<size_t>(core & 1)];
        reg(coreBase(core) + kStatx) |= 0x80u;
        const uint16_t admas = reg(coreBase(core) + kAdmas);
        if (admas & (1u << core))
        {
            // AutoDMA: interleaved blocks of 256 left then 256 right samples.
            for (size_t off = 0; off + 512u <= halfwords; off += 512u)
            {
                c.admaLeft.insert(c.admaLeft.end(), reinterpret_cast<const int16_t *>(data + off),
                                  reinterpret_cast<const int16_t *>(data + off + 256u));
                c.admaRight.insert(c.admaRight.end(), reinterpret_cast<const int16_t *>(data + off + 256u),
                                   reinterpret_cast<const int16_t *>(data + off + 512u));
            }
            // Keep roughly two blocks queued: complete when the earlier data has been played.
            const size_t queued = c.admaLeft.size();
            static const bool traceAdma = std::getenv("PS2X_TRACE_ADMA") != nullptr;
            static uint64_t admaCount = 0;
            if (traceAdma && (admaCount++ % 32u) == 0u)
                std::fprintf(stderr, "[adma] core%d n=%zu queued=%zu count=%llu\n", core, halfwords, queued, static_cast<unsigned long long>(admaCount));
            return queued > 256u ? static_cast<uint64_t>(queued - 256u) * CyclesPerSample : 64u;
        }

        if (g_traceSpu)
            std::fprintf(stderr, "[spu2] dma core%d tsa=%05x n=%zu first=%04x\n", core, c.tsa, halfwords, halfwords ? data[0] : 0);
        for (size_t i = 0; i < halfwords; ++i)
            m_ram[(c.tsa + i) & (RamHalfwords - 1u)] = data[i];
        stampWrite(c.tsa, static_cast<uint32_t>(halfwords));
        checkIrq(c.tsa, static_cast<uint32_t>(halfwords));
        c.tsa = (c.tsa + static_cast<uint32_t>(halfwords)) & (RamHalfwords - 1u);
        return std::max<uint64_t>(static_cast<uint64_t>(halfwords), 64u);
    }

    uint64_t Spu2::dmaFromSpu(int core, uint16_t *data, size_t halfwords)
    {
        Core &c = m_cores[static_cast<size_t>(core & 1)];
        reg(coreBase(core) + kStatx) |= 0x80u;
        for (size_t i = 0; i < halfwords; ++i)
            data[i] = m_ram[(c.tsa + i) & (RamHalfwords - 1u)];
        checkIrq(c.tsa, static_cast<uint32_t>(halfwords));
        c.tsa = (c.tsa + static_cast<uint32_t>(halfwords)) & (RamHalfwords - 1u);
        return std::max<uint64_t>(static_cast<uint64_t>(halfwords), 64u);
    }

    // A volume register: a fixed level, or (bit 15) a sweep that slides the level a step at a
    // time - linear or exponential, up or down, with the rate in bits 6-0 - like an envelope.
    int32_t Spu2::sweep(Sweep &state, uint16_t raw)
    {
        if ((raw & 0x8000u) == 0u)
        {
            state.level = static_cast<int16_t>(static_cast<uint16_t>(raw << 1));
            state.wait = 0;
            return state.level;
        }
        const bool exponential = (raw & 0x4000u) != 0u;
        const bool decrease = (raw & 0x2000u) != 0u;
        const bool negative = (raw & 0x1000u) != 0u;
        const int rate = raw & 0x7F;
        const int shift = rate >> 2;
        const int step = decrease ? -8 + (rate & 3) : 7 - (rate & 3);
        int32_t magnitude = std::min(std::abs(state.level), 0x7FFF);
        if (state.wait > 0)
        {
            --state.wait;
        }
        else
        {
            int32_t cycles = 1 << std::max(0, shift - 11);
            int32_t delta = step << std::max(0, 11 - shift);
            if (exponential && !decrease && magnitude > 0x6000)
                cycles *= 4;
            if (exponential && decrease)
                delta = (delta * magnitude) >> 15;
            state.wait = cycles - 1;
            magnitude = std::clamp<int32_t>(magnitude + delta, 0, 0x7FFF);
        }
        state.level = negative ? -magnitude : magnitude;
        return state.level;
    }

    void Spu2::mixSample(int16_t &outLeft, int16_t &outRight)
    {
        ++m_sampleCount;
        int32_t coreOut[2][2] = {};
        for (int core = 0; core < 2; ++core)
        {
            Core &c = m_cores[static_cast<size_t>(core)];
            const uint32_t base = coreBase(core);
            const uint32_t vmixl = reg(base + kVmixl) | (static_cast<uint32_t>(reg(base + kVmixl + 2u) & 0xFFu) << 16);
            const uint32_t vmixr = reg(base + kVmixr) | (static_cast<uint32_t>(reg(base + kVmixr + 2u) & 0xFFu) << 16);
            int32_t left = 0;
            int32_t right = 0;
            int32_t streamLeft = 0;
            int32_t streamRight = 0;
            for (int i = 0; i < 24; ++i)
            {
                Voice &v = c.voices[static_cast<size_t>(i)];
                if (!v.on && v.phase == 0)
                    continue;

                const int32_t *w = g_gauss.w[(v.counter >> 4) & 0xFFu];
                const int32_t sample = (w[0] * v.history[0] + w[1] * v.history[1] + w[2] * v.history[2] + w[3] * v.history[3]) >> 15;
                stepEnvelope(core, i, v);
                const int32_t enveloped = (sample * v.envelope) >> 15;
                const uint32_t vbase = base + static_cast<uint32_t>(i) * 0x10u;
                const int32_t volumeLeft = sweep(v.volumeLeft, reg(vbase + 0u));
                const int32_t volumeRight = sweep(v.volumeRight, reg(vbase + 2u));
                const int32_t voiceLeft = (enveloped * volumeLeft) >> 15;
                const int32_t voiceRight = (enveloped * volumeRight) >> 15;
                reg(vbase + 0xCu) = static_cast<uint16_t>(volumeLeft);
                reg(vbase + 0xEu) = static_cast<uint16_t>(volumeRight);
                int32_t &mixLeft = v.streaming ? streamLeft : left;
                int32_t &mixRight = v.streaming ? streamRight : right;
                if (vmixl & (1u << i))
                    mixLeft += voiceLeft;
                if (vmixr & (1u << i))
                    mixRight += voiceRight;

                if (!v.on)
                    continue;
                const uint32_t pitch = std::min<uint32_t>(reg(vbase + 4u), 0x3FFFu);
                v.counter += pitch;
                while (v.counter >= 0x1000u && v.on)
                {
                    v.counter -= 0x1000u;
                    nextSample(core, i, v);
                    v.history[0] = v.history[1];
                    v.history[1] = v.history[2];
                    v.history[2] = v.history[3];
                    v.history[3] = v.samples[v.sampleIndex];
                }
            }

            // F2 > Audio: streamed voices are the music, the other voices the effects, and the
            // sound data input (AutoDMA) the movies' sound.
            const int32_t effectsVolume = ps2x::iop::g_effectsVolumeQ15.load(std::memory_order_relaxed);
            const int32_t musicVolume = ps2x::iop::g_musicVolumeQ15.load(std::memory_order_relaxed);
            left = static_cast<int32_t>((static_cast<int64_t>(left) * effectsVolume + static_cast<int64_t>(streamLeft) * musicVolume) >> 15);
            right = static_cast<int32_t>((static_cast<int64_t>(right) * effectsVolume + static_cast<int64_t>(streamRight) * musicVolume) >> 15);
            const int32_t streamVolume = ps2x::iop::g_movieVolumeQ15.load(std::memory_order_relaxed);
            const uint32_t mbase = masterVolumeBase(core);
            if (!c.admaLeft.empty())
            {
                const int32_t input = (c.admaLeft.front() * plainVolume(reg(mbase + 0xCu))) >> 15; // BVOL: sound data input
                left += (input * streamVolume) >> 15;
                c.admaLeft.pop_front();
            }
            if (!c.admaRight.empty())
            {
                const int32_t input = (c.admaRight.front() * plainVolume(reg(mbase + 0xEu))) >> 15;
                right += (input * streamVolume) >> 15;
                c.admaRight.pop_front();
            }
            coreOut[core][0] = std::clamp<int32_t>(left, -32768, 32767);
            coreOut[core][1] = std::clamp<int32_t>(right, -32768, 32767);
        }

        // Core 0 feeds core 1's external input; core 1 drives the outputs.
        const uint32_t m0 = masterVolumeBase(0);
        const uint32_t m1 = masterVolumeBase(1);
        const int32_t core0Left = (coreOut[0][0] * sweep(m_cores[0].masterLeft, reg(m0 + 0u))) >> 15;
        const int32_t core0Right = (coreOut[0][1] * sweep(m_cores[0].masterRight, reg(m0 + 2u))) >> 15;
        int32_t left = coreOut[1][0] + ((core0Left * plainVolume(reg(m1 + 8u))) >> 15); // AVOL: external input
        int32_t right = coreOut[1][1] + ((core0Right * plainVolume(reg(m1 + 0xAu))) >> 15);
        left = (left * sweep(m_cores[1].masterLeft, reg(m1 + 0u))) >> 15;
        right = (right * sweep(m_cores[1].masterRight, reg(m1 + 2u))) >> 15;
        if (left > 32767 || left < -32768)
            ps2x::iop::g_statClipped.fetch_add(1u, std::memory_order_relaxed);
        if (right > 32767 || right < -32768)
            ps2x::iop::g_statClipped.fetch_add(1u, std::memory_order_relaxed);
        outLeft = softClip(left);
        outRight = softClip(right);
    }

    void Spu2::advanceTo(uint64_t iopCycle)
    {
        if (!m_cycleValid)
        {
            m_cycle = iopCycle;
            m_cycleValid = true;
            return;
        }
        if (iopCycle <= m_cycle)
            return;
        uint64_t samples = (iopCycle - m_cycle) / CyclesPerSample;
        if (samples == 0u)
            return;
        // A long stall (e.g. a blocking host operation) should not generate seconds of audio at once.
        if (samples > 48000u)
        {
            m_cycle += (samples - 48000u) * CyclesPerSample;
            samples = 48000u;
        }
        m_outBatch.resize(static_cast<size_t>(samples) * 2u);
        for (uint64_t i = 0; i < samples; ++i)
            mixSample(m_outBatch[static_cast<size_t>(i) * 2u], m_outBatch[static_cast<size_t>(i) * 2u + 1u]);
        m_cycle += samples * CyclesPerSample;
        pushAudioFrames(m_outBatch.data(), static_cast<size_t>(samples));
        if (g_traceSpu)
        {
            static uint64_t produced = 0;
            const uint64_t before = produced;
            produced += samples;
            if (before / 48000u != produced / 48000u)
            {
                int peak = 0;
                for (int16_t x : m_outBatch)
                    peak = std::max(peak, std::abs(static_cast<int>(x)));
                static const auto wallStart = std::chrono::steady_clock::now();
                const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
                std::fprintf(stderr, "[spu2] status t=%llus wall=%.1fs peak=%d\n", static_cast<unsigned long long>(produced / 48000u), wall, peak);
                for (int core = 0; core < 2; ++core)
                    for (int i = 0; i < 24; ++i)
                    {
                        const Voice &v = m_cores[static_cast<size_t>(core)].voices[static_cast<size_t>(i)];
                        if (v.on && v.ssa != 0x2800u)
                            std::fprintf(stderr, "[spu2]   c%d v%d ph=%d env=%d blk=%05x lsa=%05x idx=%d cur=%d fl=%x vol=%04x/%04x\n", core, i, v.phase,
                                         v.envelope, v.block, v.lsa, v.sampleIndex, v.history[3], v.flags,
                                         reg(coreBase(core) + static_cast<uint32_t>(i) * 0x10u), reg(coreBase(core) + static_cast<uint32_t>(i) * 0x10u + 2u));
                    }
            }
        }
    }

    bool Spu2::takeIrq() noexcept
    {
        const bool pending = m_irqPending;
        m_irqPending = false;
        return pending;
    }
}
