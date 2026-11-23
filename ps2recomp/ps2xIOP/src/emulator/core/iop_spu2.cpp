#include <chrono>
#include "iop_spu2.h"

#include "ps2x/iop/iop_audio.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ps2x::iop
{
    namespace
    {
        // Host audio ring: interleaved stereo int16 frames produced by the SPU2, pulled by the host
        // audio callback. Bounded so a slow consumer drops the oldest audio instead of growing.
        constexpr size_t kRingFrames = 48000u / 4u;
        std::mutex g_ringMutex;
        std::deque<int16_t> g_ring;
        bool g_ringPrimed = false;
    }

    void pushAudioFrames(const int16_t *interleaved, size_t frames)
    {
        // PS2X_AUDIO_DUMP=<path>: also append the raw 48 kHz s16le stereo output to a file.
        static FILE *dump = []() -> FILE *
        {
            const char *env = std::getenv("PS2X_AUDIO_DUMP");
            return (env && *env) ? std::fopen(env, "wb") : nullptr;
        }();
        if (dump)
        {
            std::fwrite(interleaved, sizeof(int16_t) * 2u, frames, dump);
            std::fflush(dump);
        }
        std::lock_guard<std::mutex> lock(g_ringMutex);
        g_ring.insert(g_ring.end(), interleaved, interleaved + frames * 2u);
        if (g_ring.size() > kRingFrames * 2u)
            g_ring.erase(g_ring.begin(), g_ring.begin() + static_cast<std::ptrdiff_t>(g_ring.size() - kRingFrames * 2u));
    }

    size_t pullAudioFrames(int16_t *interleaved, size_t frames)
    {
        std::lock_guard<std::mutex> lock(g_ringMutex);
        // Wait for a little audio to build up before starting, so the first callbacks do not starve.
        if (!g_ringPrimed)
        {
            if (g_ring.size() < 2400u * 2u)
            {
                std::fill(interleaved, interleaved + frames * 2u, int16_t{0});
                return 0u;
            }
            g_ringPrimed = true;
        }
        const size_t available = std::min(frames, g_ring.size() / 2u);
        std::copy_n(g_ring.begin(), available * 2u, interleaved);
        g_ring.erase(g_ring.begin(), g_ring.begin() + static_cast<std::ptrdiff_t>(available * 2u));
        std::fill(interleaved + available * 2u, interleaved + frames * 2u, int16_t{0});
        if (available < frames)
            g_ringPrimed = false;
        return available;
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
                return 0x7FFF; // sweep mode: not modelled, play at full volume
            return static_cast<int16_t>(static_cast<uint16_t>(raw << 1));
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
        : m_ram(RamHalfwords, 0u)
    {
        reset();
    }

    void Spu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
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
            v.previous = v.current = 0;
            v.on = true;
            v.phase = 1;
            v.envelope = 0;
            v.envelopeWait = 0;
            c.endx &= ~(1u << i);
            decodeBlock(core, v);
            v.current = v.samples[0];
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

    void Spu2::decodeBlock(int /*core*/, Voice &v)
    {
        const uint32_t base = v.block & (RamHalfwords - 1u);
        checkIrq(base, 8u);
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

    void Spu2::mixSample(int16_t &outLeft, int16_t &outRight)
    {
        int32_t coreOut[2][2] = {};
        for (int core = 0; core < 2; ++core)
        {
            Core &c = m_cores[static_cast<size_t>(core)];
            const uint32_t base = coreBase(core);
            const uint32_t vmixl = reg(base + kVmixl) | (static_cast<uint32_t>(reg(base + kVmixl + 2u) & 0xFFu) << 16);
            const uint32_t vmixr = reg(base + kVmixr) | (static_cast<uint32_t>(reg(base + kVmixr + 2u) & 0xFFu) << 16);
            int32_t left = 0;
            int32_t right = 0;
            for (int i = 0; i < 24; ++i)
            {
                Voice &v = c.voices[static_cast<size_t>(i)];
                if (!v.on && v.phase == 0)
                    continue;

                const int32_t frac = static_cast<int32_t>(v.counter & 0xFFFu);
                const int32_t sample = v.previous + (((v.current - v.previous) * frac) >> 12);
                stepEnvelope(core, i, v);
                const int32_t enveloped = (sample * v.envelope) >> 15;
                const uint32_t vbase = base + static_cast<uint32_t>(i) * 0x10u;
                const int32_t voiceLeft = (enveloped * volumeOf(reg(vbase + 0u))) >> 15;
                const int32_t voiceRight = (enveloped * volumeOf(reg(vbase + 2u))) >> 15;
                reg(vbase + 0xCu) = static_cast<uint16_t>(volumeOf(reg(vbase + 0u)));
                reg(vbase + 0xEu) = static_cast<uint16_t>(volumeOf(reg(vbase + 2u)));
                if (vmixl & (1u << i))
                    left += voiceLeft;
                if (vmixr & (1u << i))
                    right += voiceRight;

                if (!v.on)
                    continue;
                const uint32_t pitch = std::min<uint32_t>(reg(vbase + 4u), 0x3FFFu);
                v.counter += pitch;
                while (v.counter >= 0x1000u && v.on)
                {
                    v.counter -= 0x1000u;
                    v.previous = v.current;
                    nextSample(core, i, v);
                    v.current = v.samples[v.sampleIndex];
                }
            }

            const uint32_t mbase = masterVolumeBase(core);
            if (!c.admaLeft.empty())
            {
                left += (c.admaLeft.front() * plainVolume(reg(mbase + 0xCu))) >> 15; // BVOL: sound data input
                c.admaLeft.pop_front();
            }
            if (!c.admaRight.empty())
            {
                right += (c.admaRight.front() * plainVolume(reg(mbase + 0xEu))) >> 15;
                c.admaRight.pop_front();
            }
            coreOut[core][0] = left;
            coreOut[core][1] = right;
        }

        // Core 0 feeds core 1's external input; core 1 drives the outputs.
        const uint32_t m0 = masterVolumeBase(0);
        const uint32_t m1 = masterVolumeBase(1);
        const int32_t core0Left = (coreOut[0][0] * volumeOf(reg(m0 + 0u))) >> 15;
        const int32_t core0Right = (coreOut[0][1] * volumeOf(reg(m0 + 2u))) >> 15;
        int32_t left = coreOut[1][0] + ((core0Left * plainVolume(reg(m1 + 8u))) >> 15); // AVOL: external input
        int32_t right = coreOut[1][1] + ((core0Right * plainVolume(reg(m1 + 0xAu))) >> 15);
        left = (left * volumeOf(reg(m1 + 0u))) >> 15;
        right = (right * volumeOf(reg(m1 + 2u))) >> 15;
        outLeft = clamp16(left);
        outRight = clamp16(right);
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
                                         v.envelope, v.block, v.lsa, v.sampleIndex, v.current, v.flags,
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
