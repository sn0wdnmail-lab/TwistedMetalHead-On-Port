#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ps2x::iop::detail
{
    // SPU2 sound processor: register file, 2 MB sound RAM, 2 cores x 24 ADPCM voices with ADSR
    // envelopes, key on/off, end flags, IRQ address matching, DMA / PIO transfers and AutoDMA PCM
    // input. Output is mixed at 48 kHz into the host audio ring (see ps2x/iop/iop_audio.h).
    // Reverb, noise and pitch modulation are not emulated.
    class Spu2
    {
    public:
        static constexpr uint32_t RamHalfwords = 1u << 20; // 2 MB
        static constexpr uint32_t RegisterBase = 0x1F900000u;
        static constexpr uint32_t RegisterSize = 0x800u;
        static constexpr uint64_t CyclesPerSample = 768u; // 36.864 MHz / 48 kHz

        Spu2();
        void reset();

        [[nodiscard]] uint16_t read16(uint32_t offset) const;
        void write16(uint32_t offset, uint16_t value);

        // DMA for core 0 (IOP channel 4) / core 1 (channel 7). Returns the number of IOP cycles the
        // transfer should take before its completion interrupt.
        uint64_t dmaToSpu(int core, const uint16_t *data, size_t halfwords);
        uint64_t dmaFromSpu(int core, uint16_t *data, size_t halfwords);

        // Run the sound processor up to the given IOP cycle.
        void advanceTo(uint64_t iopCycle);
        [[nodiscard]] bool takeIrq() noexcept;

    private:
        struct Voice
        {
            uint32_t ssa = 0;
            uint32_t lsa = 0;
            uint32_t block = 0; // address of the ADPCM block being played
            bool lsaFromSoftware = false;
            bool on = false;
            uint32_t counter = 0; // 12-bit fractional sample position
            int sampleIndex = 0;
            int16_t samples[28]{};
            int32_t hist1 = 0;
            int32_t hist2 = 0;
            int16_t previous = 0;
            int16_t current = 0;
            uint8_t flags = 0;
            int phase = 0; // 0 off, 1 attack, 2 decay, 3 sustain, 4 release
            int32_t envelope = 0;
            int32_t envelopeWait = 0;
        };

        struct Core
        {
            std::array<Voice, 24> voices{};
            uint32_t endx = 0;
            uint32_t tsa = 0;
            std::deque<int16_t> admaLeft;
            std::deque<int16_t> admaRight;
        };

        [[nodiscard]] uint16_t &reg(uint32_t offset) { return m_regs[(offset & 0x7FFu) >> 1]; }
        [[nodiscard]] uint16_t reg(uint32_t offset) const { return m_regs[(offset & 0x7FFu) >> 1]; }
        [[nodiscard]] static uint32_t coreBase(int core) { return core ? 0x400u : 0u; }
        [[nodiscard]] uint32_t coreAddress(int core, uint32_t highOffset) const;
        [[nodiscard]] uint32_t voiceAddress(int core, int voice, uint32_t field) const;
        void setVoiceAddress(int core, int voice, uint32_t field, uint32_t address);

        void keyOn(int core, uint32_t mask);
        void keyOff(int core, uint32_t mask);
        void decodeBlock(int core, Voice &voice);
        void nextSample(int core, int index, Voice &voice);
        void stepEnvelope(int core, int index, Voice &voice);
        void checkIrq(uint32_t start, uint32_t count);
        void mixSample(int16_t &outLeft, int16_t &outRight);

        std::vector<uint16_t> m_ram;
        std::array<uint16_t, RegisterSize / 2> m_regs{};
        std::array<Core, 2> m_cores{};
        uint64_t m_cycle = 0;
        bool m_cycleValid = false;
        bool m_irqPending = false;
        std::vector<int16_t> m_outBatch;
    };
}
