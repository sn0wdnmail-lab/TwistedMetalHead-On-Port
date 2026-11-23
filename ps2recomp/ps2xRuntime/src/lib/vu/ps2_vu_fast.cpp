// Fast VU core.
//
// The cycle-accurate interpreter in ps2_vu1_core.cpp keeps every result in a per-cycle
// pipeline model, which costs a few hundred nanoseconds per instruction pair. Commercial
// microcode only depends on a handful of those timings, so this core executes pairs
// sequentially and models just the parts programs can observe:
//   * VF/VI read-after-write stalls (per lane), so the cycle count stays right;
//   * Q (FDIV) and P (EFU) results that only appear after their latency, so software-pipelined
//     loops that read the old Q/P keep working;
//   * MAC/status/clip flags that appear four cycles after the instruction that set them;
//   * the upper/lower same-pair rule (the lower instruction sees the old upper destination);
//   * the branch-after-integer-op rule (a branch reads the old VI value).
// Stores are visible immediately and XGKICK sends its packet at issue time.
//
// PS2X_VU_CORE=accurate switches back to the cycle-accurate model; PS2X_VU_CORE=compare runs
// both on every microprogram and reports where they disagree.

#include "runtime/ps2_vu1.h"
#include "runtime/ps2_perf.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu_fast_inl.h"

#if defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h>
#endif

#include <algorithm>
#include <chrono>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

bool ps2xVu1StatsEnabled();
void ps2xVu1StatsAccumulate(uint64_t pairs, uint64_t cycles, double hostMs);
void ps2xVu1StatsKick();

using namespace vufast;

void VU1Interpreter::fastResetPipelines()
{
    m_fastFlagHead = 0;
    m_fastFlagCount = 0;
}

void VU1Interpreter::fastCommit()
{
    while (m_fastFlagCount != 0u)
    {
        const FastFlagEntry &entry = m_fastFlags[m_fastFlagHead];
        if (entry.readyCycle > m_cycle)
            break;
        if (entry.writes & 1u)
        {
            m_state.mac = entry.mac;
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | (current << 6);
        }
        if (entry.writes & 2u)
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        if (entry.writes & 4u)
            m_state.clip = entry.clip;
        m_fastFlagHead = (m_fastFlagHead + 1u) % kFastFlagRing;
        --m_fastFlagCount;
    }

    if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {
        m_state.q = m_fdiv.value;
        const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        m_fdiv.valid = false;
    }
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry.valid = false;
        }
    }
}

void VU1Interpreter::fastFlushAll()
{
    uint64_t last = m_cycle;
    for (uint32_t index = 0; index < m_fastFlagCount; ++index)
        last = std::max(last, m_fastFlags[(m_fastFlagHead + index) % kFastFlagRing].readyCycle);
    if (m_fdiv.valid)
        last = std::max(last, m_fdiv.readyCycle);
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            last = std::max(last, entry.readyCycle);
    if (last > m_cycle)
    {
        // Commit in time order (flags first by cycle, then Q/P) so the final state matches.
        while (m_cycle < last)
        {
            ++m_cycle;
            fastCommit();
        }
    }
    else
    {
        fastCommit();
    }
}

