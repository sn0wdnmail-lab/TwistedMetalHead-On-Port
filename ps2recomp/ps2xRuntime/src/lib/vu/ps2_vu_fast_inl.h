#pragma once

// Fast VU core internals shared by the interpreter (ps2_vu_fast.cpp) and the ahead-of-time
// compiled VU1 microprograms (vu1_aot_programs.cpp). The per-pair semantics live in one place
// (Vu1Aot::pairCore / postPair), so a compiled program behaves exactly like the interpreter;
// compiled code passes each decoded pair as a compile-time constant and the compiler folds the
// decode switches away.

#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"

#if defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h>
#endif

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>

#if defined(__GNUC__) || defined(__clang__)
#define VU_FAST_INLINE inline __attribute__((always_inline))
#else
#define VU_FAST_INLINE __forceinline
#endif

inline bool g_vu1TracePairs = false; // debugging aid (vu1_replay --trace)

namespace vufast
{
    enum FastUpperKind : uint8_t
    {
        FU_NOP = 0,
        FU_ARITH,
        FU_ITOF,
        FU_FTOI,
        FU_ABS,
        FU_CLIP,
    };

    enum FastArith : uint8_t
    {
        FA_ADD,
        FA_SUB,
        FA_MADD,
        FA_MSUB,
        FA_MUL,
        FA_MAX,
        FA_MINI,
        FA_OPMULA,
        FA_OPMSUB,
    };

    enum FastSrc : uint8_t
    {
        FSRC_BC,
        FSRC_VT,
        FSRC_Q,
        FSRC_I,
    };

    enum FastLowerKind : uint8_t
    {
        FL_NOP = 0,
        FL_LQ, FL_SQ, FL_ILW, FL_ISW, FL_IADDIU, FL_ISUBIU,
        FL_FCEQ, FL_FCSET, FL_FCAND, FL_FCOR, FL_FSEQ, FL_FSSET, FL_FSAND, FL_FSOR,
        FL_FMEQ, FL_FMAND, FL_FMOR, FL_FCGET,
        FL_B, FL_BAL, FL_JR, FL_JALR, FL_IBEQ, FL_IBNE, FL_IBLTZ, FL_IBGTZ, FL_IBLEZ, FL_IBGEZ,
        FL_IADD, FL_ISUB, FL_IADDI, FL_IAND, FL_IOR,
        FL_MOVE, FL_MR32, FL_LQI, FL_SQI, FL_LQD, FL_SQD,
        FL_DIV, FL_SQRT, FL_RSQRT, FL_WAITQ,
        FL_MTIR, FL_MFIR, FL_ILWR, FL_ISWR,
        FL_RNEXT, FL_RGET, FL_RINIT, FL_RXOR,
        FL_MFP, FL_XTOP, FL_XITOP, FL_XGKICK,
        FL_ESADD, FL_ERSADD, FL_ELENG, FL_ERLENG, FL_EATANXY, FL_EATANXZ, FL_ESUM,
        FL_ERSQRT, FL_ESQRT, FL_ESIN, FL_ERCPR, FL_WAITP, FL_EATAN, FL_EEXP,
    };

    // VU lane order is x = dest bit 3 ... w = dest bit 0; SSE movemask order is x = bit 0.
    constexpr uint8_t kRev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

    struct LaneMasks
    {
        __m128 mask[16];
        LaneMasks()
        {
            for (int dest = 0; dest < 16; ++dest)
            {
                alignas(16) uint32_t lanes[4] = {
                    (dest & 8) ? 0xFFFFFFFFu : 0u,
                    (dest & 4) ? 0xFFFFFFFFu : 0u,
                    (dest & 2) ? 0xFFFFFFFFu : 0u,
                    (dest & 1) ? 0xFFFFFFFFu : 0u};
                mask[dest] = _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(lanes)));
            }
        }
    };
    inline const LaneMasks g_laneMasks{};

    VU_FAST_INLINE void storeMasked(float *dst, __m128 value, uint8_t dest)
    {
        if (dest == 0xFu)
        {
            _mm_storeu_ps(dst, value);
            return;
        }
        const __m128 mask = g_laneMasks.mask[dest];
        const __m128 old = _mm_loadu_ps(dst);
        _mm_storeu_ps(dst, _mm_or_ps(_mm_and_ps(mask, value), _mm_andnot_ps(mask, old)));
    }

    // PS2 operand rules: denormals read as signed zero, Inf/NaN exponents read as +-FLT_MAX.
    VU_FAST_INLINE __m128 vuNorm(__m128 value)
    {
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i signMask = _mm_set1_epi32(static_cast<int>(0x80000000u));
        const __m128i maxMag = _mm_set1_epi32(0x7F7FFFFF);
        __m128i bits = _mm_castps_si128(value);
        const __m128i exponent = _mm_and_si128(bits, expMask);
        const __m128i sign = _mm_and_si128(bits, signMask);
        const __m128i isMax = _mm_cmpeq_epi32(exponent, expMask);
        const __m128i isZero = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
        bits = _mm_or_si128(_mm_andnot_si128(isMax, bits), _mm_and_si128(isMax, _mm_or_si128(sign, maxMag)));
        bits = _mm_or_si128(_mm_andnot_si128(isZero, bits), _mm_and_si128(isZero, sign));
        return _mm_castsi128_ps(bits);
    }

    VU_FAST_INLINE float normScalar(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        const uint32_t exponent = bits & 0x7F800000u;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0x7F800000u)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    // Clamp a FMAC result to PS2 range and compute its MAC flags and status nibble.
    VU_FAST_INLINE __m128 vuResult(__m128 value, uint8_t dest, uint32_t &mac, uint32_t &statusNibble)
    {
        const __m128i expMask = _mm_set1_epi32(0x7F800000);
        const __m128i signMask = _mm_set1_epi32(static_cast<int>(0x80000000u));
        const __m128i magMask = _mm_set1_epi32(0x7FFFFFFF);
        const __m128i maxMag = _mm_set1_epi32(0x7F7FFFFF);
        __m128i bits = _mm_castps_si128(value);
        const __m128i exponent = _mm_and_si128(bits, expMask);
        const __m128i sign = _mm_and_si128(bits, signMask);
        const __m128i magnitudeZero = _mm_cmpeq_epi32(_mm_and_si128(bits, magMask), _mm_setzero_si128());
        const __m128i overflow = _mm_cmpeq_epi32(exponent, expMask);
        const __m128i underflow = _mm_andnot_si128(magnitudeZero, _mm_cmpeq_epi32(exponent, _mm_setzero_si128()));
        bits = _mm_or_si128(_mm_andnot_si128(overflow, bits), _mm_and_si128(overflow, _mm_or_si128(sign, maxMag)));
        bits = _mm_or_si128(_mm_andnot_si128(underflow, bits), _mm_and_si128(underflow, sign));

        const int destBits = kRev4[dest];
        const int zeroBits = (_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(magnitudeZero, underflow)))) & destBits;
        const int signBits = _mm_movemask_ps(_mm_castsi128_ps(sign)) & destBits;
        const int underBits = _mm_movemask_ps(_mm_castsi128_ps(underflow)) & destBits;
        const int overBits = _mm_movemask_ps(_mm_castsi128_ps(overflow)) & destBits;
        mac = static_cast<uint32_t>(kRev4[zeroBits]) |
              (static_cast<uint32_t>(kRev4[signBits]) << 4) |
              (static_cast<uint32_t>(kRev4[underBits]) << 8) |
              (static_cast<uint32_t>(kRev4[overBits]) << 12);
        statusNibble = (zeroBits ? 1u : 0u) | (signBits ? 2u : 0u) | (underBits ? 4u : 0u) | (overBits ? 8u : 0u);
        return _mm_castsi128_ps(bits);
    }

    inline float normResultScalar(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        const uint32_t exponent = bits & 0x7F800000u;
        if (exponent == 0u)
            bits &= 0x80000000u;
        else if (exponent == 0x7F800000u)
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    inline float fastEatan(float value)
    {
        constexpr float c[] = {0.999999344348907f, -0.333298563957214f, 0.199465364217758f, -0.13085337519646f,
                               0.096420042216778f, -0.055909886956215f, 0.021861229091883f, -0.004054057877511f};
        const float squared = value * value;
        float poly = c[7];
        for (int index = 6; index >= 0; --index)
            poly = c[index] + squared * poly;
        return 0.785398185253143f + value * poly;
    }

    inline float fastEsin(float value)
    {
        constexpr float c[] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f, 0.000002601886990f};
        const float squared = value * value;
        float poly = c[4];
        for (int index = 3; index >= 0; --index)
            poly = c[index] + squared * poly;
        return value * poly;
    }

    inline float fastEexp(float value)
    {
        constexpr float c[] = {0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
                               0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
        float poly = c[5];
        for (int index = 4; index >= 0; --index)
            poly = c[index] + value * poly;
        poly = 1.0f + value * poly;
        poly *= poly;
        poly *= poly;
        return poly != 0.0f ? 1.0f / poly : std::numeric_limits<float>::max();
    }

    // Round toward zero, flush denormal inputs and results, for the duration of a run.
    struct VuFpEnvironment
    {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        unsigned int saved;
        VuFpEnvironment() : saved(_mm_getcsr())
        {
            _mm_setcsr((saved & ~0x6000u) | 0x6000u /*RZ*/ | 0x8000u /*FTZ*/ | 0x0040u /*DAZ*/ | 0x1F80u /*masks*/);
        }
        ~VuFpEnvironment() { _mm_setcsr(saved); }
#else
        int saved;
        VuFpEnvironment() : saved(std::fegetround()) { std::fesetround(FE_TOWARDZERO); }
        ~VuFpEnvironment()
        {
            if (saved != -1)
                std::fesetround(saved);
        }
#endif
    };

    constexpr uint8_t laneBit(uint32_t component) { return static_cast<uint8_t>(8u >> component); }

    inline uint64_t codeHash(const uint8_t *data, size_t size)
    {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
        {
            h ^= data[i];
            h *= 1099511628211ull;
        }
        return h;
    }
}

