// Static tier of the VU1 AOT generator (included by vu1aot_gen.cpp).
//
// The dynamic tier (one labelled block per pair, Vu1Aot::pairCoreWith) works out every stall at
// run time: ready times per register lane, the flag queue, FDIV/EFU state, a cycle count and a
// budget check per pair. The static tier works those stalls out here instead. A "version" is a
// basic block compiled for one pipeline state at its start (how many cycles each VF lane / VI
// register / FDIV / EFU still needs); simulating the block from that state gives every pair's
// issue cycle and the state at its end, which selects the version of each successor. Inside a
// version a pair is just its upper and lower operation; the cycle count, the pair count and the
// budget are handled once per block.
//
// What still happens at run time, through the same structures the dynamic tier uses:
//   - MAC/status/clip flags (pushed by FMAC/CLIP/FCSET/FSSET with absolute cycle stamps, and
//     committed before any instruction that reads them, pushes new ones, or uses Q/P),
//   - Q and P values (queued with absolute ready cycles, committed before they are read),
//   - the VI value a branch sees right after a VI write (backup register),
// so the static tier can hand over to the dynamic tier at any block boundary: it writes the
// still-pending ready times into m_vfReady / m_viReady ("materialize") and continues there.
// The dynamic tier comes back at the next block start where nothing is in flight.
//
// Entry from the dynamic tier or from runFast is only into "clean" versions (nothing in flight),
// which exist for the recorded entry points, MSCNT resume points and the usual JR targets.

namespace
{
    struct SState
    {
        std::array<uint8_t, 128> vf{}; // cycles until the lane (reg * 4 + lane, lane 0 = x) is ready
        std::array<uint8_t, 16> vi{};
        uint8_t fdiv = 0;              // cycles until the FDIV result is ready
        uint8_t efuRes = 0;            // cycles until the EFU accepts another instruction
        std::array<uint8_t, 2> efu{};  // m_efu slots: 0 = free, otherwise 1 + cycles until ready
        uint16_t viKnown = 0;          // constant VI values (JR/JALR source registers only)
        std::array<int32_t, 16> viValue{};

        auto key() const { return std::tie(vf, vi, fdiv, efuRes, efu, viKnown, viValue); }
        bool operator<(const SState &o) const { return key() < o.key(); }
        bool operator==(const SState &o) const { return key() == o.key(); }
        bool clean() const { return *this == SState{}; }
    };

    enum class SExit : uint8_t
    {
        Next,      // fall through into the block at succPc[0]
        Branch,    // B/BAL: succPc[0]
        Cond,      // IBxx: taken -> succPc[0], not taken -> succPc[1]
        JrStatic,  // JR/JALR with a known target: succPc[0]
        JrDynamic, // JR/JALR, target known only at run time
        Ended,     // E bit: program ends, MSCNT resumes at succPc[0]
        Dynamic,   // the pair at succPc[0] is left to the dynamic tier
    };

    struct SPair
    {
        uint32_t pc = 0;
        uint32_t issue = 0;       // issue cycle relative to the block start
        bool setCycle = false;    // m_cycle must be exact for this pair (flags, FDIV/EFU, XGKICK, commit)
        bool commit = false;      // commit due flags / Q / P first
        bool clearBackup = false; // the VI branch backup recorded by the previous pair expires
        bool recordBackup = false;
        bool branch = false;      // the lower op is a branch: generated separately
    };

    struct SVersion
    {
        int id = 0;
        uint32_t pc = 0;
        SState entry;
        std::vector<SPair> pairs;
        uint32_t end = 0;         // cycles from block start to the next block's start
        SExit exit = SExit::Dynamic;
        uint32_t succPc[2] = {0, 0};
        SState succState[2];
        int succ[2] = {-1, -1};   // successor version ids (-1: hand over to the dynamic tier)
        uint32_t branchPc = 0;    // pc of the branch pair (Cond/Jr exits)
    };

    bool sIsFdiv(uint8_t kind) { return kind == FL_DIV || kind == FL_SQRT || kind == FL_RSQRT; }
    uint32_t sFdivLatency(uint8_t kind) { return kind == FL_RSQRT ? 13u : 7u; }
    bool sIsEfu(uint8_t kind)
    {
        switch (kind)
        {
        case FL_ESADD: case FL_ERSADD: case FL_ELENG: case FL_ERLENG: case FL_EATANXY: case FL_EATANXZ: case FL_ESUM:
        case FL_ERSQRT: case FL_ESQRT: case FL_ESIN: case FL_ERCPR: case FL_EATAN: case FL_EEXP:
            return true;
        default:
            return false;
        }
    }
    uint32_t sEfuLatency(uint8_t kind)
    {
        switch (kind)
        {
        case FL_ESADD: return 11u;
        case FL_ERSADD: return 18u;
        case FL_ELENG: return 18u;
        case FL_ERLENG: return 24u;
        case FL_EATANXY: case FL_EATANXZ: case FL_EATAN: return 54u;
        case FL_ESUM: return 12u;
        case FL_ERSQRT: return 18u;
        case FL_ESQRT: return 12u;
        case FL_ESIN: return 29u;
        case FL_ERCPR: return 12u;
        case FL_EEXP: return 44u;
        default: return 0u;
        }
    }
    bool sReadsFlags(uint8_t kind)
    {
        switch (kind)
        {
        case FL_FCEQ: case FL_FCAND: case FL_FCOR: case FL_FCGET: case FL_FSEQ: case FL_FSAND: case FL_FSOR:
        case FL_FMEQ: case FL_FMAND: case FL_FMOR:
            return true;
        default:
            return false;
        }
    }