void VU1Interpreter::fastDecode(FastPair &out, const uint8_t *vuCode, uint32_t pc) const
{
    const DecodedInstructionPair decoded = decodeInstructionPair(vuCode, pc);
    FastPair pair;
    pair.upper = decoded.upper;
    pair.lower = decoded.lower;
    pair.iBit = decoded.iBit;
    pair.eBit = decoded.eBit;
    pair.dBit = decoded.dBit;
    pair.tBit = decoded.tBit;
    pair.reservedUpper = decoded.upperUsage.reserved;
    pair.reservedLower = decoded.lowerUsage.reserved;
    std::memcpy(&pair.immediate, &decoded.lower, sizeof(float));

    // ---- timing information ----
    const InstructionUsage *usages[2] = {&decoded.upperUsage, &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        for (uint32_t index = 0; index < usage->vfReadCount && pair.readCount < 4u; ++index)
        {
            if (usage->vfRead[index].reg == 0u || usage->vfRead[index].lanes == 0u)
                continue;
            pair.readReg[pair.readCount] = usage->vfRead[index].reg;
            pair.readLanes[pair.readCount] = usage->vfRead[index].lanes;
            ++pair.readCount;
        }
    }
    pair.viRead = static_cast<uint16_t>(decoded.lowerUsage.viRead & 0xFFFEu);
    pair.upWriteReg = decoded.upperUsage.vfWrite.reg;
    pair.upWriteLanes = decoded.upperUsage.vfWrite.lanes;
    pair.upLatency = decoded.upperUsage.vfLatency != 0u ? decoded.upperUsage.vfLatency : decoded.upperUsage.latency;
    pair.loWriteReg = decoded.lowerUsage.vfWrite.reg;
    pair.loWriteLanes = decoded.lowerUsage.vfWrite.lanes;
    if (decoded.suppressedLowerVf != 0u && decoded.suppressedLowerVf == pair.loWriteReg)
        pair.loWriteReg = 0u;
    pair.loVfLatency = decoded.lowerUsage.vfLatency != 0u ? decoded.lowerUsage.vfLatency : decoded.lowerUsage.latency;
    for (uint32_t reg = 1; reg < 16u; ++reg)
    {
        if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
        {
            pair.viWriteReg = static_cast<uint8_t>(reg);
            break;
        }
    }
    pair.viLatency = decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency;
    pair.loPipeline = static_cast<uint8_t>(decoded.lowerUsage.pipeline);
    pair.loLatency = decoded.lowerUsage.latency;
    pair.waitQ = decoded.lowerUsage.waitQ;
    pair.waitP = decoded.lowerUsage.waitP;
    pair.delaysNextBranchRead = decoded.lowerUsage.delaysNextBranchRead;
    pair.shadowReg = decoded.upperVfShadowReg;

    // ---- upper ----
    const uint32_t upper = decoded.upper;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    pair.upDest = DEST(upper);
    pair.upFs = FS(upper);
    pair.upFt = FT(upper);
    pair.upFd = FD(upper);
    pair.upBc = static_cast<uint8_t>(upper & 3u);
    if (!pair.reservedUpper)
    {
        auto arith = [&](uint8_t kind, uint8_t src, bool toAcc)
        {
            pair.upKind = FU_ARITH;
            pair.upArith = kind;
            pair.upSrc = src;
            pair.upToAcc = toAcc ? 1u : 0u;
        };
        if (op <= 0x2Fu)
        {
            switch (op)
            {
            case 0x00: case 0x01: case 0x02: case 0x03: arith(FA_ADD, FSRC_BC, false); break;
            case 0x04: case 0x05: case 0x06: case 0x07: arith(FA_SUB, FSRC_BC, false); break;
            case 0x08: case 0x09: case 0x0A: case 0x0B: arith(FA_MADD, FSRC_BC, false); break;
            case 0x0C: case 0x0D: case 0x0E: case 0x0F: arith(FA_MSUB, FSRC_BC, false); break;
            case 0x10: case 0x11: case 0x12: case 0x13: arith(FA_MAX, FSRC_BC, false); break;
            case 0x14: case 0x15: case 0x16: case 0x17: arith(FA_MINI, FSRC_BC, false); break;
            case 0x18: case 0x19: case 0x1A: case 0x1B: arith(FA_MUL, FSRC_BC, false); break;
            case 0x1C: arith(FA_MUL, FSRC_Q, false); break;
            case 0x1D: arith(FA_MAX, FSRC_I, false); break;
            case 0x1E: arith(FA_MUL, FSRC_I, false); break;
            case 0x1F: arith(FA_MINI, FSRC_I, false); break;
            case 0x20: arith(FA_ADD, FSRC_Q, false); break;
            case 0x21: arith(FA_MADD, FSRC_Q, false); break;
            case 0x22: arith(FA_ADD, FSRC_I, false); break;
            case 0x23: arith(FA_MADD, FSRC_I, false); break;
            case 0x24: arith(FA_SUB, FSRC_Q, false); break;
            case 0x25: arith(FA_MSUB, FSRC_Q, false); break;
            case 0x26: arith(FA_SUB, FSRC_I, false); break;
            case 0x27: arith(FA_MSUB, FSRC_I, false); break;
            case 0x28: arith(FA_ADD, FSRC_VT, false); break;
            case 0x29: arith(FA_MADD, FSRC_VT, false); break;
            case 0x2A: arith(FA_MUL, FSRC_VT, false); break;
            case 0x2B: arith(FA_MAX, FSRC_VT, false); break;
            case 0x2C: arith(FA_SUB, FSRC_VT, false); break;
            case 0x2D: arith(FA_MSUB, FSRC_VT, false); break;
            case 0x2E: arith(FA_OPMSUB, FSRC_VT, false); break;
            case 0x2F: arith(FA_MINI, FSRC_VT, false); break;
            default: pair.reservedUpper = true; break;
            }
        }
        else if (op >= 0x3Cu)
        {
            const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
            pair.upBc = static_cast<uint8_t>(special & 3u);
            switch (special)
            {
            case 0x00: case 0x01: case 0x02: case 0x03: arith(FA_ADD, FSRC_BC, true); break;
            case 0x04: case 0x05: case 0x06: case 0x07: arith(FA_SUB, FSRC_BC, true); break;
            case 0x08: case 0x09: case 0x0A: case 0x0B: arith(FA_MADD, FSRC_BC, true); break;
            case 0x0C: case 0x0D: case 0x0E: case 0x0F: arith(FA_MSUB, FSRC_BC, true); break;
            case 0x10: case 0x11: case 0x12: case 0x13:
                pair.upKind = FU_ITOF;
                pair.upScale = static_cast<uint8_t>(special & 3u);
                break;
            case 0x14: case 0x15: case 0x16: case 0x17:
                pair.upKind = FU_FTOI;
                pair.upScale = static_cast<uint8_t>(special & 3u);
                break;
            case 0x18: case 0x19: case 0x1A: case 0x1B: arith(FA_MUL, FSRC_BC, true); break;
            case 0x1C: arith(FA_MUL, FSRC_Q, true); break;
            case 0x1D: pair.upKind = FU_ABS; break;
            case 0x1E: arith(FA_MUL, FSRC_I, true); break;
            case 0x1F: pair.upKind = FU_CLIP; break;
            case 0x20: arith(FA_ADD, FSRC_Q, true); break;
            case 0x21: arith(FA_MADD, FSRC_Q, true); break;
            case 0x22: arith(FA_ADD, FSRC_I, true); break;
            case 0x23: arith(FA_MADD, FSRC_I, true); break;
            case 0x24: arith(FA_SUB, FSRC_Q, true); break;
            case 0x25: arith(FA_MSUB, FSRC_Q, true); break;
            case 0x26: arith(FA_SUB, FSRC_I, true); break;
            case 0x27: arith(FA_MSUB, FSRC_I, true); break;
            case 0x28: arith(FA_ADD, FSRC_VT, true); break;
            case 0x29: arith(FA_MADD, FSRC_VT, true); break;
            case 0x2A: arith(FA_MUL, FSRC_VT, true); break;
            case 0x2C: arith(FA_SUB, FSRC_VT, true); break;
            case 0x2D: arith(FA_MSUB, FSRC_VT, true); break;
            case 0x2E: arith(FA_OPMULA, FSRC_VT, true); break;
            case 0x2F: case 0x30: pair.upKind = FU_NOP; break;
            default: pair.reservedUpper = true; break;
            }
        }
        else
        {
            pair.reservedUpper = true;
        }
    }

    // ---- lower ----
    const uint32_t lower = decoded.lower;
    pair.loKind = FL_NOP;
    if (!pair.iBit && !pair.reservedLower && lower != 0u && lower != 0x8000033Cu)
    {
        const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
        pair.loDest = DEST(lower);
        pair.loFs = FS(lower);
        pair.loFt = FT(lower);
        pair.loFd = FD(lower);
        pair.loFsf = static_cast<uint8_t>((lower >> 21) & 3u);
        pair.loFtf = static_cast<uint8_t>((lower >> 23) & 3u);
        pair.loImm = IMM11(lower);
        pair.loImm24 = lower & 0xFFFFFFu;
        switch (opHi)
        {
        case 0x00: pair.loKind = FL_LQ; break;
        case 0x01: pair.loKind = FL_SQ; break;
        case 0x04: pair.loKind = FL_ILW; break;
        case 0x05: pair.loKind = FL_ISW; break;
        case 0x08:
            pair.loKind = FL_IADDIU;
            pair.loImm = static_cast<int16_t>((lower & 0x7FFu) | ((lower >> 10) & 0x7800u));
            break;
        case 0x09:
            pair.loKind = FL_ISUBIU;
            pair.loImm = static_cast<int16_t>((lower & 0x7FFu) | ((lower >> 10) & 0x7800u));
            break;
        case 0x10: pair.loKind = FL_FCEQ; break;
        case 0x11: pair.loKind = FL_FCSET; break;
        case 0x12: pair.loKind = FL_FCAND; break;
        case 0x13: pair.loKind = FL_FCOR; break;
        case 0x14: case 0x15: case 0x16: case 0x17:
            pair.loKind = opHi == 0x14 ? FL_FSEQ : opHi == 0x15 ? FL_FSSET : opHi == 0x16 ? FL_FSAND : FL_FSOR;
            pair.loImm24 = static_cast<uint32_t>((((lower >> 21) & 0x1u) << 11) | (lower & 0x7FFu));
            break;
        case 0x18: pair.loKind = FL_FMEQ; break;
        case 0x1A: pair.loKind = FL_FMAND; break;
        case 0x1B: pair.loKind = FL_FMOR; break;
        case 0x1C: pair.loKind = FL_FCGET; break;
        case 0x20: pair.loKind = FL_B; break;
        case 0x21: pair.loKind = FL_BAL; break;
        case 0x24: pair.loKind = FL_JR; break;
        case 0x25: pair.loKind = FL_JALR; break;
        case 0x28: pair.loKind = FL_IBEQ; break;
        case 0x29: pair.loKind = FL_IBNE; break;
        case 0x2C: pair.loKind = FL_IBLTZ; break;
        case 0x2D: pair.loKind = FL_IBGTZ; break;
        case 0x2E: pair.loKind = FL_IBLEZ; break;
        case 0x2F: pair.loKind = FL_IBGEZ; break;
        case 0x40:
        {
            const uint8_t funct = static_cast<uint8_t>(lower & 0x3Fu);
            switch (funct)
            {
            case 0x30: pair.loKind = FL_IADD; break;
            case 0x31: pair.loKind = FL_ISUB; break;
            case 0x32:
                pair.loKind = FL_IADDI;
                pair.loImm = static_cast<int16_t>(static_cast<int32_t>(((lower >> 6) & 0x1Fu) << 27) >> 27);
                break;
            case 0x34: pair.loKind = FL_IAND; break;
            case 0x35: pair.loKind = FL_IOR; break;
            case 0x3C: case 0x3D: case 0x3E: case 0x3F:
            {
                const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
                switch (special)
                {
                case 0x30: pair.loKind = FL_MOVE; break;
                case 0x31: pair.loKind = FL_MR32; break;
                case 0x34: pair.loKind = FL_LQI; break;
                case 0x35: pair.loKind = FL_SQI; break;
                case 0x36: pair.loKind = FL_LQD; break;
                case 0x37: pair.loKind = FL_SQD; break;
                case 0x38: pair.loKind = FL_DIV; break;
                case 0x39: pair.loKind = FL_SQRT; break;
                case 0x3A: pair.loKind = FL_RSQRT; break;
                case 0x3B: pair.loKind = FL_WAITQ; break;
                case 0x3C: pair.loKind = FL_MTIR; break;
                case 0x3D: pair.loKind = FL_MFIR; break;
                case 0x3E: pair.loKind = FL_ILWR; break;
                case 0x3F: pair.loKind = FL_ISWR; break;
                case 0x40: pair.loKind = FL_RNEXT; break;
                case 0x41: pair.loKind = FL_RGET; break;
                case 0x42: pair.loKind = FL_RINIT; break;
                case 0x43: pair.loKind = FL_RXOR; break;
                case 0x64: pair.loKind = FL_MFP; break;
                case 0x68: pair.loKind = FL_XTOP; break;
                case 0x69: pair.loKind = FL_XITOP; break;
                case 0x6C: pair.loKind = FL_XGKICK; break;
                case 0x70: pair.loKind = FL_ESADD; break;
                case 0x71: pair.loKind = FL_ERSADD; break;
                case 0x72: pair.loKind = FL_ELENG; break;
                case 0x73: pair.loKind = FL_ERLENG; break;
                case 0x74: pair.loKind = FL_EATANXY; break;
                case 0x75: pair.loKind = FL_EATANXZ; break;
                case 0x76: pair.loKind = FL_ESUM; break;
                case 0x77: pair.loKind = FL_ERSQRT; break;
                case 0x78: pair.loKind = FL_ESQRT; break;
                case 0x79: pair.loKind = FL_ESIN; break;
                case 0x7A: pair.loKind = FL_ERCPR; break;
                case 0x7B: pair.loKind = FL_WAITP; break;
                case 0x7C: pair.loKind = FL_EATAN; break;
                case 0x7D: pair.loKind = FL_EEXP; break;
                default: pair.reservedLower = true; break;
                }
                break;
            }
            default:
                pair.reservedLower = true;
                break;
            }
            break;
        }
        default:
            pair.reservedLower = true;
            break;
        }
    }

    // Does anything in this pair write VF0? (VF0 is reset to 0,0,0,1 after such a pair.)
    bool upperWrites0 = false;
    switch (pair.upKind)
    {
    case FU_ARITH: upperWrites0 = !pair.upToAcc && pair.upFd == 0u && pair.upDest != 0u; break;
    case FU_ITOF:
    case FU_FTOI:
    case FU_ABS: upperWrites0 = pair.upFt == 0u && pair.upDest != 0u; break;
    default: break;
    }
    bool lowerWrites0 = false;
    switch (pair.loKind)
    {
    case FL_LQ: case FL_LQI: case FL_LQD: case FL_MOVE: case FL_MR32: case FL_MFIR:
    case FL_RNEXT: case FL_RGET: case FL_MFP:
        lowerWrites0 = pair.loFt == 0u && pair.loDest != 0u;
        break;
    default: break;
    }
    pair.writesVf0 = upperWrites0 || lowerWrites0 || pair.reservedUpper || pair.reservedLower;
    out = pair;
}