VU_FAST_INLINE void VU1Interpreter::fastPushFlagEntry(const FastFlagEntry &entry)
{
    if (m_fastFlagCount == kFastFlagRing)
    {
        // Cannot happen with a four-cycle latency and one entry per unit per cycle; keep the newest.
        m_fastFlagHead = (m_fastFlagHead + 1u) % kFastFlagRing;
        --m_fastFlagCount;
    }
    m_fastFlags[(m_fastFlagHead + m_fastFlagCount) % kFastFlagRing] = entry;
    ++m_fastFlagCount;
}

template <uint8_t Kind>
VU_FAST_INLINE void VU1Interpreter::fastExecUpperKind(const FastPair &pair)
{
    using namespace vufast;
    const uint8_t dest = pair.upDest;
    (void)dest;
    if constexpr (Kind == FU_NOP)
    {
            return;
    }
    else if constexpr (Kind == FU_ARITH)
    {
        {
            const __m128 s = vuNorm(_mm_loadu_ps(m_state.vf[pair.upFs]));
            __m128 t;
            switch (pair.upSrc)
            {
            case FSRC_BC: t = _mm_set1_ps(normScalar(m_state.vf[pair.upFt][pair.upBc])); break;
            case FSRC_VT: t = vuNorm(_mm_loadu_ps(m_state.vf[pair.upFt])); break;
            case FSRC_Q: t = _mm_set1_ps(normScalar(m_state.q)); break;
            default: t = _mm_set1_ps(normScalar(m_state.i)); break;
            }
            __m128 result;
            bool fmac = true;
            switch (pair.upArith)
            {
            case FA_ADD: result = _mm_add_ps(s, t); break;
            case FA_SUB: result = _mm_sub_ps(s, t); break;
            case FA_MUL: result = _mm_mul_ps(s, t); break;
            case FA_MADD: result = _mm_add_ps(vuNorm(_mm_loadu_ps(m_state.acc)), _mm_mul_ps(s, t)); break;
            case FA_MSUB: result = _mm_sub_ps(vuNorm(_mm_loadu_ps(m_state.acc)), _mm_mul_ps(s, t)); break;
            case FA_MAX: result = _mm_max_ps(s, t); fmac = false; break;
            case FA_MINI: result = _mm_min_ps(s, t); fmac = false; break;
            case FA_OPMULA:
            case FA_OPMSUB:
            {
                // (s.y*t.z, s.z*t.x, s.x*t.y, 0)
                const __m128 sYzx = _mm_shuffle_ps(s, s, _MM_SHUFFLE(3, 0, 2, 1));
                const __m128 tZxy = _mm_shuffle_ps(t, t, _MM_SHUFFLE(3, 1, 0, 2));
                __m128 product = _mm_mul_ps(sYzx, tZxy);
                if (pair.upArith == FA_OPMSUB)
                    product = _mm_sub_ps(vuNorm(_mm_loadu_ps(m_state.acc)), product);
                result = _mm_and_ps(product, g_laneMasks.mask[0xE]);
                break;
            }
            default: result = _mm_setzero_ps(); break;
            }
            float *target = pair.upToAcc ? m_state.acc : m_state.vf[pair.upFd];
            if (fmac && (!m_fastNeedMacStatus || pair.flagsDead))
            {
                // Nothing in the loaded program reads MAC/status: clamp the result and skip the flags.
                const __m128 maxValue = _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF));
                const __m128 minValue = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0xFF7FFFFFu)));
                result = _mm_min_ps(_mm_max_ps(result, minValue), maxValue);
                storeMasked(target, result, dest);
                return;
            }
            if (fmac)
            {
                if (dest == 0u)
                    return;
                uint32_t mac = 0u;
                uint32_t statusNibble = 0u;
                result = vuResult(result, dest, mac, statusNibble);
                storeMasked(target, result, dest);
                FastFlagEntry entry;
                entry.issueCycle = m_cycle;
                entry.readyCycle = m_cycle + kFmacLatency;
                entry.mac = mac;
                entry.status = statusNibble;
                entry.writes = 1u;
                fastPushFlagEntry(entry);
            }
            else
            {
                storeMasked(target, result, dest);
            }
            return;
        }
    }
    else if constexpr (Kind == FU_ITOF)
    {
        {
            static const float scales[4] = {1.0f, 1.0f / 16.0f, 1.0f / 4096.0f, 1.0f / 32768.0f};
            const __m128i ints = _mm_loadu_si128(reinterpret_cast<const __m128i *>(m_state.vf[pair.upFs]));
            __m128 result = _mm_cvtepi32_ps(ints);
            if (pair.upScale != 0u)
                result = _mm_mul_ps(result, _mm_set1_ps(scales[pair.upScale]));
            storeMasked(m_state.vf[pair.upFt], result, dest);
            return;
        }
    }
    else if constexpr (Kind == FU_FTOI)
    {
        {
            static const float scales[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
            float values[4];
            _mm_storeu_ps(values, vuNorm(_mm_loadu_ps(m_state.vf[pair.upFs])));
            alignas(16) int32_t ints[4];
            const double scale = scales[pair.upScale];
            for (int c = 0; c < 4; ++c)
            {
                const double scaled = static_cast<double>(values[c]) * scale;
                if (scaled >= 2147483647.0)
                    ints[c] = std::numeric_limits<int32_t>::max();
                else if (scaled <= -2147483648.0)
                    ints[c] = std::numeric_limits<int32_t>::min();
                else
                    ints[c] = static_cast<int32_t>(scaled);
            }
            storeMasked(m_state.vf[pair.upFt], _mm_castsi128_ps(_mm_load_si128(reinterpret_cast<const __m128i *>(ints))), dest);
            return;
        }
    }
    else if constexpr (Kind == FU_ABS)
    {
        {
            const __m128 s = vuNorm(_mm_loadu_ps(m_state.vf[pair.upFs]));
            const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
            storeMasked(m_state.vf[pair.upFt], _mm_and_ps(s, absMask), dest);
            return;
        }
    }
    else if constexpr (Kind == FU_CLIP)
    {
        {
            uint32_t wBits = 0u;
            std::memcpy(&wBits, &m_state.vf[pair.upFt][3], sizeof(wBits));
            const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
            uint32_t flags = 0u;
            for (uint32_t component = 0; component < 3u; ++component)
            {
                uint32_t bits = 0u;
                std::memcpy(&bits, &m_state.vf[pair.upFs][component], sizeof(bits));
                int32_t positive = 0;
                int32_t negative = 0;
                const uint32_t flipped = bits ^ 0x80000000u;
                std::memcpy(&positive, &bits, 4);
                std::memcpy(&negative, &flipped, 4);
                if (positive > limit)
                    flags |= 1u << (component * 2u);
                if (negative > limit)
                    flags |= 2u << (component * 2u);
            }
            m_workingClip = ((m_workingClip << 6) | (flags & 0x3Fu)) & 0xFFFFFFu;
            FastFlagEntry entry;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.clip = m_workingClip;
            entry.writes = 4u;
            fastPushFlagEntry(entry);
            return;
        }
    }
    else
    {
            return;
    }
}

