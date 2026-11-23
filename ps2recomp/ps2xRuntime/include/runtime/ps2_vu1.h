#ifndef PS2_VU1_H
#define PS2_VU1_H

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class GS;
class PS2Memory;

struct VU1State
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t r;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    uint64_t cycles;
    bool ebit;
    bool haltAfterDelaySlot;
    bool dBitEnabled;
    bool tBitEnabled;
    bool stoppedByD;
    bool stoppedByT;
    uint32_t top;  // VIF TOP visible to XTOP
    uint32_t itop; // VIF ITOP visible to XITOP

    bool branchPending;
    uint32_t branchTarget;
    uint32_t branchDelay;
};

class VU1Interpreter
{
public:
    enum class Unit : uint8_t
    {
        VU0,
        VU1
    };

    explicit VU1Interpreter(Unit unit = Unit::VU1);

    void reset();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    // PS2X_VU1_CAPTURE: run() plus a recording of the run (execute and resume).
    void runCaptured(uint8_t *vuCode, uint32_t codeSize, uint8_t *vuData, uint32_t dataSize,
                     GS &gs, PS2Memory *memory, uint32_t startPC, uint32_t top, uint32_t itop,
                     uint32_t maxCycles, bool isResume);

    // True when the fast core (sequential execution with modelled Q/P/flag latency) runs
    // microprograms; PS2X_VU_CORE=accurate selects the cycle-accurate pipeline model instead.
    static bool fastCoreEnabled();

    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }

    // False when the last run stopped because its cycle budget ran out, not at an E-bit or halt.
    bool lastRunEnded() const { return m_lastRunEnded; }