const VU1Interpreter::FastPair &VU1Interpreter::fastPairFor(const uint8_t *vuCode, uint32_t codeSize,
                                                            PS2Memory *memory, uint32_t pc)
{
    if (!m_fastPairs)
        m_fastPairs = std::make_unique<FastPair[]>(kMaxDecodedPairs);

    const uint32_t index = (pc / 8u) % kMaxDecodedPairs;
    FastPair &entry = m_fastPairs[index];
    // Decoding depends only on the two instruction words, so comparing them is enough to
    // notice MPG uploads (no need to trust the code-generation counter).
    uint32_t lower = 0u;
    uint32_t upper = 0u;
    std::memcpy(&lower, vuCode + pc, 4);
    std::memcpy(&upper, vuCode + pc + 4, 4);
    if (entry.generation == 0u || entry.lower != lower || entry.upper != upper || (pc & 7u) != 0u)
    {
        fastDecode(entry, vuCode, pc);
        entry.generation = 1u;
    }
    (void)memory;
    (void)codeSize;
    return entry;
}

void VU1Interpreter::fastXgkick(uint32_t qwordAddress, const uint8_t *vuData, uint32_t dataSize)
{
    if (m_unit != Unit::VU1 || !vuData || dataSize < 16u)
        return;
    if (ps2xVu1StatsEnabled())
        ps2xVu1StatsKick();

    uint8_t *packet = m_xgkick.packet.data();
    constexpr uint32_t kBuffer = XgkickPipeline::kBufferSize;
    uint32_t source = (qwordAddress * 16u) % dataSize;
    uint32_t total = 0u;
    auto copyBytes = [&](uint32_t bytes) -> bool
    {
        if (bytes > kBuffer - total)
            return false;
        while (bytes != 0u)
        {
            const uint32_t chunk = std::min(bytes, dataSize - source);
            std::memcpy(packet + total, vuData + source, chunk);
            total += chunk;
            bytes -= chunk;
            source = (source + chunk) % dataSize;
        }
        return true;
    };

    for (;;)
    {
        const uint32_t tagOffset = total;
        if (!copyBytes(16u))
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            return;
        }
        uint64_t tagLo = 0;
        std::memcpy(&tagLo, packet + tagOffset, sizeof(tagLo));
        const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
        const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
        if (nreg == 0u)
            nreg = 16u;
        uint64_t bodyBytes = 0u;
        if (format == 0u)
            bodyBytes = static_cast<uint64_t>(nloop) * nreg * 16u;
        else if (format == 1u)
            bodyBytes = ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
        else
            bodyBytes = static_cast<uint64_t>(nloop) * 16u;
        if (bodyBytes > kBuffer || !copyBytes(static_cast<uint32_t>(bodyBytes)))
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            return;
        }
        if (((tagLo >> 15) & 1u) != 0u)
            break;
    }
    emitPath1Packet(packet, total);
}