VU_FAST_INLINE void VU1Interpreter::fastExecUpper(const FastPair &pair)
{
    using namespace vufast;
    switch (pair.upKind)
    {
    case FU_NOP: fastExecUpperKind<FU_NOP>(pair); return;
    case FU_ARITH: fastExecUpperKind<FU_ARITH>(pair); return;
    case FU_ITOF: fastExecUpperKind<FU_ITOF>(pair); return;
    case FU_FTOI: fastExecUpperKind<FU_FTOI>(pair); return;
    case FU_ABS: fastExecUpperKind<FU_ABS>(pair); return;
    case FU_CLIP: fastExecUpperKind<FU_CLIP>(pair); return;
    default: return;
    }
}

template <uint8_t Kind>
VU_FAST_INLINE void VU1Interpreter::fastExecLowerKind(const FastPair &pair, uint8_t *vuData, uint32_t dataSize)
{
    using namespace vufast;
    const uint32_t dataMask = dataSize - 1u;
    const uint32_t pcMask = m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
    int32_t *vi = m_state.vi;
    auto setVi = [vi](uint8_t reg, int32_t value)
    {
        if (reg != 0u)
            vi[reg] = value;
    };
    auto branchTo = [this](uint32_t target)
    {
        m_state.branchPending = true;
        m_state.branchTarget = target;
        m_state.branchDelay = 1;
    };
    auto relTarget = [&]() { return static_cast<uint32_t>(m_state.pc + 8 + pair.loImm * 8) & pcMask; };
    auto loadQ = [&](uint32_t address, uint8_t reg)
    {
        address &= dataMask;
        storeMasked(m_state.vf[reg], _mm_loadu_ps(reinterpret_cast<const float *>(vuData + address)), pair.loDest);
    };
    auto storeQ = [&](uint32_t address, const float *value)
    {
        address &= dataMask;
        storeMasked(reinterpret_cast<float *>(vuData + address), _mm_loadu_ps(value), pair.loDest);
    };
    auto storeI = [&](uint32_t address, int32_t value)
    {
        address &= dataMask;
        const uint32_t word = static_cast<uint32_t>(static_cast<uint16_t>(value & 0xFFFF));
        uint32_t *dst = reinterpret_cast<uint32_t *>(vuData + address);
        for (uint32_t c = 0; c < 4u; ++c)
            if (pair.loDest & laneBit(c))
                std::memcpy(dst + c, &word, 4);
    };
    auto loadI = [&](uint32_t address) -> int32_t
    {
        address &= dataMask;
        uint32_t component = 3u;
        if (pair.loDest & 0x8u)
            component = 0u;
        else if (pair.loDest & 0x4u)
            component = 1u;
        else if (pair.loDest & 0x2u)
            component = 2u;
        uint32_t value = 0u;
        std::memcpy(&value, vuData + address + component * 4u, 4);
        return static_cast<int32_t>(static_cast<int16_t>(value & 0xFFFFu));
    };
    auto broadcastStore = [&](uint8_t reg, float value)
    {
        storeMasked(m_state.vf[reg], _mm_set1_ps(value), pair.loDest);
    };
    auto queueP = [&](float value, uint32_t latency)
    {
        value = normResultScalar(value);
        for (ScalarPipelineEntry &entry : m_efu)
        {
            if (!entry.valid)
            {
                entry.valid = true;
                entry.readyCycle = m_cycle + latency;
                entry.value = value;
                m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
                return;
            }
        }
    };
    auto queueQ = [&](float value, uint32_t latency, uint32_t statusDi)
    {
        m_fdiv.valid = true;
        m_fdiv.readyCycle = m_cycle + latency;
        m_fdiv.value = normResultScalar(value);
        m_fdiv.statusDi = statusDi & 0x30u;
    };
    const float *vfs = m_state.vf[pair.loFs];
    const uint8_t is = pair.loFs & 0xFu; // VI fields alias the low four bits of the VF fields
    const uint8_t it = pair.loFt & 0xFu;
    const uint8_t id = pair.loFd & 0xFu;

    (void)dataMask;
    (void)pcMask;
    (void)setVi;
    (void)branchTo;
    (void)relTarget;
    (void)loadQ;
    (void)storeQ;
    (void)storeI;
    (void)loadI;
    (void)broadcastStore;
    (void)queueP;
    (void)queueQ;
    (void)is;
    (void)it;
    (void)id;
    if constexpr (Kind == FL_NOP)
    {
            return;
    }
    else if constexpr (Kind == FL_LQ)
    {
            loadQ(static_cast<uint32_t>(static_cast<int32_t>(vi[is] + pair.loImm)) * 16u, pair.loFt);
            return;
    }
    else if constexpr (Kind == FL_SQ)
    {
            storeQ(static_cast<uint32_t>(static_cast<int32_t>(vi[it] + pair.loImm)) * 16u, vfs);
            return;
    }
    else if constexpr (Kind == FL_ILW)
    {
            setVi(it, loadI(static_cast<uint32_t>(static_cast<int32_t>(vi[is] + pair.loImm)) * 16u));
            return;
    }
    else if constexpr (Kind == FL_ISW)
    {
            storeI(static_cast<uint32_t>(static_cast<int32_t>(vi[is] + pair.loImm)) * 16u, vi[it]);
            return;
    }
    else if constexpr (Kind == FL_IADDIU)
    {
            setVi(it, static_cast<int16_t>(vi[is] + pair.loImm));
            return;
    }
    else if constexpr (Kind == FL_ISUBIU)
    {
            setVi(it, static_cast<int16_t>(vi[is] - pair.loImm));
            return;
    }
    else if constexpr (Kind == FL_FCEQ)
    {
            vi[1] = ((m_state.clip & 0xFFFFFFu) == pair.loImm24) ? 1 : 0;
            return;
    }
    else if constexpr (Kind == FL_FCSET)
    {
        {
            m_workingClip = pair.loImm24;
            for (uint32_t index = 0; index < m_fastFlagCount; ++index)
            {
                FastFlagEntry &entry = m_fastFlags[(m_fastFlagHead + index) % kFastFlagRing];
                if (entry.issueCycle == m_cycle)
                    entry.writes &= static_cast<uint8_t>(~4u);
            }
            FastFlagEntry entry;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.clip = m_workingClip;
            entry.writes = 4u;
            fastPushFlagEntry(entry);
            return;
        }
    }
    else if constexpr (Kind == FL_FCAND)
    {
            vi[1] = ((m_state.clip & pair.loImm24) != 0u) ? 1 : 0;
            return;
    }
    else if constexpr (Kind == FL_FCOR)
    {
            vi[1] = ((m_state.clip | pair.loImm24) == 0xFFFFFFu) ? 1 : 0;
            return;
    }
    else if constexpr (Kind == FL_FSEQ)
    {
            setVi(it, ((m_state.status & 0xFFFu) == pair.loImm24) ? 1 : 0);
            return;
    }
    else if constexpr (Kind == FL_FSSET)
    {
        {
            for (uint32_t index = 0; index < m_fastFlagCount; ++index)
            {
                FastFlagEntry &entry = m_fastFlags[(m_fastFlagHead + index) % kFastFlagRing];
                if (entry.issueCycle == m_cycle)
                    entry.writes &= static_cast<uint8_t>(~1u);
            }
            FastFlagEntry entry;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.status = pair.loImm24 & 0xFC0u;
            entry.writes = 2u;
            fastPushFlagEntry(entry);
            return;
        }
    }
    else if constexpr (Kind == FL_FSAND)
    {
            setVi(it, static_cast<int32_t>((m_state.status & 0xFFFu) & pair.loImm24));
            return;
    }
    else if constexpr (Kind == FL_FSOR)
    {
            setVi(it, static_cast<int32_t>((m_state.status & 0xFFFu) | pair.loImm24));
            return;
    }
    else if constexpr (Kind == FL_FMEQ)
    {
            setVi(it, ((m_state.mac & 0xFFFFu) == static_cast<uint32_t>(static_cast<uint16_t>(vi[is]))) ? 1 : 0);
            return;
    }
    else if constexpr (Kind == FL_FMAND)
    {
            setVi(it, static_cast<int32_t>(m_state.mac & static_cast<uint32_t>(static_cast<uint16_t>(vi[is]))));
            return;
    }
    else if constexpr (Kind == FL_FMOR)
    {
            setVi(it, static_cast<int32_t>(m_state.mac | static_cast<uint32_t>(static_cast<uint16_t>(vi[is]))));
            return;
    }
    else if constexpr (Kind == FL_FCGET)
    {
            setVi(it, static_cast<int32_t>(m_state.clip & 0x0FFFu));
            return;
    }
    else if constexpr (Kind == FL_B)
    {
            branchTo(relTarget());
            return;
    }
    else if constexpr (Kind == FL_BAL)
    {
            setVi(it, static_cast<int32_t>((m_state.pc + 16u) / 8u));
            branchTo(relTarget());
            return;
    }
    else if constexpr (Kind == FL_JR)
    {
            branchTo((static_cast<uint32_t>(static_cast<uint16_t>(readBranchVi(is))) * 8u) & pcMask);
            return;
    }
    else if constexpr (Kind == FL_JALR)
    {
        {
            const uint32_t target = (static_cast<uint32_t>(static_cast<uint16_t>(readBranchVi(is))) * 8u) & pcMask;
            setVi(it, static_cast<int32_t>((m_state.pc + 16u) / 8u));
            branchTo(target);
            return;
        }
    }
    else if constexpr (Kind == FL_IBEQ)
    {
            if (static_cast<int16_t>(readBranchVi(is)) == static_cast<int16_t>(readBranchVi(it)))
                branchTo(relTarget());
            return;
    }
    else if constexpr (Kind == FL_IBNE)
    {
            if (static_cast<int16_t>(readBranchVi(is)) != static_cast<int16_t>(readBranchVi(it)))
                branchTo(relTarget());
            return;
    }
    else if constexpr (Kind == FL_IBLTZ)
    {
            if (static_cast<int16_t>(readBranchVi(is)) < 0)
                branchTo(relTarget());
            return;
    }
    else if constexpr (Kind == FL_IBGTZ)
    {
            if (static_cast<int16_t>(readBranchVi(is)) > 0)
                branchTo(relTarget());
            return;
    }
    else if constexpr (Kind == FL_IBLEZ)
    {
            if (static_cast<int16_t>(readBranchVi(is)) <= 0)
                branchTo(relTarget());
            return;
    }
    else if constexpr (Kind == FL_IBGEZ)
    {
            if (static_cast<int16_t>(readBranchVi(is)) >= 0)
                branchTo(relTarget());
            return;
    }
    else if constexpr (Kind == FL_IADD)
    {
            setVi(id, static_cast<int16_t>(vi[is] + vi[it]));
            return;
    }
    else if constexpr (Kind == FL_ISUB)
    {
            setVi(id, static_cast<int16_t>(vi[is] - vi[it]));
            return;
    }
    else if constexpr (Kind == FL_IADDI)
    {
            setVi(it, static_cast<int16_t>(vi[is] + pair.loImm));
            return;
    }
    else if constexpr (Kind == FL_IAND)
    {
            setVi(id, vi[is] & vi[it]);
            return;
    }
    else if constexpr (Kind == FL_IOR)
    {
            setVi(id, vi[is] | vi[it]);
            return;
    }
    else if constexpr (Kind == FL_MOVE)
    {
            storeMasked(m_state.vf[pair.loFt], _mm_loadu_ps(vfs), pair.loDest);
            return;
    }
    else if constexpr (Kind == FL_MR32)
    {
        {
            const __m128 s = _mm_loadu_ps(vfs);
            storeMasked(m_state.vf[pair.loFt], _mm_shuffle_ps(s, s, _MM_SHUFFLE(0, 3, 2, 1)), pair.loDest);
            return;
        }
    }
    else if constexpr (Kind == FL_LQI)
    {
            loadQ(static_cast<uint32_t>(static_cast<uint16_t>(vi[is])) * 16u, pair.loFt);
            setVi(is, static_cast<int16_t>(vi[is] + 1));
            return;
    }
    else if constexpr (Kind == FL_SQI)
    {
            storeQ(static_cast<uint32_t>(static_cast<uint16_t>(vi[it])) * 16u, vfs);
            setVi(it, static_cast<int16_t>(vi[it] + 1));
            return;
    }
    else if constexpr (Kind == FL_LQD)
    {
            setVi(is, static_cast<int16_t>(vi[is] - 1));
            loadQ(static_cast<uint32_t>(static_cast<uint16_t>(vi[is])) * 16u, pair.loFt);
            return;
    }
    else if constexpr (Kind == FL_SQD)
    {
            setVi(it, static_cast<int16_t>(vi[it] - 1));
            storeQ(static_cast<uint32_t>(static_cast<uint16_t>(vi[it])) * 16u, vfs);
            return;
    }
    else if constexpr (Kind == FL_DIV)
    {
        {
            const float num = normScalar(vfs[pair.loFsf]);
            const float den = normScalar(m_state.vf[pair.loFt][pair.loFtf]);
            uint32_t statusDi = 0u;
            float result;
            if (den == 0.0f)
            {
                statusDi = num == 0.0f ? 0x10u : 0x20u;
                result = std::signbit(num) != std::signbit(den) ? -std::numeric_limits<float>::max()
                                                                 : std::numeric_limits<float>::max();
            }
            else
            {
                result = num / den;
            }
            queueQ(result, 7u, statusDi);
            return;
        }
    }
    else if constexpr (Kind == FL_SQRT)
    {
        {
            const float value = normScalar(m_state.vf[pair.loFt][pair.loFtf]);
            queueQ(std::sqrt(std::fabs(value)), 7u, value < 0.0f ? 0x10u : 0u);
            return;
        }
    }
    else if constexpr (Kind == FL_RSQRT)
    {
        {
            const float num = normScalar(vfs[pair.loFsf]);
            const float radicand = normScalar(m_state.vf[pair.loFt][pair.loFtf]);
            const float den = std::sqrt(std::fabs(radicand));
            uint32_t statusDi = radicand < 0.0f ? 0x10u : 0u;
            float result;
            if (den != 0.0f)
            {
                result = num / den;
            }
            else
            {
                statusDi = num == 0.0f ? 0x10u : 0x20u;
                result = std::signbit(num) ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
            }
            queueQ(result, 13u, statusDi);
            return;
        }
    }
    else if constexpr (Kind == FL_WAITQ || Kind == FL_WAITP)
    {
            return;
    }
    else if constexpr (Kind == FL_MTIR)
    {
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &vfs[pair.loFsf], 4);
            setVi(it, static_cast<int32_t>(static_cast<int16_t>(bits & 0xFFFFu)));
            return;
        }
    }
    else if constexpr (Kind == FL_MFIR)
    {
        {
            const int32_t value = static_cast<int32_t>(static_cast<int16_t>(vi[is] & 0xFFFF));
            storeMasked(m_state.vf[pair.loFt], _mm_castsi128_ps(_mm_set1_epi32(value)), pair.loDest);
            return;
        }
    }
    else if constexpr (Kind == FL_ILWR)
    {
            setVi(it, loadI(static_cast<uint32_t>(static_cast<uint16_t>(vi[is])) * 16u));
            return;
    }
    else if constexpr (Kind == FL_ISWR)
    {
            storeI(static_cast<uint32_t>(static_cast<uint16_t>(vi[is])) * 16u, vi[it]);
            return;
    }
    else if constexpr (Kind == FL_RNEXT)
    {
        {
            const uint32_t x = (m_state.r >> 4) & 1u;
            const uint32_t y = (m_state.r >> 22) & 1u;
            m_state.r = (((m_state.r << 1) ^ x ^ y) & 0x007FFFFFu) | 0x3F800000u;
            float value;
            std::memcpy(&value, &m_state.r, 4);
            broadcastStore(pair.loFt, value);
            return;
        }
    }
    else if constexpr (Kind == FL_RGET)
    {
        {
            float value;
            std::memcpy(&value, &m_state.r, 4);
            broadcastStore(pair.loFt, value);
            return;
        }
    }
    else if constexpr (Kind == FL_RINIT)
    {
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &vfs[pair.loFsf], 4);
            m_state.r = 0x3F800000u | (bits & 0x007FFFFFu);
            return;
        }
    }
    else if constexpr (Kind == FL_RXOR)
    {
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &vfs[pair.loFsf], 4);
            m_state.r = 0x3F800000u | ((m_state.r ^ bits) & 0x007FFFFFu);
            return;
        }
    }
    else if constexpr (Kind == FL_MFP)
    {
            broadcastStore(pair.loFt, m_state.p);
            return;
    }
    else if constexpr (Kind == FL_XTOP)
    {
            setVi(it, static_cast<int32_t>(m_state.top & 0x3FFu));
            return;
    }
    else if constexpr (Kind == FL_XITOP)
    {
            setVi(it, static_cast<int32_t>(m_state.itop & 0x3FFu));
            return;
    }
    else if constexpr (Kind == FL_XGKICK)
    {
            fastXgkick(static_cast<uint32_t>(static_cast<uint16_t>(vi[is])), vuData, dataSize);
            return;
    }
    else if constexpr (Kind == FL_ESADD || Kind == FL_ERSADD || Kind == FL_ELENG || Kind == FL_ERLENG)
    {
        {
            const float x = normScalar(vfs[0]);
            const float y = normScalar(vfs[1]);
            const float z = normScalar(vfs[2]);
            const float sum = x * x + y * y + z * z;
            switch (pair.loKind)
            {
            case FL_ESADD: queueP(sum, 11u); break;
            case FL_ERSADD: queueP(sum != 0.0f ? 1.0f / sum : sum, 18u); break;
            case FL_ELENG: queueP(std::sqrt(sum), 18u); break;
            default:
            {
                const float length = std::sqrt(sum);
                queueP(length != 0.0f ? 1.0f / length : length, 24u);
                break;
            }
            }
            return;
        }
    }
    else if constexpr (Kind == FL_EATANXY)
    {
        {
            const float x = normScalar(vfs[0]);
            const float y = normScalar(vfs[1]);
            queueP(x != 0.0f ? fastEatan(y / x) : 0.0f, 54u);
            return;
        }
    }
    else if constexpr (Kind == FL_EATANXZ)
    {
        {
            const float x = normScalar(vfs[0]);
            const float z = normScalar(vfs[2]);
            queueP(x != 0.0f ? fastEatan(z / x) : 0.0f, 54u);
            return;
        }
    }
    else if constexpr (Kind == FL_ESUM)
    {
            queueP(normScalar(vfs[0]) + normScalar(vfs[1]) + normScalar(vfs[2]) + normScalar(vfs[3]), 12u);
            return;
    }
    else if constexpr (Kind == FL_ERSQRT)
    {
        {
            float value = normScalar(vfs[pair.loFsf]);
            if (value >= 0.0f)
            {
                value = std::sqrt(value);
                if (value != 0.0f)
                    value = 1.0f / value;
            }
            queueP(value, 18u);
            return;
        }
    }
    else if constexpr (Kind == FL_ESQRT)
    {
        {
            const float value = normScalar(vfs[pair.loFsf]);
            queueP(value >= 0.0f ? std::sqrt(value) : value, 12u);
            return;
        }
    }
    else if constexpr (Kind == FL_ESIN)
    {
            queueP(fastEsin(normScalar(vfs[pair.loFsf])), 29u);
            return;
    }
    else if constexpr (Kind == FL_ERCPR)
    {
        {
            const float value = normScalar(vfs[pair.loFsf]);
            queueP(value != 0.0f ? 1.0f / value : value, 12u);
            return;
        }
    }
    else if constexpr (Kind == FL_EATAN)
    {
            queueP(fastEatan(normScalar(vfs[pair.loFsf])), 54u);
            return;
    }
    else if constexpr (Kind == FL_EEXP)
    {
            queueP(fastEexp(normScalar(vfs[pair.loFsf])), 44u);
            return;
    }
    else
    {
            return;
    }
}