    class StaticTier
    {
    public:
        StaticTier(const std::function<const Vu1Aot::Pair &(uint32_t)> &decode, const std::map<uint32_t, Vu1Aot::Pair> &pruned,
                   const std::set<uint32_t> &labels, const std::set<uint32_t> &blockStarts, const std::set<uint32_t> &cleanStarts,
                   bool needMacStatus)
            : m_decode(decode), m_pruned(pruned), m_labels(labels), m_blockStarts(blockStarts), m_needMacStatus(needMacStatus)
        {
            // VI registers whose value a JR/JALR jumps to: constants are tracked for these only.
            for (uint32_t pc : labels)
            {
                const Vu1Aot::Pair &p = decode(pc);
                if (!p.iBit && !p.reservedLower && (p.loKind == FL_JR || p.loKind == FL_JALR))
                    m_jrRegs |= static_cast<uint16_t>(1u << (p.loFs & 0xFu));
            }
            m_jrRegs &= static_cast<uint16_t>(~1u);
            for (uint32_t pc : cleanStarts)
            {
                const int id = versionFor(pc, SState{});
                if (id >= 0)
                    m_clean[pc] = id;
            }
            // Simulate versions as they are discovered (successors create more).
            for (size_t index = 0; index < m_versions.size(); ++index)
                simulate(m_versions[index]);
            // Clean entries whose first pair the static tier cannot run stay dynamic.
            for (auto it = m_clean.begin(); it != m_clean.end();)
                it = m_versions[it->second].pairs.empty() ? m_clean.erase(it) : std::next(it);
        }

        const std::vector<SVersion> &versions() const { return m_versions; }
        const std::map<uint32_t, int> &clean() const { return m_clean; }
        uint32_t overflow() const { return m_overflow; }

    private:
        static constexpr size_t kMaxVersionsPerPc = 8;

        const std::function<const Vu1Aot::Pair &(uint32_t)> &m_decode;
        const std::map<uint32_t, Vu1Aot::Pair> &m_pruned;
        const std::set<uint32_t> &m_labels;
        const std::set<uint32_t> &m_blockStarts;
        bool m_needMacStatus;
        uint16_t m_jrRegs = 0;
        std::vector<SVersion> m_versions;
        std::map<std::pair<uint32_t, SState>, int> m_index;
        std::map<uint32_t, size_t> m_perPc;
        std::map<uint32_t, int> m_clean;
        uint32_t m_overflow = 0;
    public:
        std::map<uint32_t, uint32_t> m_overflowAt;
    private:

        // Id of the version of the block at pc for this start state; -1 when the static tier hands
        // over to the dynamic tier instead (unknown code, or too many versions of this block).
        int versionFor(uint32_t pc, const SState &state)
        {
            if (!m_labels.count(pc))
                return -1;
            const auto key = std::make_pair(pc, state);
            auto it = m_index.find(key);
            if (it != m_index.end())
                return it->second;
            size_t &count = m_perPc[pc];
            if (count >= kMaxVersionsPerPc && !state.clean())
            {
                ++m_overflow;
                ++m_overflowAt[pc];
                return -1;
            }
            ++count;
            SVersion v;
            v.id = static_cast<int>(m_versions.size());
            v.pc = pc;
            v.entry = state;
            m_versions.push_back(v);
            m_index.emplace(key, v.id);
            return v.id;
        }

        bool unsupported(const Vu1Aot::Pair &p) const
        {
            return p.reservedUpper || p.reservedLower || p.dBit || p.tBit;
        }

        bool isBranchPair(const Vu1Aot::Pair &p) const { return !p.iBit && !p.reservedLower && isBranchKind(p.loKind); }

        bool pushesFlags(const Vu1Aot::Pair &p, const Vu1Aot::Pair &k) const
        {
            if (p.reservedUpper)
                return false;
            if (p.upKind == FU_CLIP)
                return true;
            if (p.upKind == FU_ARITH && p.upArith != FA_MAX && p.upArith != FA_MINI && p.upDest != 0u && m_needMacStatus && !k.flagsDead)
                return true;
            return !p.iBit && (p.loKind == FL_FCSET || p.loKind == FL_FSSET);
        }