void VU1Interpreter::runFast(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    VuFpEnvironment fpEnvironment;
    const bool statsOn = ps2xVu1StatsEnabled() && m_unit == Unit::VU1;
    const auto statsStart = statsOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const uint64_t startCycle = m_cycle;
    uint64_t pairs = 0;

    // Decide whether MAC/status flags must be computed for this code image (VU1 only: the EE can
    // read VU0's flags after a VU0 microprogram).
    if (m_unit != Unit::VU1 || memory == nullptr || vuCode != memory->getVU1Code())
    {
        m_fastNeedMacStatus = true;
    }
    else
    {
        const uint64_t generation = memory->getVU1CodeGeneration();
        if (generation != m_fastFlagScanGeneration)
        {
            m_fastFlagScanGeneration = generation;
            bool needed = false;
            for (uint32_t pc = 0; pc + 8u <= codeSize && !needed; pc += 8u)
            {
                uint32_t lower = 0u;
                uint32_t upper = 0u;
                std::memcpy(&lower, vuCode + pc, 4);
                std::memcpy(&upper, vuCode + pc + 4, 4);
                if (upper & 0x80000000u) // I bit: the lower word is an immediate
                    continue;
                const uint32_t opHi = (lower >> 25) & 0x7Fu;
                needed = opHi == 0x14u || opHi == 0x16u || opHi == 0x17u || // FSEQ/FSAND/FSOR
                         opHi == 0x18u || opHi == 0x1Au || opHi == 0x1Bu;   // FMEQ/FMAND/FMOR
            }
            m_fastNeedMacStatus = needed;
        }
    }

    // Ahead-of-time compiled version of this code image (VU1 only), resolved once per upload.
    static uint64_t s_aotImageHash = 0; // image of m_aotFn, for the compiled-code miss report
    Vu1Aot::Fn aot = nullptr;
    if (m_unit == Unit::VU1 && memory != nullptr && vuCode == memory->getVU1Code() && Vu1Aot::enabled())
    {
        const uint64_t generation = memory->getVU1CodeGeneration();
        if (generation != m_aotGeneration)
        {
            m_aotGeneration = generation;
            m_aotFn = reinterpret_cast<void *>(Vu1Aot::lookup(vuCode, codeSize));
            s_aotImageHash = m_aotFn ? vufast::codeHash(vuCode, codeSize) : 0;
        }
        aot = reinterpret_cast<Vu1Aot::Fn>(m_aotFn);
    }
    // Interpreted stretches inside a compiled image (no compiled entry at that pc).
    const bool trackMisses = aot != nullptr && ps2perf::enabled();
    uint32_t missPc = 0;
    uint32_t missPairs = 0;
    const uint64_t pairsBeforeRun = pairs;
    uint64_t interpretedPairs = 0;
    auto closeMiss = [&]()
    {
        if (missPairs != 0u)
        {
            ps2perf::vu1CompiledMiss(s_aotImageHash, missPc, missPairs);
            interpretedPairs += missPairs;
            missPairs = 0;
        }
    };

    const uint64_t budgetEnd = m_cycle + maxCycles;
    bool programEnded = false;
    while (m_cycle < budgetEnd)
    {
        if (aot)
        {
            const int result = aot(*this, vuData, dataSize, budgetEnd, pairs);
            if (trackMisses && result != Vu1Aot::kUnhandled)
                closeMiss();
            if (result == Vu1Aot::kEnded)
            {
                programEnded = true;
                break;
            }
            if (result == Vu1Aot::kBudget || m_cycle >= budgetEnd)
                break;
            // kUnhandled: interpret the pair at m_state.pc, then try compiled code again.
            if (trackMisses)
            {
                if (missPairs == 0u)
                    missPc = m_state.pc;
                ++missPairs;
            }
        }
        if (m_state.pc + 8u > codeSize)
            break;
        const FastPair &pair = fastPairFor(vuCode, codeSize, memory, m_state.pc);
        if (pair.reservedUpper || pair.reservedLower)
        {
            reportReservedInstruction(pair.reservedUpper, pair.reservedUpper ? pair.upper : pair.lower);
            break;
        }
        if (!Vu1Aot::pairCore(*this, pair, vuData, dataSize, budgetEnd, pairs))
            break;
        if (Vu1Aot::postPair(*this, pair, codeSize))
        {
            programEnded = true;
            break;
        }
    }

    if (programEnded)
    {
        fastFlushAll();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    m_lastRunEnded = programEnded;
    m_state.cycles = m_cycle;
    if (trackMisses)
    {
        closeMiss();
        ps2perf::count(ps2perf::VuPairsInterpreted, interpretedPairs);
    }
    else if (aot == nullptr && m_unit == Unit::VU1 && ps2perf::enabled())
        ps2perf::count(ps2perf::VuPairsInterpreted, pairs - pairsBeforeRun);
    if (ps2perf::enabled())
        ps2perf::count(ps2perf::VuPairs, pairs);
    if (statsOn)
        ps2xVu1StatsAccumulate(pairs, m_cycle - startCycle,
                               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - statsStart).count());
}