VU_FAST_INLINE void VU1Interpreter::fastExecLower(const FastPair &pair, uint8_t *vuData, uint32_t dataSize)
{
    using namespace vufast;
    switch (pair.loKind)
    {
    case FL_NOP: fastExecLowerKind<FL_NOP>(pair, vuData, dataSize); return;
    case FL_LQ: fastExecLowerKind<FL_LQ>(pair, vuData, dataSize); return;
    case FL_SQ: fastExecLowerKind<FL_SQ>(pair, vuData, dataSize); return;
    case FL_ILW: fastExecLowerKind<FL_ILW>(pair, vuData, dataSize); return;
    case FL_ISW: fastExecLowerKind<FL_ISW>(pair, vuData, dataSize); return;
    case FL_IADDIU: fastExecLowerKind<FL_IADDIU>(pair, vuData, dataSize); return;
    case FL_ISUBIU: fastExecLowerKind<FL_ISUBIU>(pair, vuData, dataSize); return;
    case FL_FCEQ: fastExecLowerKind<FL_FCEQ>(pair, vuData, dataSize); return;
    case FL_FCSET: fastExecLowerKind<FL_FCSET>(pair, vuData, dataSize); return;
    case FL_FCAND: fastExecLowerKind<FL_FCAND>(pair, vuData, dataSize); return;
    case FL_FCOR: fastExecLowerKind<FL_FCOR>(pair, vuData, dataSize); return;
    case FL_FSEQ: fastExecLowerKind<FL_FSEQ>(pair, vuData, dataSize); return;
    case FL_FSSET: fastExecLowerKind<FL_FSSET>(pair, vuData, dataSize); return;
    case FL_FSAND: fastExecLowerKind<FL_FSAND>(pair, vuData, dataSize); return;
    case FL_FSOR: fastExecLowerKind<FL_FSOR>(pair, vuData, dataSize); return;
    case FL_FMEQ: fastExecLowerKind<FL_FMEQ>(pair, vuData, dataSize); return;
    case FL_FMAND: fastExecLowerKind<FL_FMAND>(pair, vuData, dataSize); return;
    case FL_FMOR: fastExecLowerKind<FL_FMOR>(pair, vuData, dataSize); return;
    case FL_FCGET: fastExecLowerKind<FL_FCGET>(pair, vuData, dataSize); return;
    case FL_B: fastExecLowerKind<FL_B>(pair, vuData, dataSize); return;
    case FL_BAL: fastExecLowerKind<FL_BAL>(pair, vuData, dataSize); return;
    case FL_JR: fastExecLowerKind<FL_JR>(pair, vuData, dataSize); return;
    case FL_JALR: fastExecLowerKind<FL_JALR>(pair, vuData, dataSize); return;
    case FL_IBEQ: fastExecLowerKind<FL_IBEQ>(pair, vuData, dataSize); return;
    case FL_IBNE: fastExecLowerKind<FL_IBNE>(pair, vuData, dataSize); return;
    case FL_IBLTZ: fastExecLowerKind<FL_IBLTZ>(pair, vuData, dataSize); return;
    case FL_IBGTZ: fastExecLowerKind<FL_IBGTZ>(pair, vuData, dataSize); return;
    case FL_IBLEZ: fastExecLowerKind<FL_IBLEZ>(pair, vuData, dataSize); return;
    case FL_IBGEZ: fastExecLowerKind<FL_IBGEZ>(pair, vuData, dataSize); return;
    case FL_IADD: fastExecLowerKind<FL_IADD>(pair, vuData, dataSize); return;
    case FL_ISUB: fastExecLowerKind<FL_ISUB>(pair, vuData, dataSize); return;
    case FL_IADDI: fastExecLowerKind<FL_IADDI>(pair, vuData, dataSize); return;
    case FL_IAND: fastExecLowerKind<FL_IAND>(pair, vuData, dataSize); return;
    case FL_IOR: fastExecLowerKind<FL_IOR>(pair, vuData, dataSize); return;
    case FL_MOVE: fastExecLowerKind<FL_MOVE>(pair, vuData, dataSize); return;
    case FL_MR32: fastExecLowerKind<FL_MR32>(pair, vuData, dataSize); return;
    case FL_LQI: fastExecLowerKind<FL_LQI>(pair, vuData, dataSize); return;
    case FL_SQI: fastExecLowerKind<FL_SQI>(pair, vuData, dataSize); return;
    case FL_LQD: fastExecLowerKind<FL_LQD>(pair, vuData, dataSize); return;
    case FL_SQD: fastExecLowerKind<FL_SQD>(pair, vuData, dataSize); return;
    case FL_DIV: fastExecLowerKind<FL_DIV>(pair, vuData, dataSize); return;
    case FL_SQRT: fastExecLowerKind<FL_SQRT>(pair, vuData, dataSize); return;
    case FL_RSQRT: fastExecLowerKind<FL_RSQRT>(pair, vuData, dataSize); return;
    case FL_WAITQ: fastExecLowerKind<FL_WAITQ>(pair, vuData, dataSize); return;
    case FL_WAITP: fastExecLowerKind<FL_WAITP>(pair, vuData, dataSize); return;
    case FL_MTIR: fastExecLowerKind<FL_MTIR>(pair, vuData, dataSize); return;
    case FL_MFIR: fastExecLowerKind<FL_MFIR>(pair, vuData, dataSize); return;
    case FL_ILWR: fastExecLowerKind<FL_ILWR>(pair, vuData, dataSize); return;
    case FL_ISWR: fastExecLowerKind<FL_ISWR>(pair, vuData, dataSize); return;
    case FL_RNEXT: fastExecLowerKind<FL_RNEXT>(pair, vuData, dataSize); return;
    case FL_RGET: fastExecLowerKind<FL_RGET>(pair, vuData, dataSize); return;
    case FL_RINIT: fastExecLowerKind<FL_RINIT>(pair, vuData, dataSize); return;
    case FL_RXOR: fastExecLowerKind<FL_RXOR>(pair, vuData, dataSize); return;
    case FL_MFP: fastExecLowerKind<FL_MFP>(pair, vuData, dataSize); return;
    case FL_XTOP: fastExecLowerKind<FL_XTOP>(pair, vuData, dataSize); return;
    case FL_XITOP: fastExecLowerKind<FL_XITOP>(pair, vuData, dataSize); return;
    case FL_XGKICK: fastExecLowerKind<FL_XGKICK>(pair, vuData, dataSize); return;
    case FL_ESADD: fastExecLowerKind<FL_ESADD>(pair, vuData, dataSize); return;
    case FL_ERSADD: fastExecLowerKind<FL_ERSADD>(pair, vuData, dataSize); return;
    case FL_ELENG: fastExecLowerKind<FL_ELENG>(pair, vuData, dataSize); return;
    case FL_ERLENG: fastExecLowerKind<FL_ERLENG>(pair, vuData, dataSize); return;
    case FL_EATANXY: fastExecLowerKind<FL_EATANXY>(pair, vuData, dataSize); return;
    case FL_EATANXZ: fastExecLowerKind<FL_EATANXZ>(pair, vuData, dataSize); return;
    case FL_ESUM: fastExecLowerKind<FL_ESUM>(pair, vuData, dataSize); return;
    case FL_ERSQRT: fastExecLowerKind<FL_ERSQRT>(pair, vuData, dataSize); return;
    case FL_ESQRT: fastExecLowerKind<FL_ESQRT>(pair, vuData, dataSize); return;
    case FL_ESIN: fastExecLowerKind<FL_ESIN>(pair, vuData, dataSize); return;
    case FL_ERCPR: fastExecLowerKind<FL_ERCPR>(pair, vuData, dataSize); return;
    case FL_EATAN: fastExecLowerKind<FL_EATAN>(pair, vuData, dataSize); return;
    case FL_EEXP: fastExecLowerKind<FL_EEXP>(pair, vuData, dataSize); return;
    default: return;
    }
}