private:
    friend struct Vu1Aot; // compiled VU1 microprograms (src/lib/vu/ps2_vu_fast_inl.h)

    enum Pipeline : uint8_t
    {
        PipelineNone = 0,
        PipelineFmac,
        PipelineLsu,
        PipelineFdiv,
        PipelineEfu,
        PipelineIalu,
        PipelineBranch,
        PipelineXgkick
    };

    struct VfAccess
    {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct InstructionUsage
    {
        std::array<VfAccess, 2> vfRead{};
        VfAccess vfWrite{};
        uint8_t vfReadCount = 0;
        uint16_t viRead = 0;
        uint16_t viWrite = 0;
        uint8_t accRead = 0;
        uint8_t accWrite = 0;
        uint8_t latency = 0;
        uint8_t vfLatency = 0;
        uint8_t viLatency = 0;
        Pipeline pipeline = PipelineNone;
        bool waitQ = false;
        bool waitP = false;
        bool readsClip = false;
        bool writesClip = false;
        bool delaysNextBranchRead = false;
        bool reserved = false;
    };

    struct DecodedInstructionPair
    {
        uint32_t lower = 0;
        uint32_t upper = 0;
        InstructionUsage lowerUsage{};
        InstructionUsage upperUsage{};
        bool iBit = false;
        bool eBit = false;
        bool mBit = false;
        bool dBit = false;
        bool tBit = false;
        uint8_t upperVfShadowReg = 0;
        uint8_t suppressedLowerVf = 0;
    };

    struct FlagPipelineEntry
    {
        uint64_t readyCycle = 0;
        uint64_t issueCycle = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t extraSticky = 0;
        uint32_t clip = 0;
        bool valid = false;
        bool writesMac = false;
        bool writesStatus = false;
        bool writesSticky = false;
        bool writesClip = false;
    };

    struct ScalarPipelineEntry
    {
        uint64_t readyCycle = 0;
        float value = 0.0f;
        uint32_t statusDi = 0;
        bool valid = false;
    };

    struct PendingStore
    {
        uint64_t readyCycle = 0;
        uint32_t address = 0;
        std::array<uint32_t, 4> words{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingVfWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t reg = 0;
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingViWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        int32_t value = 0;
        uint8_t reg = 0;
        bool valid = false;
    };

    struct PendingAccWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct XgkickPipeline
    {
        static constexpr uint32_t kBufferSize = 0x10000u;
        std::array<uint8_t, kBufferSize> packet{};
        uint32_t sourceAddress = 0;
        uint32_t totalBytes = 0;
        uint32_t copiedBytes = 0;
        uint32_t currentTagEnd = 0;
        uint32_t cycleCredit = 0;
        uint64_t issueCycle = 0;
        bool active = false;
        bool currentTagEop = false;
    };

    static constexpr uint32_t kFmacLatency = 4u;
    static constexpr uint32_t kAccForwardLatency = 1u;
    static constexpr uint32_t kMaxFlagEntries = 8u;
    static constexpr uint32_t kMaxPendingStores = 8u;
    static constexpr uint32_t kMaxPendingVfWrites = 16u;
    static constexpr uint32_t kMaxPendingViWrites = 8u;
    static constexpr uint32_t kMaxPendingAccWrites = 8u;
    static constexpr uint32_t kMaxDecodedPairs = 0x4000u / 8u;

    Unit m_unit;
    VU1State m_state;
    std::array<DecodedInstructionPair, kMaxDecodedPairs> m_decodedCodeCache{};
    const uint8_t *m_cachedVuCode = nullptr;
    const PS2Memory *m_cachedMemory = nullptr;
    uint32_t m_cachedCodeSize = 0;
    uint64_t m_cachedCodeGeneration = 0;
    bool m_decodedCodeCacheValid = false;

    std::array<FlagPipelineEntry, kMaxFlagEntries> m_flagPipeline{};
    ScalarPipelineEntry m_fdiv{};
    std::array<ScalarPipelineEntry, 2> m_efu{};
    std::array<PendingStore, kMaxPendingStores> m_storePipeline{};
    std::array<PendingVfWrite, kMaxPendingVfWrites> m_vfWritePipeline{};
    std::array<PendingViWrite, kMaxPendingViWrites> m_viWritePipeline{};
    std::array<PendingAccWrite, kMaxPendingAccWrites> m_accWritePipeline{};
    XgkickPipeline m_xgkick{};
    std::array<std::array<uint64_t, 4>, 32> m_vfReady{};
    std::array<uint64_t, 16> m_viReady{};
    std::array<uint64_t, 4> m_accReady{};
    std::array<std::array<uint64_t, 4>, 32> m_vfLatestWrite{};
    std::array<uint64_t, 16> m_viLatestWrite{};
    std::array<uint64_t, 4> m_accLatestWrite{};

    uint64_t m_cycle = 0;
    uint64_t m_nextWriteSequence = 0;
    uint64_t m_efuResourceReady = 0;
    uint32_t m_workingClip = 0;
    uint32_t m_currentUpperInstruction = 0;
    int32_t m_viBranchBackupValue = 0;
    uint8_t m_viBranchBackupReg = 0;
    bool m_viBranchBackupValid = false;
    uint8_t *m_activeVuData = nullptr;
    uint32_t m_activeVuDataSize = 0;
    GS *m_activeGs = nullptr;
    PS2Memory *m_activeMemory = nullptr;
    bool m_stopRequested = false;
    bool m_lastRunEnded = true;
    bool m_pendingHaltD = false;
    bool m_pendingHaltT = false;

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);
    void runAccurate(uint8_t *vuCode, uint32_t codeSize,
                     uint8_t *vuData, uint32_t dataSize,
                     GS &gs, PS2Memory *memory, uint32_t maxCycles);

    // ---- Fast core (ps2_vu_fast.cpp) ----
    struct FastPair
    {
        uint64_t generation = 0; // code generation this entry was decoded for (0 = never)
        uint32_t upper = 0;
        uint32_t lower = 0;
        float immediate = 0.0f;
        uint8_t upKind = 0;   // FastUpperKind
        uint8_t upArith = 0;  // FastArith
        uint8_t upSrc = 0;    // FastSrc
        uint8_t upToAcc = 0;
        uint8_t upDest = 0;
        uint8_t upFs = 0;
        uint8_t upFt = 0;
        uint8_t upFd = 0;
        uint8_t upBc = 0;
        uint8_t upScale = 0;
        uint8_t loKind = 0;   // FastLowerKind
        uint8_t loDest = 0;
        uint8_t loFs = 0;     // VF fs / VI is
        uint8_t loFt = 0;     // VF ft / VI it
        uint8_t loFd = 0;     // VI id
        uint8_t loFsf = 0;
        uint8_t loFtf = 0;
        int16_t loImm = 0;
        uint32_t loImm24 = 0;
        uint8_t readCount = 0;
        uint8_t readReg[4]{};
        uint8_t readLanes[4]{};
        uint16_t viRead = 0;
        uint8_t upWriteReg = 0;
        uint8_t upWriteLanes = 0;
        uint8_t upLatency = 0;
        uint8_t loWriteReg = 0;
        uint8_t loWriteLanes = 0;
        uint8_t loVfLatency = 0;
        uint8_t viWriteReg = 0;
        uint8_t viLatency = 0;
        uint8_t loPipeline = 0;
        uint8_t loLatency = 0;
        uint8_t shadowReg = 0;
        bool iBit = false;
        bool eBit = false;
        bool dBit = false;
        bool tBit = false;
        bool waitQ = false;
        bool waitP = false;
        bool delaysNextBranchRead = false;
        bool reservedUpper = false;
        bool reservedLower = false;
        bool writesVf0 = true; // the pair can write VF0 (which must then be restored to 0,0,0,1)
        bool flagsDead = false; // compiled code: no instruction can observe this FMAC's MAC/status flags
    };
    struct FastFlagEntry
    {
        uint64_t issueCycle = 0;
        uint64_t readyCycle = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t clip = 0;
        uint8_t writes = 0; // 1 = MAC+status, 2 = sticky (FSSET), 4 = clip
    };
    static constexpr uint32_t kFastFlagRing = 16u;
    std::unique_ptr<FastPair[]> m_fastPairs;
    std::array<FastFlagEntry, kFastFlagRing> m_fastFlags{};
    uint32_t m_fastFlagHead = 0;
    uint32_t m_fastFlagCount = 0;
    // MAC/status flags are only computed when the loaded VU1 code reads them (FM*/FS* ops).
    uint64_t m_fastFlagScanGeneration = ~0ull;
    bool m_fastNeedMacStatus = true;
    void *m_aotFn = nullptr;            // Vu1Aot::Fn for the loaded code image
    uint64_t m_aotGeneration = ~0ull;    // VU1 code generation m_aotFn was resolved for
    std::vector<uint8_t> *m_gifCapture = nullptr; // compare mode: collect PATH1 packets
    bool m_gifSubmit = true;
    std::unique_ptr<VU1Interpreter> m_compareShadow;
    std::vector<uint8_t> m_compareData;

    void runFast(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory, uint32_t maxCycles);
    void runCompare(uint8_t *vuCode, uint32_t codeSize,
                    uint8_t *vuData, uint32_t dataSize,
                    GS &gs, PS2Memory *memory, uint32_t maxCycles);
    void fastResetPipelines();
    const FastPair &fastPairFor(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc);
    void fastDecode(FastPair &out, const uint8_t *vuCode, uint32_t pc) const;
    void fastExecUpper(const FastPair &pair);
    void fastExecLower(const FastPair &pair, uint8_t *vuData, uint32_t dataSize);
    template <uint8_t Kind>
    void fastExecUpperKind(const FastPair &pair);
    template <uint8_t Kind>
    void fastExecLowerKind(const FastPair &pair, uint8_t *vuData, uint32_t dataSize);
    void fastXgkick(uint32_t qwordAddress, const uint8_t *vuData, uint32_t dataSize);
    void fastCommit();
    void fastFlushAll();
    void fastPushFlagEntry(const FastFlagEntry &entry);
    void emitPath1Packet(const uint8_t *data, uint32_t size);

    InstructionUsage decodeUpperUsage(uint32_t upper) const;
    InstructionUsage decodeLowerUsage(uint32_t lower) const;
    static void addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static void addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static uint8_t vfReadLanes(const InstructionUsage &usage, uint8_t reg);
    DecodedInstructionPair decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const;
    DecodedInstructionPair getDecodedInstructionPairForPc(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc);
    void rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory, uint64_t generation);

    void execUpper(uint32_t instr);
    void execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);

    void applyDest(float *dst, const float *result, uint8_t dest);
    void applyDestAcc(const float *result, uint8_t dest);
    void applyFmacDest(float *dst, float *result, uint8_t dest);
    void applyFmacDestAcc(float *result, uint8_t dest);
    void normalizeFmacResult(float *result, uint8_t dest, uint8_t laneFlags[4]);
    bool calculateFmacExactResult(uint32_t component, long double &result) const;
    uint8_t normalizeFmacExactResult(float &value, long double exactResult) const;
    uint32_t calculateFmacProductSticky(uint8_t dest) const;
    void updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky);
    void queueFsset(uint16_t immediate);
    void queueClip(uint32_t clip);
    void queueFcset(uint32_t clip);
    void queueQ(float value, uint32_t latency, uint32_t statusDi);
    void queueP(float value, uint32_t latency);
    void queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    void queueVfWrite(uint8_t reg, uint8_t laneMask, const float value[4], uint32_t latency);
    void queueViWrite(uint8_t reg, int32_t value, uint32_t latency);
    void queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency);
    void startXgkick(uint32_t qwordAddress);

    void resetScheduler();
    void commitReadyPipelines();
    void advanceOneCycle();
    void advanceTo(uint64_t targetCycle);
    void flushPipelines();
    void progressXgkick();
    void finishXgkick();
    uint64_t calculatePairReadyCycle(const DecodedInstructionPair &decoded) const;
    void markPairWrites(const DecodedInstructionPair &decoded);
    bool pipelinesPending() const;

    float normalizeOperand(float value) const;
    float normalizeResult(float value, uint32_t &laneFlags) const;
    uint32_t microAddressMask() const;
    int32_t readBranchVi(uint8_t reg) const;
    void recordViWriteForBranch(uint8_t reg, int32_t oldValue);
    void reportReservedInstruction(bool upper, uint32_t instruction);
    float broadcast(const float *vf, uint8_t bc);
};

#endif