// ---------------------------------------------------------------------------------------------
// Compare mode: run the fast core on a copy, then the accurate core for real, and report
// differences in VU memory, registers and PATH1 output.
// ---------------------------------------------------------------------------------------------
namespace
{
    struct CompareTotals
    {
        uint64_t runs = 0;
        uint64_t mismatchRuns = 0;
        uint64_t reports = 0;
    };
    CompareTotals g_compare;

    bool wordsClose(uint32_t a, uint32_t b)
    {
        if (a == b)
            return true;
        // Treat as floats: allow a few ulps (the accurate core rounds FMAC results exactly).
        const bool aFloat = (a & 0x7F800000u) != 0u;
        const bool bFloat = (b & 0x7F800000u) != 0u;
        if (aFloat && bFloat && ((a ^ b) & 0x80000000u) == 0u)
        {
            const int64_t diff = static_cast<int64_t>(a & 0x7FFFFFFFu) - static_cast<int64_t>(b & 0x7FFFFFFFu);
            return diff >= -8 && diff <= 8;
        }
        // Small integers (FTOI4 coordinates, colours): allow off-by-one from rounding.
        const int64_t diff = static_cast<int64_t>(static_cast<int32_t>(a)) - static_cast<int64_t>(static_cast<int32_t>(b));
        return diff >= -1 && diff <= 1;
    }
}