        // Constant VI values through one pair's VI write (tracked registers only).
        void trackVi(const Vu1Aot::Pair &p, uint32_t pc, SState &s) const
        {
            if (p.iBit || p.viWriteReg == 0u)
                return;
            const uint32_t reg = p.viWriteReg;
            const uint16_t bit = static_cast<uint16_t>(1u << reg);
            if (!(m_jrRegs & bit))
                return;
            auto known = [&](uint32_t r, int32_t &value) -> bool
            {
                r &= 0xFu;
                if (r == 0u)
                {
                    value = 0;
                    return true;
                }
                if (s.viKnown & (1u << r))
                {
                    value = s.viValue[r];
                    return true;
                }
                return false;
            };
            int32_t a = 0, b = 0, result = 0;
            bool ok = false;
            switch (p.loKind)
            {
            case FL_BAL: case FL_JALR: result = static_cast<int32_t>((pc + 16u) / 8u); ok = true; break;
            case FL_IADDIU: case FL_IADDI: ok = known(p.loFs, a); result = static_cast<int16_t>(a + p.loImm); break;
            case FL_ISUBIU: ok = known(p.loFs, a); result = static_cast<int16_t>(a - p.loImm); break;
            case FL_IADD: ok = known(p.loFs, a) && known(p.loFt, b); result = static_cast<int16_t>(a + b); break;
            case FL_ISUB: ok = known(p.loFs, a) && known(p.loFt, b); result = static_cast<int16_t>(a - b); break;
            case FL_IAND: ok = known(p.loFs, a) && known(p.loFt, b); result = a & b; break;
            case FL_IOR: ok = known(p.loFs, a) && known(p.loFt, b); result = a | b; break;
            default: break;
            }
            if (ok)
            {
                s.viKnown = static_cast<uint16_t>(s.viKnown | bit);
                s.viValue[reg] = result;
            }
            else
            {
                s.viKnown = static_cast<uint16_t>(s.viKnown & ~bit);
                s.viValue[reg] = 0;
            }
        }

        // Absolute (block-relative) timing state while a block is simulated.
        struct Timing
        {
            std::array<int, 128> vf{};
            std::array<int, 16> vi{};
            int fdiv = 0, efuRes = 0;
            std::array<int, 2> efu{}; // -1 free, otherwise ready cycle
        };

        static Timing toTiming(const SState &s)
        {
            Timing t;
            for (size_t i = 0; i < 128; ++i)
                t.vf[i] = s.vf[i];
            for (size_t i = 0; i < 16; ++i)
                t.vi[i] = s.vi[i];
            t.fdiv = s.fdiv;
            t.efuRes = s.efuRes;
            for (size_t i = 0; i < 2; ++i)
                t.efu[i] = s.efu[i] == 0u ? -1 : s.efu[i] - 1;
            return t;
        }

        // State at block-relative time `now` (the next pair's earliest issue cycle).
        static SState fromTiming(const Timing &t, int now, const SState &vi)
        {
            SState s;
            auto rem = [now](int at) -> uint8_t { return static_cast<uint8_t>(at > now ? std::min(at - now, 255) : 0); };
            for (size_t i = 0; i < 128; ++i)
                s.vf[i] = rem(t.vf[i]);
            for (size_t i = 0; i < 16; ++i)
                s.vi[i] = rem(t.vi[i]);
            s.fdiv = rem(t.fdiv);
            s.efuRes = rem(t.efuRes);
            for (size_t i = 0; i < 2; ++i)
                s.efu[i] = t.efu[i] < 0 ? 0u : static_cast<uint8_t>(1u + rem(t.efu[i]));
            s.viKnown = vi.viKnown;
            s.viValue = vi.viValue;
            return s;
        }