// Friend of VU1Interpreter: the per-pair steps of the fast core, used by runFast and by the
// compiled microprograms.
struct Vu1Aot
{
    using Pair = VU1Interpreter::FastPair;
    using Fn = int (*)(VU1Interpreter &vu, uint8_t *vuData, uint32_t dataSize, uint64_t budgetEnd, uint64_t &pairs);
    enum Result : int
    {
        kUnhandled = 0, // the program counter is outside the compiled code: interpret this pair
        kBudget = 1,    // the cycle budget ran out (m_state.pc = where to continue)
        kEnded = 2,     // the program ended (E bit, D/T halt)
        kContinue = 3,  // inside compiled code only: continue at m_state.pc
    };

    // VU1Interpreter::fastCommit, inlined into the per-pair path.
    static VU_FAST_INLINE void commit(VU1Interpreter &vu)
    {
        while (vu.m_fastFlagCount != 0u)
        {
            const VU1Interpreter::FastFlagEntry &entry = vu.m_fastFlags[vu.m_fastFlagHead];
            if (entry.readyCycle > vu.m_cycle)
                break;
            if (entry.writes & 1u)
            {
                vu.m_state.mac = entry.mac;
                const uint32_t current = entry.status & 0xFu;
                vu.m_state.status = (vu.m_state.status & 0xFF0u) | current | (current << 6);
            }
            if (entry.writes & 2u)
                vu.m_state.status = (vu.m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
            if (entry.writes & 4u)
                vu.m_state.clip = entry.clip;
            vu.m_fastFlagHead = (vu.m_fastFlagHead + 1u) % VU1Interpreter::kFastFlagRing;
            --vu.m_fastFlagCount;
        }
        if (vu.m_fdiv.valid && vu.m_fdiv.readyCycle <= vu.m_cycle)
        {
            vu.m_state.q = vu.m_fdiv.value;
            const uint32_t currentDi = vu.m_fdiv.statusDi & 0x30u;
            vu.m_state.status = (vu.m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
            vu.m_fdiv.valid = false;
        }
        for (VU1Interpreter::ScalarPipelineEntry &entry : vu.m_efu)
        {
            if (entry.valid && entry.readyCycle <= vu.m_cycle)
            {
                vu.m_state.p = entry.value;
                entry.valid = false;
            }
        }
    }

    // How pairCore executes the upper/lower instruction: through the kind switch (interpreter),
    // or straight into the one kind of a compile-time constant pair (compiled programs).
    struct RuntimeExec
    {
        static VU_FAST_INLINE void upper(VU1Interpreter &vu, const Pair &pair) { vu.fastExecUpper(pair); }
        static VU_FAST_INLINE void lower(VU1Interpreter &vu, const Pair &pair, uint8_t *vuData, uint32_t dataSize)
        {
            vu.fastExecLower(pair, vuData, dataSize);
        }
    };
    template <uint8_t Kind>
    static VU_FAST_INLINE void upperKind(VU1Interpreter &vu, const Pair &pair) { vu.fastExecUpperKind<Kind>(pair); }
    template <uint8_t Kind>
    static VU_FAST_INLINE void lowerKind(VU1Interpreter &vu, const Pair &pair, uint8_t *vuData, uint32_t dataSize)
    {
        vu.fastExecLowerKind<Kind>(pair, vuData, dataSize);
    }

    // Stall, commit, execute one pair. False when the budget ran out before it could issue.
    static VU_FAST_INLINE bool pairCore(VU1Interpreter &vu, const Pair &pair, uint8_t *vuData, uint32_t dataSize,
                                        uint64_t budgetEnd, uint64_t &pairs)
    {
        return pairCoreWith<RuntimeExec>(vu, pair, vuData, dataSize, budgetEnd, pairs);
    }

    template <typename Exec>
    static VU_FAST_INLINE bool pairCoreWith(VU1Interpreter &vu, const Pair &pair, uint8_t *vuData, uint32_t dataSize,
                                            uint64_t budgetEnd, uint64_t &pairs)
    {
        using namespace vufast;
        // Stall until every operand this pair reads is ready.
        uint64_t ready = vu.m_cycle;
        for (uint32_t index = 0; index < pair.readCount; ++index)
        {
            const auto &lanes = vu.m_vfReady[pair.readReg[index]];
            const uint8_t mask = pair.readLanes[index];
            if (mask & 8u) ready = std::max(ready, lanes[0]);
            if (mask & 4u) ready = std::max(ready, lanes[1]);
            if (mask & 2u) ready = std::max(ready, lanes[2]);
            if (mask & 1u) ready = std::max(ready, lanes[3]);
        }
        if (pair.viRead != 0u)
        {
            for (uint32_t bits = pair.viRead; bits != 0u; bits &= bits - 1u)
            {
#if defined(_MSC_VER) && !defined(__clang__)
                unsigned long reg;
                _BitScanForward(&reg, bits);
#else
                const uint32_t reg = static_cast<uint32_t>(__builtin_ctz(bits));
#endif
                ready = std::max(ready, vu.m_viReady[reg]);
            }
        }
        if (pair.loPipeline != VU1Interpreter::PipelineNone)
        {
            if ((pair.loPipeline == VU1Interpreter::PipelineFdiv || pair.waitQ) && vu.m_fdiv.valid)
                ready = std::max(ready, vu.m_fdiv.readyCycle);
            if (pair.loPipeline == VU1Interpreter::PipelineEfu && !pair.waitP)
                ready = std::max(ready, vu.m_efuResourceReady);
            if (pair.waitP)
                for (const VU1Interpreter::ScalarPipelineEntry &entry : vu.m_efu)
                    if (entry.valid)
                        ready = std::max(ready, entry.readyCycle);
        }
        if (ready > vu.m_cycle)
        {
            if (ready >= budgetEnd)
            {
                vu.m_cycle = budgetEnd;
                vu.fastCommit();
                return false;
            }
            vu.m_cycle = ready;
        }
        if (vu.m_fastFlagCount != 0u || vu.m_fdiv.valid || vu.m_efu[0].valid || vu.m_efu[1].valid)
            commit(vu);
#if defined(PS2X_VU1_TRACE_PAIRS)
        if (g_vu1TracePairs) [[unlikely]]
            std::fprintf(stderr, "[trace] #%llu cycle %llu up %08x lo %08x\n", (unsigned long long)pairs, (unsigned long long)vu.m_cycle, pair.upper, pair.lower);
#endif
        ++pairs;

        const uint8_t viWriteReg = pair.viWriteReg;
        const int32_t oldVi = viWriteReg ? vu.m_state.vi[viWriteReg] : 0;

        if (pair.iBit)
        {
            Exec::upper(vu, pair);
            vu.m_state.i = normScalar(pair.immediate);
        }
        else if (pair.shadowReg != 0u)
        {
            float oldValue[4];
            float upperValue[4];
            std::memcpy(oldValue, vu.m_state.vf[pair.shadowReg], sizeof(oldValue));
            Exec::upper(vu, pair);
            std::memcpy(upperValue, vu.m_state.vf[pair.shadowReg], sizeof(upperValue));
            std::memcpy(vu.m_state.vf[pair.shadowReg], oldValue, sizeof(oldValue));
            Exec::lower(vu, pair, vuData, dataSize);
            std::memcpy(vu.m_state.vf[pair.shadowReg], upperValue, sizeof(upperValue));
        }
        else
        {
            Exec::upper(vu, pair);
            if (pair.loKind != FL_NOP)
                Exec::lower(vu, pair, vuData, dataSize);
        }

        // Result visibility for later stall checks.
        if (pair.upWriteReg != 0u)
        {
            auto &lanes = vu.m_vfReady[pair.upWriteReg];
            const uint64_t at = vu.m_cycle + pair.upLatency;
            if (pair.upWriteLanes & 8u) lanes[0] = at;
            if (pair.upWriteLanes & 4u) lanes[1] = at;
            if (pair.upWriteLanes & 2u) lanes[2] = at;
            if (pair.upWriteLanes & 1u) lanes[3] = at;
        }
        if (pair.loWriteReg != 0u)
        {
            auto &lanes = vu.m_vfReady[pair.loWriteReg];
            const uint64_t at = vu.m_cycle + pair.loVfLatency;
            if (pair.loWriteLanes & 8u) lanes[0] = at;
            if (pair.loWriteLanes & 4u) lanes[1] = at;
            if (pair.loWriteLanes & 2u) lanes[2] = at;
            if (pair.loWriteLanes & 1u) lanes[3] = at;
        }
        vu.m_viBranchBackupValid = false;
        if (viWriteReg != 0u)
        {
            vu.m_viReady[viWriteReg] = vu.m_cycle + pair.viLatency;
            if (pair.delaysNextBranchRead)
                vu.recordViWriteForBranch(viWriteReg, oldVi);
        }
        if ((pair.upWriteReg | pair.loWriteReg | viWriteReg) != 0u)
        {
            const uint64_t latest = vu.m_cycle + std::max<uint32_t>(std::max<uint32_t>(pair.upWriteReg ? pair.upLatency : 0u,
                                                                                          pair.loWriteReg ? pair.loVfLatency : 0u),
                                                                    viWriteReg ? pair.viLatency : 0u);
            uint64_t &until = pendingUntil(vu);
            if (latest > until)
                until = latest;
        }

        if (pair.writesVf0)
        {
            vu.m_state.vf[0][0] = 0.0f;
            vu.m_state.vf[0][1] = 0.0f;
            vu.m_state.vf[0][2] = 0.0f;
            vu.m_state.vf[0][3] = 1.0f;
        }
        vu.m_state.vi[0] = 0;

        return true;
    }

    // Program counter, branch delay slots, E/D/T bits, cycle. True when the program ended.
    static VU_FAST_INLINE bool postPair(VU1Interpreter &vu, const Pair &pair, uint32_t codeSize)
    {
        bool programEnded = false;
        uint32_t nextPc = vu.m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        vu.m_state.pc = nextPc;
        if (vu.m_state.branchPending)
        {
            if (vu.m_state.branchDelay == 0u)
            {
                vu.m_state.pc = vu.m_state.branchTarget & (vu.m_unit == VU1Interpreter::Unit::VU1 ? 0x3FFFu : 0x0FFFu);
                vu.m_state.branchPending = false;
            }
            else
            {
                --vu.m_state.branchDelay;
            }
        }

        const bool dHalt = pair.dBit && vu.m_state.dBitEnabled;
        const bool tHalt = pair.tBit && vu.m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && pair.loPipeline == VU1Interpreter::PipelineBranch;
        if (vu.m_state.haltAfterDelaySlot)
        {
            vu.m_state.stoppedByD = vu.m_pendingHaltD;
            vu.m_state.stoppedByT = vu.m_pendingHaltT;
            programEnded = true;
        }
        else if (vu.m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            vu.m_state.stoppedByD = dHalt;
            vu.m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (pair.eBit)
            vu.m_state.ebit = true;
        else if (haltBranch)
        {
            vu.m_state.haltAfterDelaySlot = true;
            vu.m_pendingHaltD = dHalt;
            vu.m_pendingHaltT = tHalt;
        }

        ++vu.m_cycle;
        return programEnded;
    }

    // FU_ARITH / FU_ABS / FU_FTOI for the static tier: operands are brought into PS2 range
    // (denormal -> 0, Inf/NaN exponent -> max) only where the generator could not prove they
    // already are. Norm bits: 1 = fs, 2 = ft (or the broadcast lane), 4 = ACC. Q and I are
    // always in range (normalized when they are written). Everything else is fastExecUpperKind.
    template <uint8_t Kind, uint8_t Norm>
    static VU_FAST_INLINE void upperStatic(VU1Interpreter &vu, const Pair &pair)
    {
        using namespace vufast;
        if constexpr (Kind == FU_ARITH)
        {
            const uint8_t dest = pair.upDest;
            const __m128 sRaw = _mm_loadu_ps(vu.m_state.vf[pair.upFs]);
            const __m128 s = (Norm & 1u) ? vuNorm(sRaw) : sRaw;
            __m128 t;
            switch (pair.upSrc)
            {
            case FSRC_BC:
            {
                const float lane = vu.m_state.vf[pair.upFt][pair.upBc];
                t = _mm_set1_ps((Norm & 2u) ? normScalar(lane) : lane);
                break;
            }
            case FSRC_VT:
            {
                const __m128 tRaw = _mm_loadu_ps(vu.m_state.vf[pair.upFt]);
                t = (Norm & 2u) ? vuNorm(tRaw) : tRaw;
                break;
            }
            case FSRC_Q: t = _mm_set1_ps(vu.m_state.q); break;
            default: t = _mm_set1_ps(vu.m_state.i); break;
            }
            auto acc = [&]()
            {
                const __m128 aRaw = _mm_loadu_ps(vu.m_state.acc);
                return (Norm & 4u) ? vuNorm(aRaw) : aRaw;
            };
            __m128 result;
            bool fmac = true;
            switch (pair.upArith)
            {
            case FA_ADD: result = _mm_add_ps(s, t); break;
            case FA_SUB: result = _mm_sub_ps(s, t); break;
            case FA_MUL: result = _mm_mul_ps(s, t); break;
            case FA_MADD: result = _mm_add_ps(acc(), _mm_mul_ps(s, t)); break;
            case FA_MSUB: result = _mm_sub_ps(acc(), _mm_mul_ps(s, t)); break;
            case FA_MAX: result = _mm_max_ps(s, t); fmac = false; break;
            case FA_MINI: result = _mm_min_ps(s, t); fmac = false; break;
            case FA_OPMULA:
            case FA_OPMSUB:
            {
                const __m128 sYzx = _mm_shuffle_ps(s, s, _MM_SHUFFLE(3, 0, 2, 1));
                const __m128 tZxy = _mm_shuffle_ps(t, t, _MM_SHUFFLE(3, 1, 0, 2));
                __m128 product = _mm_mul_ps(sYzx, tZxy);
                if (pair.upArith == FA_OPMSUB)
                    product = _mm_sub_ps(acc(), product);
                result = _mm_and_ps(product, g_laneMasks.mask[0xE]);
                break;
            }
            default: result = _mm_setzero_ps(); break;
            }
            float *target = pair.upToAcc ? vu.m_state.acc : vu.m_state.vf[pair.upFd];
            if (fmac && (!vu.m_fastNeedMacStatus || pair.flagsDead))
            {
                const __m128 maxValue = _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF));
                const __m128 minValue = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0xFF7FFFFFu)));
                result = _mm_min_ps(_mm_max_ps(result, minValue), maxValue);
                storeMasked(target, result, dest);
                return;
            }
            if (fmac)
            {
                if (dest == 0u)
                    return;
                uint32_t mac = 0u;
                uint32_t statusNibble = 0u;
                result = vuResult(result, dest, mac, statusNibble);
                storeMasked(target, result, dest);
                VU1Interpreter::FastFlagEntry entry;
                entry.issueCycle = vu.m_cycle;
                entry.readyCycle = vu.m_cycle + VU1Interpreter::kFmacLatency;
                entry.mac = mac;
                entry.status = statusNibble;
                entry.writes = 1u;
                vu.fastPushFlagEntry(entry);
            }
            else
            {
                storeMasked(target, result, dest);
            }
        }
        else if constexpr (Kind == FU_ABS && !(Norm & 1u))
        {
            const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
            storeMasked(vu.m_state.vf[pair.upFt], _mm_and_ps(_mm_loadu_ps(vu.m_state.vf[pair.upFs]), absMask), pair.upDest);
        }
        else
        {
            vu.fastExecUpperKind<Kind>(pair);
        }
    }

    // Pipeline kinds of FastPair::loPipeline, for the generator.
    static constexpr uint8_t kPipeNone = VU1Interpreter::PipelineNone;
    static constexpr uint8_t kPipeFdiv = VU1Interpreter::PipelineFdiv;
    static constexpr uint8_t kPipeEfu = VU1Interpreter::PipelineEfu;

    // ---- Static tier (compiled blocks whose stalls were worked out by the generator) ----
    // The upper/lower execution of pairCoreWith without the stall and ready-time bookkeeping.
    // Branch lower kinds are left to the caller (Lower = false).
    template <typename Exec, bool Lower>
    static VU_FAST_INLINE void execStatic(VU1Interpreter &vu, const Pair &pair, uint8_t *vuData, uint32_t dataSize)
    {
        using namespace vufast;
        if (pair.iBit)
        {
            Exec::upper(vu, pair);
            vu.m_state.i = normScalar(pair.immediate);
        }
        else if (pair.shadowReg != 0u && Lower)
        {
            float oldValue[4];
            float upperValue[4];
            std::memcpy(oldValue, vu.m_state.vf[pair.shadowReg], sizeof(oldValue));
            Exec::upper(vu, pair);
            std::memcpy(upperValue, vu.m_state.vf[pair.shadowReg], sizeof(upperValue));
            std::memcpy(vu.m_state.vf[pair.shadowReg], oldValue, sizeof(oldValue));
            Exec::lower(vu, pair, vuData, dataSize);
            std::memcpy(vu.m_state.vf[pair.shadowReg], upperValue, sizeof(upperValue));
        }
        else
        {
            Exec::upper(vu, pair);
            if constexpr (Lower)
            {
                if (pair.loKind != FL_NOP)
                    Exec::lower(vu, pair, vuData, dataSize);
            }
        }
        if (pair.writesVf0)
        {
            vu.m_state.vf[0][0] = 0.0f;
            vu.m_state.vf[0][1] = 0.0f;
            vu.m_state.vf[0][2] = 0.0f;
            vu.m_state.vf[0][3] = 1.0f;
        }
    }
    static VU_FAST_INLINE void setCycle(VU1Interpreter &vu, uint64_t value) { vu.m_cycle = value; }
    // Latest ready time ever written to m_vfReady / m_viReady since the last resetScheduler, so a
    // "nothing in flight" test does not scan 144 entries. Kept in m_accReady[3], which only the
    // accurate core uses otherwise (it never runs the fast core's paths).
    static VU_FAST_INLINE uint64_t &pendingUntil(VU1Interpreter &vu) { return vu.m_accReady[3]; }
    static VU_FAST_INLINE void setVfReady(VU1Interpreter &vu, uint32_t reg, uint32_t lane, uint64_t at)
    {
        vu.m_vfReady[reg][lane] = at;
        if (at > pendingUntil(vu))
            pendingUntil(vu) = at;
    }
    static VU_FAST_INLINE void setViReady(VU1Interpreter &vu, uint32_t reg, uint64_t at)
    {
        vu.m_viReady[reg] = at;
        if (at > pendingUntil(vu))
            pendingUntil(vu) = at;
    }
    static VU_FAST_INLINE int32_t vi(const VU1Interpreter &vu, uint32_t reg) { return vu.m_state.vi[reg]; }
    static VU_FAST_INLINE void setVi(VU1Interpreter &vu, uint32_t reg, int32_t value)
    {
        if (reg != 0u)
            vu.m_state.vi[reg] = value;
    }
    // VU1Interpreter::readBranchVi / recordViWriteForBranch, inline.
    static VU_FAST_INLINE int32_t branchVi(const VU1Interpreter &vu, uint32_t reg)
    {
        if (reg == 0u)
            return 0;
        if (vu.m_viBranchBackupValid && vu.m_viBranchBackupReg == reg)
            return vu.m_viBranchBackupValue;
        return vu.m_state.vi[reg];
    }
    static VU_FAST_INLINE void clearBranchBackup(VU1Interpreter &vu) { vu.m_viBranchBackupValid = false; }
    static VU_FAST_INLINE void recordBranchBackup(VU1Interpreter &vu, uint32_t reg, int32_t oldValue)
    {
        if (reg == 0u)
            return;
        vu.m_viBranchBackupValue = oldValue;
        vu.m_viBranchBackupReg = static_cast<uint8_t>(reg);
        vu.m_viBranchBackupValid = true;
    }
    // Nothing in flight that a stall could wait for, and no branch, E bit or halt pending: the
    // static tier's blocks assume exactly this at their clean entry points.
    static bool quiescent(const VU1Interpreter &vu)
    {
        const uint64_t now = vu.m_cycle;
        if (vu.m_state.branchPending | vu.m_state.ebit | vu.m_state.haltAfterDelaySlot)
            return false;
        if (vu.m_fdiv.valid && vu.m_fdiv.readyCycle > now)
            return false;
        if (vu.m_efuResourceReady > now)
            return false;
        for (const VU1Interpreter::ScalarPipelineEntry &entry : vu.m_efu)
            if (entry.valid && entry.readyCycle > now)
                return false;
        return vu.m_accReady[3] <= now;
    }

    // A branch, E bit or halt is pending: the next pair needs postPair's handling.
    static VU_FAST_INLINE bool pendingControl(const VU1Interpreter &vu)
    {
        return vu.m_state.branchPending | vu.m_state.ebit | vu.m_state.haltAfterDelaySlot;
    }
    static VU_FAST_INLINE uint32_t pc(const VU1Interpreter &vu) { return vu.m_state.pc; }
    static VU_FAST_INLINE void setPc(VU1Interpreter &vu, uint32_t value) { vu.m_state.pc = value; }
    static VU_FAST_INLINE uint64_t cycle(const VU1Interpreter &vu) { return vu.m_cycle; }
    static VU_FAST_INLINE void tick(VU1Interpreter &vu) { ++vu.m_cycle; }
    static void decode(const VU1Interpreter &vu, Pair &out, const uint8_t *code, uint32_t pc) { vu.fastDecode(out, code, pc); }
    // Tools (replay harness): collect PATH1 output, optionally without submitting it.
    // Replay tools: a clean pipeline (as after an E-bit end) before resuming a recorded run.
    static void resetPipeline(VU1Interpreter &vu) { vu.resetScheduler(); }
    static void setGifCapture(VU1Interpreter &vu, std::vector<uint8_t> *capture, bool submit)
    {
        vu.m_gifCapture = capture;
        vu.m_gifSubmit = submit;
    }

    // Compiled program for this code image, or nullptr (vu1_aot_programs.cpp).
    static Fn lookup(const uint8_t *code, uint32_t codeSize);
    static bool enabled() { return enabledFlag(); }
    static bool &enabledFlag(); // PS2X_VU1_AOT=0 clears it; tools may change it
};