void VU1Interpreter::runCompare(uint8_t *vuCode, uint32_t codeSize,
                                uint8_t *vuData, uint32_t dataSize,
                                GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    if (!m_compareShadow)
        m_compareShadow = std::make_unique<VU1Interpreter>(m_unit);
    VU1Interpreter &shadow = *m_compareShadow;
    shadow.m_state = m_state;
    shadow.m_cycle = m_cycle;
    shadow.m_workingClip = m_workingClip;
    if (!pipelinesPending())
    {
        shadow.fastResetPipelines();
        shadow.m_fdiv = {};
        shadow.m_efu = {};
        shadow.m_vfReady = {};
        shadow.m_viReady = {};
        shadow.m_efuResourceReady = 0;
        shadow.m_viBranchBackupValid = false;
    }
    const uint32_t startPc = m_state.pc;
    m_compareData.assign(vuData, vuData + dataSize);
    std::vector<uint8_t> fastGif;
    std::vector<uint8_t> accurateGif;
    shadow.m_gifCapture = &fastGif;
    shadow.m_gifSubmit = false;
    shadow.runFast(vuCode, codeSize, m_compareData.data(), dataSize, gs, memory, maxCycles);

    m_gifCapture = &accurateGif;
    runAccurate(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    m_gifCapture = nullptr;

    ++g_compare.runs;
    uint32_t memDiffs = 0, firstMem = 0xFFFFFFFFu;
    for (uint32_t offset = 0; offset + 4u <= dataSize; offset += 4u)
    {
        uint32_t a, b;
        std::memcpy(&a, vuData + offset, 4);
        std::memcpy(&b, m_compareData.data() + offset, 4);
        if (!wordsClose(a, b))
        {
            if (memDiffs++ == 0u)
                firstMem = offset;
        }
    }
    uint32_t regDiffs = 0;
    int firstReg = -1;
    for (int reg = 1; reg < 32; ++reg)
        for (int c = 0; c < 4; ++c)
        {
            uint32_t a, b;
            std::memcpy(&a, &m_state.vf[reg][c], 4);
            std::memcpy(&b, &shadow.m_state.vf[reg][c], 4);
            if (!wordsClose(a, b) && regDiffs++ == 0u)
                firstReg = reg;
        }
    for (int reg = 1; reg < 16; ++reg)
        if (m_state.vi[reg] != shadow.m_state.vi[reg] && regDiffs++ == 0u)
            firstReg = 100 + reg;
    uint32_t gifDiffs = 0, firstGif = 0xFFFFFFFFu;
    const size_t gifCommon = std::min(fastGif.size(), accurateGif.size());
    for (size_t offset = 0; offset + 4u <= gifCommon; offset += 4u)
    {
        uint32_t a, b;
        std::memcpy(&a, accurateGif.data() + offset, 4);
        std::memcpy(&b, fastGif.data() + offset, 4);
        if (!wordsClose(a, b) && gifDiffs++ == 0u)
            firstGif = static_cast<uint32_t>(offset);
    }
    const bool pcDiff = m_state.pc != shadow.m_state.pc || m_lastRunEnded != shadow.m_lastRunEnded;
    const bool mismatch = memDiffs || regDiffs || gifDiffs || fastGif.size() != accurateGif.size() || pcDiff;
    if (mismatch)
        ++g_compare.mismatchRuns;
    if (mismatch && g_compare.reports < 60u)
    {
        ++g_compare.reports;
        std::fprintf(stderr, "[vu-compare] %s start=%04x end=%04x/%04x ended=%d/%d mem=%u@%x regs=%u(first %d) gif=%zu/%zu diffs=%u@%x\n",
                     m_unit == Unit::VU1 ? "vu1" : "vu0", startPc, m_state.pc, shadow.m_state.pc,
                     m_lastRunEnded, shadow.m_lastRunEnded, memDiffs, firstMem, regDiffs, firstReg,
                     accurateGif.size(), fastGif.size(), gifDiffs, firstGif);
        if (firstReg > 0 && firstReg < 32)
            std::fprintf(stderr, "[vu-compare]   vf%d acc=(%g %g %g %g) fast=(%g %g %g %g)\n", firstReg,
                         m_state.vf[firstReg][0], m_state.vf[firstReg][1], m_state.vf[firstReg][2], m_state.vf[firstReg][3],
                         shadow.m_state.vf[firstReg][0], shadow.m_state.vf[firstReg][1], shadow.m_state.vf[firstReg][2], shadow.m_state.vf[firstReg][3]);
        if (firstGif != 0xFFFFFFFFu)
        {
            const uint32_t base = firstGif & ~15u;
            uint32_t a[4] = {}, b[4] = {};
            std::memcpy(a, accurateGif.data() + base, std::min<size_t>(16, accurateGif.size() - base));
            std::memcpy(b, fastGif.data() + base, std::min<size_t>(16, fastGif.size() - base));
            std::fprintf(stderr, "[vu-compare]   gif+%x acc=%08x %08x %08x %08x fast=%08x %08x %08x %08x\n", base,
                         a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        }
    }
    if ((g_compare.runs % 20000u) == 0u)
        std::fprintf(stderr, "[vu-compare] %llu runs, %llu with differences\n",
                     static_cast<unsigned long long>(g_compare.runs), static_cast<unsigned long long>(g_compare.mismatchRuns));
}