        // One pair: its issue cycle and effects on the timing state. Mirrors pairCoreWith.
        SPair step(uint32_t pc, Timing &t, int &now, SState &vi, bool first, bool &prevRecorded)
        {
            const Vu1Aot::Pair &p = m_decode(pc);
            const Vu1Aot::Pair &k = m_pruned.at(pc);
            SPair out;
            out.pc = pc;
            int issue = now;
            for (uint32_t r = 0; r < p.readCount; ++r)
            {
                const uint32_t reg = p.readReg[r];
                for (uint32_t lane = 0; lane < 4u; ++lane)
                    if (p.readLanes[r] & (8u >> lane))
                        issue = std::max(issue, t.vf[reg * 4u + lane]);
            }
            for (uint32_t reg = 0; reg < 16u; ++reg)
                if (p.viRead & (1u << reg))
                    issue = std::max(issue, t.vi[reg]);
            if (p.loPipeline != Vu1Aot::kPipeNone)
            {
                if (p.loPipeline == Vu1Aot::kPipeFdiv || p.waitQ)
                    issue = std::max(issue, t.fdiv);
                if (p.loPipeline == Vu1Aot::kPipeEfu && !p.waitP)
                    issue = std::max(issue, t.efuRes);
                if (p.waitP)
                    for (int ready : t.efu)
                        if (ready >= 0)
                            issue = std::max(issue, ready);
            }
            out.issue = static_cast<uint32_t>(issue);

            const uint8_t lo = p.iBit ? static_cast<uint8_t>(FL_NOP) : p.loKind;
            bool efuDue = false;
            for (int &ready : t.efu)
                if (ready >= 0 && ready <= issue)
                {
                    efuDue = true;
                    ready = -1; // committed at this pair
                }
            const bool readsQ = !p.reservedUpper && p.upKind == FU_ARITH && p.upSrc == FSRC_Q;
            const bool push = pushesFlags(p, k);
            out.commit = efuDue || push || readsQ || sReadsFlags(lo) || sIsFdiv(lo) || sIsEfu(lo) || lo == FL_MFP || lo == FL_WAITQ ||
                         lo == FL_WAITP;
            out.setCycle = out.commit || lo == FL_XGKICK;
            out.branch = isBranchPair(p);
            out.clearBackup = first || prevRecorded;
            out.recordBackup = !p.iBit && p.viWriteReg != 0u && p.delaysNextBranchRead;
            prevRecorded = out.recordBackup;

            if (p.upWriteReg != 0u)
                for (uint32_t lane = 0; lane < 4u; ++lane)
                    if (p.upWriteLanes & (8u >> lane))
                        t.vf[p.upWriteReg * 4u + lane] = issue + p.upLatency;
            if (p.loWriteReg != 0u)
                for (uint32_t lane = 0; lane < 4u; ++lane)
                    if (p.loWriteLanes & (8u >> lane))
                        t.vf[p.loWriteReg * 4u + lane] = issue + p.loVfLatency;
            if (p.viWriteReg != 0u)
                t.vi[p.viWriteReg] = issue + p.viLatency;
            if (sIsFdiv(lo))
                t.fdiv = issue + static_cast<int>(sFdivLatency(lo));
            if (sIsEfu(lo))
            {
                const int latency = static_cast<int>(sEfuLatency(lo));
                for (int &slot : t.efu)
                    if (slot < 0)
                    {
                        slot = issue + latency;
                        t.efuRes = issue + (latency > 0 ? latency - 1 : 0);
                        break;
                    }
            }
            trackVi(p, pc, vi);
            now = issue + 1;
            return out;
        }

        void simulate(SVersion &v)
        {
            Timing t = toTiming(v.entry);
            SState vi = v.entry; // viKnown / viValue
            int now = 0;
            bool prevRecorded = false;
            uint32_t pc = v.pc;
            auto finish = [&](SExit exit, uint32_t pc0, uint32_t pc1)
            {
                v.exit = exit;
                v.end = static_cast<uint32_t>(now);
                v.succPc[0] = pc0;
                v.succPc[1] = pc1;
                v.succState[0] = fromTiming(t, now, vi);
                v.succState[1] = v.succState[0];
            };
            for (;;)
            {
                const Vu1Aot::Pair &p = m_decode(pc);
                if (!m_labels.count(pc) || unsupported(p))
                {
                    finish(SExit::Dynamic, pc, 0);
                    break;
                }
                if (isBranchPair(p) || p.eBit)
                {
                    // The pair and its delay slot (which must be a plain pair) end the block.
                    const uint32_t slot = next(pc);
                    const Vu1Aot::Pair &d = m_decode(slot);
                    if (!m_labels.count(slot) || unsupported(d) || isBranchPair(d) || d.eBit || (isBranchPair(p) && p.eBit))
                    {
                        finish(SExit::Dynamic, pc, 0);
                        break;
                    }
                    // The JR target register as the branch reads it (the backup quirk is not
                    // modelled statically: a VI write right before the JR makes it dynamic).
                    const SState before = vi;
                    const bool firstPair = v.pairs.empty();
                    v.pairs.push_back(step(pc, t, now, vi, firstPair, prevRecorded));
                    v.pairs.push_back(step(slot, t, now, vi, false, prevRecorded));
                    v.branchPc = pc;
                    const uint32_t after = next(slot);
                    if (p.eBit)
                    {
                        finish(SExit::Ended, after, 0);
                        break;
                    }
                    switch (p.loKind)
                    {
                    case FL_B: case FL_BAL:
                        finish(SExit::Branch, staticTarget(p, pc), 0);
                        break;
                    case FL_JR: case FL_JALR:
                    {
                        const uint32_t reg = p.loFs & 0xFu;
                        const bool backupRisk = firstPair || (v.pairs.size() >= 3 && v.pairs[v.pairs.size() - 3].recordBackup);
                        if (!backupRisk && reg != 0u && (before.viKnown & (1u << reg)))
                            finish(SExit::JrStatic, (static_cast<uint32_t>(static_cast<uint16_t>(before.viValue[reg])) * 8u) & kPcMask, 0);
                        else
                            finish(SExit::JrDynamic, 0, 0);
                        break;
                    }
                    default:
                        finish(SExit::Cond, staticTarget(p, pc), after);
                        break;
                    }
                    break;
                }
                v.pairs.push_back(step(pc, t, now, vi, v.pairs.empty(), prevRecorded));
                const uint32_t nextPc = next(pc);
                if (nextPc == 0u || m_blockStarts.count(nextPc))
                {
                    finish(SExit::Next, nextPc, 0);
                    break;
                }
                pc = nextPc;
            }
            // Successor versions.
            switch (v.exit)
            {
            case SExit::Next: case SExit::Branch: case SExit::JrStatic:
                v.succ[0] = versionFor(v.succPc[0], v.succState[0]);
                break;
            case SExit::Cond:
                v.succ[0] = versionFor(v.succPc[0], v.succState[0]);
                v.succ[1] = versionFor(v.succPc[1], v.succState[1]);
                break;
            default:
                break;
            }
        }
    };