// One compiled code image (tables in vu1_aot_programs.cpp).
struct Vu1AotProgram
{
    uint64_t hash;
    Vu1Aot::Fn fn;
    uint32_t pairs;   // compiled pairs (for the log)
    uint32_t entries; // entry points it was generated from
};

// Static tier: the same, with operand normalization only where the generator needs it.
template <const Vu1Aot::Pair &P, uint8_t Norm>
struct Vu1AotStaticExecN
{
    static VU_FAST_INLINE void upper(VU1Interpreter &vu, const Vu1Aot::Pair &pair) { Vu1Aot::upperStatic<P.upKind, Norm>(vu, pair); }
    static VU_FAST_INLINE void lower(VU1Interpreter &vu, const Vu1Aot::Pair &pair, uint8_t *vuData, uint32_t dataSize)
    {
        Vu1Aot::lowerKind<P.loKind>(vu, pair, vuData, dataSize);
    }
};

// Execution policy for a compile-time constant pair: only its own upper and lower kinds are
// instantiated, so the compiled programs stay quick to build.
template <const Vu1Aot::Pair &P>
struct Vu1AotStaticExec
{
    static VU_FAST_INLINE void upper(VU1Interpreter &vu, const Vu1Aot::Pair &pair) { Vu1Aot::upperKind<P.upKind>(vu, pair); }
    static VU_FAST_INLINE void lower(VU1Interpreter &vu, const Vu1Aot::Pair &pair, uint8_t *vuData, uint32_t dataSize)
    {
        Vu1Aot::lowerKind<P.loKind>(vu, pair, vuData, dataSize);
    }
};