    std::string sPairName(uint64_t hash, uint32_t pc)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "kP_%016" PRIx64 "_%04x", hash, pc);
        return text;
    }

    // Stores the ready times still pending in `state` (relative to `when`) for the dynamic tier.
    void emitMaterialize(std::ostream &out, const SState &state, const std::string &when, const char *indent)
    {
        for (uint32_t i = 0; i < 128; ++i)
            if (state.vf[i] != 0u)
                out << indent << "Vu1Aot::setVfReady(vu, " << (i / 4u) << "u, " << (i % 4u) << "u, " << when << " + "
                    << unsigned(state.vf[i]) << "u);\n";
        for (uint32_t i = 0; i < 16; ++i)
            if (state.vi[i] != 0u)
                out << indent << "Vu1Aot::setViReady(vu, " << i << "u, " << when << " + " << unsigned(state.vi[i]) << "u);\n";
    }
}

namespace
{
    // Lanes whose value is known to be in PS2 range (no denormal, no Inf/NaN exponent): results of
    // FMAC/ITOF/ABS/MAX/MINI, MFP/RGET/RNEXT, VF0, and copies of such lanes. Bit reg * 4 + lane
    // (lane 0 = x); ACC is reg 32.
    using SClean = std::bitset<132>;

    SClean sVf0Clean()
    {
        SClean c;
        for (int lane = 0; lane < 4; ++lane)
            c.set(lane);
        return c;
    }

    bool sLanesClean(const SClean &c, uint32_t reg, uint8_t lanes)
    {
        for (uint32_t lane = 0; lane < 4u; ++lane)
            if ((lanes & (8u >> lane)) && !c.test(reg * 4u + lane))
                return false;
        return true;
    }

    void sSetLanes(SClean &c, uint32_t reg, uint8_t lanes, bool clean)
    {
        for (uint32_t lane = 0; lane < 4u; ++lane)
            if (lanes & (8u >> lane))
                c.set(reg * 4u + lane, clean);
    }

    // Norm bits for one pair given the lanes clean before it (see Vu1Aot::upperStatic).
    uint8_t sNormMask(const Vu1Aot::Pair &p, const SClean &c)
    {
        if (p.reservedUpper)
            return 7u;
        const bool op = p.upKind == FU_ARITH && (p.upArith == FA_OPMULA || p.upArith == FA_OPMSUB);
        const uint8_t lanes = op ? 0xEu : p.upDest;
        uint8_t norm = 0u;
        if (p.upKind == FU_ARITH)
        {
            if (!sLanesClean(c, p.upFs, lanes))
                norm |= 1u;
            if (p.upSrc == FSRC_VT && !sLanesClean(c, p.upFt, lanes))
                norm |= 2u;
            if (p.upSrc == FSRC_BC && !sLanesClean(c, p.upFt, static_cast<uint8_t>(8u >> p.upBc)))
                norm |= 2u;
            if ((p.upArith == FA_MADD || p.upArith == FA_MSUB || p.upArith == FA_OPMSUB) && !sLanesClean(c, 32u, lanes))
                norm |= 4u;
        }
        else if (p.upKind == FU_ABS || p.upKind == FU_FTOI)
        {
            if (!sLanesClean(c, p.upFs, p.upDest))
                norm |= 1u;
        }
        else
            norm = 7u;
        return norm;
    }

    // Cleanliness after one pair (upper result first, then the lower op; a shadowed register keeps
    // the upper result, as pairCoreWith arranges).
    void sCleanStep(const Vu1Aot::Pair &p, SClean &c)
    {
        if (p.reservedUpper || p.reservedLower)
        {
            c.reset();
            c |= sVf0Clean();
            return;
        }
        SClean upper = c;
        bool upperWrites = false;
        uint32_t upperReg = 0;
        switch (p.upKind)
        {
        case FU_ARITH:
            upperReg = p.upToAcc ? 32u : p.upFd;
            sSetLanes(upper, upperReg, (p.upArith == FA_OPMULA || p.upArith == FA_OPMSUB) ? 0xEu : p.upDest, true);
            upperWrites = true;
            break;
        case FU_ITOF: case FU_ABS:
            upperReg = p.upFt;
            sSetLanes(upper, upperReg, p.upDest, true);
            upperWrites = true;
            break;
        case FU_FTOI:
            upperReg = p.upFt;
            sSetLanes(upper, upperReg, p.upDest, false);
            upperWrites = true;
            break;
        default:
            break;
        }
        SClean lower = upper;
        if (!p.iBit)
        {
            const uint32_t ft = p.loFt, fs = p.loFs;
            switch (p.loKind)
            {
            case FL_LQ: case FL_LQI: case FL_LQD: case FL_MFIR:
                sSetLanes(lower, ft, p.loDest, false);
                break;
            case FL_MFP: case FL_RGET: case FL_RNEXT:
                sSetLanes(lower, ft, p.loDest, true);
                break;
            case FL_MOVE:
                for (uint32_t lane = 0; lane < 4u; ++lane)
                    if (p.loDest & (8u >> lane))
                        lower.set(ft * 4u + lane, upper.test(fs * 4u + lane));
                break;
            case FL_MR32:
                for (uint32_t lane = 0; lane < 4u; ++lane)
                    if (p.loDest & (8u >> lane))
                        lower.set(ft * 4u + lane, upper.test(fs * 4u + ((lane + 1u) & 3u)));
                break;
            default:
                break;
            }
        }
        if (p.shadowReg != 0u && upperWrites && upperReg == p.shadowReg)
            for (uint32_t lane = 0; lane < 4u; ++lane)
                lower.set(upperReg * 4u + lane, upper.test(upperReg * 4u + lane));
        c = lower;
        c |= sVf0Clean();
    }

    // Lanes clean at each version's start: a fixpoint over the static successors, starting from
    // "nothing known" at versions the run time can enter (the clean entries).
    std::vector<SClean> sCleanAtEntry(const std::vector<SVersion> &versions, const std::map<uint32_t, int> &cleanEntries,
                                      const std::map<uint32_t, Vu1Aot::Pair> &pruned)
    {
        SClean all;
        all.set();
        std::vector<SClean> entry(versions.size(), all);
        std::vector<char> runtime(versions.size(), 0);
        for (const auto &[pc, id] : cleanEntries)
            runtime[id] = 1;
        for (const SVersion &v : versions)
            if (v.entry.clean())
                runtime[v.id] = 1;
        for (size_t i = 0; i < versions.size(); ++i)
            if (runtime[i])
                entry[i] = sVf0Clean();
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (const SVersion &v : versions)
            {
                SClean c = entry[v.id];
                for (const SPair &sp : v.pairs)
                    sCleanStep(pruned.at(sp.pc), c);
                for (int s : v.succ)
                {
                    if (s < 0)
                        continue;
                    const SClean merged = entry[s] & c;
                    if (merged != entry[s])
                    {
                        entry[s] = merged;
                        changed = true;
                    }
                }
            }
        }
        return entry;
    }

    // Emits the static tier of one image and the image's entry function `name`.
    void emitStaticTier(std::ostream &out, uint64_t hash, const char *name, const StaticTier &tier,
                        const std::map<uint32_t, Vu1Aot::Pair> &pruned)
    {
        const std::vector<SVersion> &versions = tier.versions();
        // A version whose first pair the static tier cannot run is only a handover.
        auto stub = [&](int id) { return id < 0 || versions[id].pairs.empty(); };

        // The pairs used by static versions, as constants shared by all versions of a block.
        std::set<uint32_t> used;
        for (const SVersion &v : versions)
            for (const SPair &p : v.pairs)
                used.insert(p.pc);
        for (uint32_t pc : used)
            out << "static constexpr Vu1Aot::Pair " << sPairName(hash, pc) << " = " << pairInitializer(pruned.at(pc)) << ";\n";
        out << "\n";

        // Chunks: versions in depth-first order along their successors (a loop's versions end up
        // next to each other, so its back edge is a goto), ~384 pairs each.
        constexpr size_t kChunkPairs = 384;
        std::vector<int> order;
        {
            std::vector<char> seen(versions.size(), 0);
            for (const SVersion &root : versions)
            {
                if (seen[root.id] || stub(root.id))
                    continue;
                std::vector<int> stack{root.id};
                while (!stack.empty())
                {
                    const int id = stack.back();
                    stack.pop_back();
                    if (seen[id] || stub(id))
                        continue;
                    seen[id] = 1;
                    order.push_back(id);
                    for (int j = 1; j >= 0; --j)
                        if (versions[id].succ[j] >= 0 && !seen[versions[id].succ[j]])
                            stack.push_back(versions[id].succ[j]);
                }
            }
        }
        std::vector<int> chunkOf(versions.size(), -1);
        std::vector<std::vector<int>> chunks;
        size_t inChunk = 0;
        for (int id : order)
        {
            const SVersion &v = versions[id];
            if (chunks.empty() || inChunk + v.pairs.size() > kChunkPairs)
            {
                chunks.emplace_back();
                inChunk = 0;
            }
            chunks.back().push_back(v.id);
            chunkOf[v.id] = static_cast<int>(chunks.size() - 1);
            inChunk += v.pairs.size();
        }

        // Forward declarations.
        const std::string sig = "(VU1Interpreter &vu, uint8_t *__restrict vuData, uint32_t dataSize, uint64_t budgetEnd, uint64_t &pairs, int vid)";
        for (size_t c = 0; c < chunks.size(); ++c)
            out << "VU_AOT_BLOCK int " << name << "_s" << c << sig << ";\n";
        out << "int " << name << "_clean(uint32_t pc);\n\n";

        const std::vector<SClean> cleanEntry = sCleanAtEntry(versions, tier.clean(), pruned);
        uint32_t staticPairs = 0, handovers = 0, normSkipped = 0, normKept = 0;
        for (size_t c = 0; c < chunks.size(); ++c)
        {
            out << "//@@MOVE\nVU_AOT_BLOCK int " << name << "_s" << c << sig << "\n{\n    switch (vid)\n    {\n";
            for (int id : chunks[c])
                out << "    case " << id << ": goto S_" << id << ";\n";
            out << "    default: return vid;\n    }\n";
            for (int id : chunks[c])
            {
                const SVersion &v = versions[id];
                char pcText[16];
                std::snprintf(pcText, sizeof(pcText), "0x%04xu", v.pc);
                SClean cleanNow = cleanEntry[id];
                out << "S_" << id << ": {\n";
                out << "    const uint64_t base = Vu1Aot::cycle(vu);\n";
                out << "    if (base + " << v.pairs.back().issue << "u >= budgetEnd)\n    {\n";
                emitMaterialize(out, v.entry, "base", "        ");
                out << "        Vu1Aot::setPc(vu, " << pcText << ");\n        return -1;\n    }\n";
                for (size_t i = 0; i < v.pairs.size(); ++i)
                {
                    const SPair &sp = v.pairs[i];
                    const Vu1Aot::Pair &p = pruned.at(sp.pc);
                    const std::string k = sPairName(hash, sp.pc);
                    ++staticPairs;
                    if (sp.setCycle)
                        out << "    Vu1Aot::setCycle(vu, base + " << sp.issue << "u);\n";
                    if (sp.commit)
                        out << "    Vu1Aot::commit(vu);\n";
                    if (sp.recordBackup)
                        out << "    const int32_t old" << i << " = Vu1Aot::vi(vu, " << unsigned(p.viWriteReg) << "u);\n";
                    const uint8_t norm = sNormMask(p, cleanNow);
                    if (p.upKind == FU_ARITH || p.upKind == FU_ABS || p.upKind == FU_FTOI)
                    {
                        normSkipped += __builtin_popcount(~norm & 7u);
                        normKept += __builtin_popcount(norm & 7u);
                    }
                    sCleanStep(p, cleanNow);
                    const std::string exec = "Vu1AotStaticExecN<" + k + ", " + std::to_string(norm) + "u>";
                    if (!sp.branch)
                    {
                        out << "    Vu1Aot::execStatic<" << exec << ", true>(vu, " << k << ", vuData, dataSize);\n";
                    }
                    else
                    {
                        out << "    Vu1Aot::execStatic<" << exec << ", false>(vu, " << k << ", vuData, dataSize);\n";
                        const unsigned is = p.loFs & 0xFu, it = p.loFt & 0xFu;
                        const unsigned link = (sp.pc + 16u) / 8u;
                        auto bv = [](unsigned reg) { return "static_cast<int16_t>(Vu1Aot::branchVi(vu, " + std::to_string(reg) + "u))"; };
                        switch (p.loKind)
                        {
                        case FL_B:
                            break;
                        case FL_BAL:
                            out << "    Vu1Aot::setVi(vu, " << it << "u, " << link << ");\n";
                            break;
                        case FL_JR:
                            out << "    const uint32_t target = (static_cast<uint32_t>(static_cast<uint16_t>(Vu1Aot::branchVi(vu, " << is
                                << "u))) * 8u) & " << kPcMask << "u;\n    (void)target;\n";
                            break;
                        case FL_JALR:
                            out << "    const uint32_t target = (static_cast<uint32_t>(static_cast<uint16_t>(Vu1Aot::branchVi(vu, " << is
                                << "u))) * 8u) & " << kPcMask << "u;\n    (void)target;\n";
                            out << "    Vu1Aot::setVi(vu, " << it << "u, " << link << ");\n";
                            break;
                        case FL_IBEQ: out << "    const bool taken = " << bv(is) << " == " << bv(it) << ";\n"; break;
                        case FL_IBNE: out << "    const bool taken = " << bv(is) << " != " << bv(it) << ";\n"; break;
                        case FL_IBLTZ: out << "    const bool taken = " << bv(is) << " < 0;\n"; break;
                        case FL_IBGTZ: out << "    const bool taken = " << bv(is) << " > 0;\n"; break;
                        case FL_IBLEZ: out << "    const bool taken = " << bv(is) << " <= 0;\n"; break;
                        case FL_IBGEZ: out << "    const bool taken = " << bv(is) << " >= 0;\n"; break;
                        default: break;
                        }
                    }
                    if (sp.clearBackup)
                        out << "    Vu1Aot::clearBranchBackup(vu);\n";
                    if (sp.recordBackup)
                        out << "    Vu1Aot::recordBranchBackup(vu, " << unsigned(p.viWriteReg) << "u, old" << i << ");\n";
                }
                out << "    pairs += " << v.pairs.size() << "u;\n";
                out << "    Vu1Aot::setCycle(vu, base + " << v.end << "u);\n";
                const std::string when = "(base + " + std::to_string(v.end) + "u)";
                auto transition = [&](int which, const char *indent)
                {
                    const int s = v.succ[which];
                    char text[32];
                    std::snprintf(text, sizeof(text), "0x%04xu", v.succPc[which]);
                    if (!stub(s))
                    {
                        if (chunkOf[s] == static_cast<int>(c))
                            out << indent << "goto S_" << s << ";\n";
                        else
                            out << indent << "return " << s << ";\n";
                        return;
                    }
                    ++handovers;
                    emitMaterialize(out, v.succState[which], when, indent);
                    out << indent << "Vu1Aot::setPc(vu, " << text << ");\n" << indent << "return -1;\n";
                };
                switch (v.exit)
                {
                case SExit::Next: case SExit::Branch: case SExit::JrStatic: case SExit::Dynamic:
                    transition(0, "    ");
                    break;
                case SExit::Cond:
                    out << "    if (taken)\n    {\n";
                    transition(0, "        ");
                    out << "    }\n";
                    transition(1, "    ");
                    break;
                case SExit::JrDynamic:
                    if (v.succState[0].clean())
                        out << "    if (const int next = " << name << "_clean(target); next >= 0)\n        return next;\n";
                    else
                        emitMaterialize(out, v.succState[0], when, "    ");
                    out << "    Vu1Aot::setPc(vu, target);\n    return -1;\n";
                    ++handovers;
                    break;
                case SExit::Ended:
                {
                    emitMaterialize(out, v.succState[0], when, "    ");
                    char text[32];
                    std::snprintf(text, sizeof(text), "0x%04xu", v.succPc[0]);
                    out << "    Vu1Aot::setPc(vu, " << text << ");\n    return -2;\n";
                    break;
                }
                }
                out << "}\n";
            }
            out << "}\n//@@END\n\n";
        }

        // Clean entry points, and the chunk of every version.
        out << "int " << name << "_clean(uint32_t pc)\n{\n    switch (pc)\n    {\n";
        for (const auto &[pc, id] : tier.clean())
            if (!stub(id))
            {
                char line[64];
                std::snprintf(line, sizeof(line), "    case 0x%04xu: return %d;\n", pc, id);
                out << line;
            }
        out << "    default: return -1;\n    }\n}\n\n";
        out << "using " << name << "_chunk = int (*)" << sig << ";\n";
        out << "static const " << name << "_chunk " << name << "_chunks[] = {\n";
        for (const SVersion &v : versions)
        {
            if (stub(v.id))
                out << "    nullptr,\n";
            else
                out << "    &" << name << "_s" << chunkOf[v.id] << ",\n";
        }
        out << "};\n\n";

        // The image entry: static tier whenever nothing is in flight, dynamic tier otherwise.
        out << "static int " << name
            << "(VU1Interpreter &vu, uint8_t *__restrict vuData, uint32_t dataSize, uint64_t budgetEnd, uint64_t &pairs)\n{\n"
               "    for (;;)\n    {\n"
               "        if (Vu1Aot::quiescent(vu))\n        {\n"
               "            int vid = " << name << "_clean(Vu1Aot::pc(vu));\n"
               "            if (vid >= 0)\n            {\n"
               "                do\n                    vid = " << name << "_chunks[vid](vu, vuData, dataSize, budgetEnd, pairs, vid);\n"
               "                while (vid >= 0);\n"
               "                if (vid == -2)\n                    return Vu1Aot::kEnded;\n"
               "            }\n        }\n"
               "        const int result = " << name << "_dyn(vu, vuData, dataSize, budgetEnd, pairs);\n"
               "        if (result != Vu1Aot::kContinue)\n            return result;\n    }\n}\n\n";
        std::fprintf(stderr, "vu1aot_gen: image %016" PRIx64 ": static tier %zu versions (%zu clean entries) in %zu functions, %u pairs, %u handovers, %u version overflows, operand normalization skipped %u kept %u\n",
                     hash, versions.size(), tier.clean().size(), chunks.size(), staticPairs, handovers, tier.overflow(), normSkipped, normKept);
        if (std::getenv("VU1AOT_VERBOSE"))
        {
            for (const auto &[pc, n] : tier.m_overflowAt)
                std::fprintf(stderr, "    overflow at %04x x%u (versions there: %u)\n", pc, n, [&]() { unsigned c = 0; for (const SVersion &v : versions) c += v.pc == pc; return c; }());
            std::map<std::string, unsigned> kinds;
            for (const SVersion &v : versions)
            {
                if (stub(v.id))
                    continue;
                const char *k = v.exit == SExit::JrDynamic ? (v.succState[0].clean() ? "jr-dynamic clean" : "jr-dynamic busy")
                              : v.exit == SExit::Dynamic   ? "dynamic (unsupported pair)"
                              : v.exit == SExit::Ended     ? "ended" : nullptr;
                if (k)
                    ++kinds[k];
                for (int j = 0; j < 2; ++j)
                    if ((j == 0 || v.exit == SExit::Cond) && (v.exit == SExit::Next || v.exit == SExit::Branch || v.exit == SExit::JrStatic || v.exit == SExit::Cond) && stub(v.succ[j]))
                        ++kinds["successor handed over"];
            }
            for (const auto &[k, n] : kinds)
                std::fprintf(stderr, "    exits: %s x%u\n", k.c_str(), n);
        }
    }
}
