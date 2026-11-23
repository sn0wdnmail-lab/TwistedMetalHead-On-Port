// vu1aot_gen: turns recorded VU1 code images (PS2X_VU1_CAPTURE=1, Logs/vu1capture) into C++.
//
//   vu1aot_gen -o src/lib/vu/vu1_aot_programs.cpp <capture dir> [<capture dir> ...]
//
// Every capture dir holds code_<hash>.bin images and entries.txt ("hash pc runs cycles ...").
// For each image the generator follows the code from its recorded entry points (fall-through,
// static branch targets, return addresses after BAL/JALR) and emits one function per image in
// which every reachable pair is a labelled block calling Vu1Aot::pairCore with the decoded pair
// as a compile-time constant. Control flow that is only known at run time (JR/JALR, a branch's
// taken/not-taken result) goes through a switch on the program counter; a program counter with
// no block returns to the interpreter. Semantics are the fast interpreter's own (pairCore and
// postPair are shared), so the compiled code can only differ in speed.

#include "../../src/lib/vu/ps2_vu_fast_inl.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace vufast;

namespace
{
    constexpr uint32_t kCodeSize = 16384u;
    constexpr uint32_t kPcMask = 0x3FFFu;

    struct Image
    {
        uint64_t hash = 0;
        std::vector<uint8_t> code;
        std::set<uint32_t> entries;
        uint64_t runs = 0;
    };

    bool isBranchKind(uint8_t kind)
    {
        switch (kind)
        {
        case FL_B: case FL_BAL: case FL_JR: case FL_JALR:
        case FL_IBEQ: case FL_IBNE: case FL_IBLTZ: case FL_IBGTZ: case FL_IBLEZ: case FL_IBGEZ:
            return true;
        default:
            return false;
        }
    }

    bool isBranch(const Vu1Aot::Pair &p)
    {
        return !p.iBit && !p.reservedLower && isBranchKind(p.loKind);
    }

    bool hasStaticTarget(const Vu1Aot::Pair &p)
    {
        return isBranch(p) && p.loKind != FL_JR && p.loKind != FL_JALR;
    }

    uint32_t staticTarget(const Vu1Aot::Pair &p, uint32_t pc)
    {
        return static_cast<uint32_t>(pc + 8 + p.loImm * 8) & kPcMask;
    }

    uint32_t next(uint32_t pc) { return (pc + 8u) % kCodeSize; }
    uint32_t prev(uint32_t pc) { return (pc + kCodeSize - 8u) % kCodeSize; }

    std::string floatBits(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        char text[64];
        std::snprintf(text, sizeof(text), "std::bit_cast<float>(0x%08xu)", bits);
        return text;
    }

    std::string pairInitializer(const Vu1Aot::Pair &p)
    {
        std::ostringstream o;
        auto u = [&](const char *name, unsigned value)
        { o << "." << name << " = " << value << "u, "; };
        auto b = [&](const char *name, bool value)
        { o << "." << name << " = " << (value ? "true" : "false") << ", "; };
        o << "{.generation = 1u, ";
        char hex[64];
        std::snprintf(hex, sizeof(hex), ".upper = 0x%08xu, .lower = 0x%08xu, ", p.upper, p.lower);
        o << hex;
        o << ".immediate = " << floatBits(p.immediate) << ", ";
        u("upKind", p.upKind); u("upArith", p.upArith); u("upSrc", p.upSrc); u("upToAcc", p.upToAcc);
        u("upDest", p.upDest); u("upFs", p.upFs); u("upFt", p.upFt); u("upFd", p.upFd); u("upBc", p.upBc);
        u("upScale", p.upScale); u("loKind", p.loKind); u("loDest", p.loDest); u("loFs", p.loFs); u("loFt", p.loFt);
        u("loFd", p.loFd); u("loFsf", p.loFsf); u("loFtf", p.loFtf);
        o << ".loImm = " << static_cast<int>(p.loImm) << ", ";
        std::snprintf(hex, sizeof(hex), ".loImm24 = 0x%06xu, ", p.loImm24);
        o << hex;
        u("readCount", p.readCount);
        o << ".readReg = {" << unsigned(p.readReg[0]) << "u, " << unsigned(p.readReg[1]) << "u, " << unsigned(p.readReg[2])
          << "u, " << unsigned(p.readReg[3]) << "u}, ";
        o << ".readLanes = {" << unsigned(p.readLanes[0]) << "u, " << unsigned(p.readLanes[1]) << "u, "
          << unsigned(p.readLanes[2]) << "u, " << unsigned(p.readLanes[3]) << "u}, ";
        std::snprintf(hex, sizeof(hex), ".viRead = 0x%04xu, ", p.viRead);
        o << hex;
        u("upWriteReg", p.upWriteReg); u("upWriteLanes", p.upWriteLanes); u("upLatency", p.upLatency);
        u("loWriteReg", p.loWriteReg); u("loWriteLanes", p.loWriteLanes); u("loVfLatency", p.loVfLatency);
        u("viWriteReg", p.viWriteReg); u("viLatency", p.viLatency); u("loPipeline", p.loPipeline);
        u("loLatency", p.loLatency); u("shadowReg", p.shadowReg);
        b("iBit", p.iBit); b("eBit", p.eBit); b("dBit", p.dBit); b("tBit", p.tBit); b("waitQ", p.waitQ);
        b("waitP", p.waitP); b("delaysNextBranchRead", p.delaysNextBranchRead); b("reservedUpper", p.reservedUpper);
        b("reservedLower", p.reservedLower);
        b("writesVf0", p.writesVf0);
        o << ".flagsDead = " << (p.flagsDead ? "true" : "false") << "}";
        return o.str();
    }

    bool readFile(const fs::path &path, std::vector<uint8_t> &out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    // Emits the function for one image; returns the number of compiled pairs.
    uint32_t emitImage(std::ostream &out, const Image &image)
    {
        VU1Interpreter decoder(VU1Interpreter::Unit::VU1);
        std::map<uint32_t, Vu1Aot::Pair> pairs;
        auto decode = [&](uint32_t pc) -> const Vu1Aot::Pair &
        {
            auto it = pairs.find(pc);
            if (it == pairs.end())
            {
                Vu1Aot::Pair p;
                Vu1Aot::decode(decoder, p, image.code.data(), pc);
                it = pairs.emplace(pc, p).first;
            }
            return it->second;
        };

        // Reachability from the recorded entry points.
        std::set<uint32_t> labels;
        std::set<uint32_t> resumePoints; // where MSCNT continues after an E-bit end
        std::vector<uint32_t> work(image.entries.begin(), image.entries.end());
        while (!work.empty())
        {
            const uint32_t pc = work.back();
            work.pop_back();
            if ((pc & 7u) != 0u || pc >= kCodeSize || !labels.insert(pc).second)
                continue;
            const Vu1Aot::Pair &p = decode(pc);
            if (p.reservedUpper || p.reservedLower)
                continue;
            const Vu1Aot::Pair &before = decode(prev(pc));
            if (before.eBit && !(before.reservedUpper || before.reservedLower))
            {
                // E-bit delay slot: the program ends after this pair, and a later MSCNT resumes
                // at the next one (the capture only records MSCAL entry points).
                if (next(pc) < kCodeSize)
                {
                    resumePoints.insert(next(pc));
                    work.push_back(next(pc));
                }
                continue;
            }
            if (isBranch(p))
            {
                work.push_back(next(pc)); // delay slot
                continue;
            }
            if (isBranch(before))
            {
                // This pair is the branch's delay slot: control continues at the target and/or after it.
                if (hasStaticTarget(before))
                    work.push_back(staticTarget(before, prev(pc)));
                // Not taken, or the return address of BAL/JALR. After B/JR it is still a likely entry:
                // microcode often "calls" with B plus an IADDIU of the return address in the delay
                // slot and comes back with JR.
                work.push_back(next(pc));
                continue;
            }
            work.push_back(next(pc));
        }

        // ---- Static stall pruning ----
        // Inside a basic block, a pair issues at least one cycle after the previous one, so a read
        // of a lane written L or more pairs earlier in the same block can never stall: drop it from
        // the pair's runtime stall check. Block starts: entry points, static branch targets, the
        // pair after every delay slot (branch fall-through / return address) and any gap.
        std::set<uint32_t> blockStarts(image.entries.begin(), image.entries.end());
        blockStarts.insert(resumePoints.begin(), resumePoints.end());
        for (uint32_t pc : labels)
        {
            const Vu1Aot::Pair &p = decode(pc);
            if (isBranch(p))
            {
                if (hasStaticTarget(p))
                    blockStarts.insert(staticTarget(p, pc));
                blockStarts.insert(next(next(pc)));
            }
            if (!labels.count(prev(pc)))
                blockStarts.insert(pc);
        }
        std::map<uint32_t, Vu1Aot::Pair> pruned;
        uint32_t prunedLanes = 0, keptLanes = 0;
        {
            // known = the lane was written earlier in this block (otherwise its ready time comes
            // from before the block and must be checked at run time).
            struct Writer { bool known = false; int index = 0; int latency = 0; };
            Writer vf[32][4];
            Writer vi[16];
            int index = 0;
            for (uint32_t pc : labels)
            {
                if (blockStarts.count(pc))
                {
                    for (auto &reg : vf)
                        for (auto &lane : reg)
                            lane = Writer{};
                    for (auto &reg : vi)
                        reg = Writer{};
                    index = 0;
                }
                Vu1Aot::Pair p = decode(pc);
                if (!(p.reservedUpper || p.reservedLower))
                {
                    uint8_t regs[4] = {}, lanes[4] = {};
                    uint8_t count = 0;
                    for (uint32_t r = 0; r < p.readCount; ++r)
                    {
                        const uint8_t reg = p.readReg[r];
                        uint8_t mask = p.readLanes[r];
                        for (uint32_t lane = 0; lane < 4u; ++lane)
                        {
                            const uint8_t bit = static_cast<uint8_t>(8u >> lane);
                            if (!(mask & bit))
                                continue;
                            const Writer &w = vf[reg][lane];
                            if (reg == 0u || (w.known && index - w.index >= w.latency))
                            {
                                mask = static_cast<uint8_t>(mask & ~bit);
                                ++prunedLanes;
                            }
                            else
                                ++keptLanes;
                        }
                        if (mask != 0u)
                        {
                            regs[count] = reg;
                            lanes[count] = mask;
                            ++count;
                        }
                    }
                    for (uint32_t r = 0; r < 4u; ++r)
                    {
                        p.readReg[r] = r < count ? regs[r] : 0u;
                        p.readLanes[r] = r < count ? lanes[r] : 0u;
                    }
                    p.readCount = count;
                    uint16_t viRead = p.viRead;
                    for (uint32_t reg = 0; reg < 16u; ++reg)
                    {
                        if (!(viRead & (1u << reg)))
                            continue;
                        if (reg == 0u || (vi[reg].known && index - vi[reg].index >= vi[reg].latency))
                            viRead = static_cast<uint16_t>(viRead & ~(1u << reg));
                    }
                    p.viRead = viRead;
                    // record this pair's writes (upper first, then lower, as pairCore does)
                    for (uint32_t lane = 0; lane < 4u; ++lane)
                    {
                        const uint8_t bit = static_cast<uint8_t>(8u >> lane);
                        if (p.upWriteReg != 0u && (p.upWriteLanes & bit))
                            vf[p.upWriteReg][lane] = Writer{true, index, p.upLatency};
                        if (p.loWriteReg != 0u && (p.loWriteLanes & bit))
                            vf[p.loWriteReg][lane] = Writer{true, index, p.loVfLatency};
                    }
                    if (p.viWriteReg != 0u)
                        vi[p.viWriteReg] = Writer{true, index, p.viLatency};
                }
                pruned[pc] = p;
                ++index;
            }
        }

        // ---- FMAC flag elision ----
        // MAC/status flags appear four cycles after an FMAC. When nothing in this code image reads
        // the sticky status bits, an FMAC's flags only matter if a reader (FMAND/FMEQ/FMOR/FSAND)
        // or a block exit can see them before a later FMAC replaces them; a later FMAC at least
        // four pairs before every such point replaces them for sure (pairs issue >= 1 cycle apart).
        uint32_t deadFlags = 0, liveFlags = 0;
        {
            bool stickyRead = false;
            for (uint32_t pc : labels)
            {
                const Vu1Aot::Pair &p = decode(pc);
                if (p.iBit || p.reservedLower)
                    continue;
                if (p.loKind == FL_FSEQ || p.loKind == FL_FSOR || (p.loKind == FL_FSAND && (p.loImm24 & 0xFC0u) != 0u))
                    stickyRead = true;
            }
            if (!stickyRead)
            {
                auto isFmac = [](const Vu1Aot::Pair &p)
                {
                    return !p.reservedUpper && p.upKind == FU_ARITH && p.upDest != 0u && p.upArith != FA_MAX && p.upArith != FA_MINI;
                };
                auto readsFlags = [](const Vu1Aot::Pair &p)
                {
                    return !p.iBit && !p.reservedLower &&
                           (p.loKind == FL_FMAND || p.loKind == FL_FMEQ || p.loKind == FL_FMOR || p.loKind == FL_FSAND ||
                            p.loKind == FL_FSEQ || p.loKind == FL_FSOR);
                };
                // walk blocks: [start, end) runs of labels between block starts
                std::vector<uint32_t> order(labels.begin(), labels.end());
                size_t b0 = 0;
                while (b0 < order.size())
                {
                    size_t b1 = b0 + 1;
                    while (b1 < order.size() && !blockStarts.count(order[b1]) && order[b1] == next(order[b1 - 1]) && order[b1] != 0u)
                        ++b1;
                    const int len = static_cast<int>(b1 - b0);
                    // observation points (relative index): readers inside the block, and the block exit
                    // (treated as a reader at index len; an E bit ends the program at its delay slot,
                    // which is the block's last pair or beyond, so `len` covers it too).
                    std::vector<int> points;
                    std::vector<int> fmacs;
                    for (int k = 0; k < len; ++k)
                    {
                        const Vu1Aot::Pair &p = decode(order[b0 + k]);
                        if (readsFlags(p))
                            points.push_back(k);
                        if (isFmac(p))
                            fmacs.push_back(k);
                    }
                    points.push_back(len);
                    for (int i : fmacs)
                    {
                        bool live = false;
                        for (int r : points)
                        {
                            if (r <= i)
                                continue;
                            // shadowed for this observer if a later FMAC j < r issued >= 4 pairs before it
                            bool shadowed = false;
                            for (int j : fmacs)
                                if (j > i && j < r && r - j >= 4)
                                {
                                    shadowed = true;
                                    break;
                                }
                            if (!shadowed)
                            {
                                live = true;
                                break;
                            }
                        }
                        if (live)
                            ++liveFlags;
                        else
                        {
                            pruned[order[b0 + i]].flagsDead = true;
                            ++deadFlags;
                        }
                    }
                    b0 = b1;
                }
            }
        }

        char name[64];
        std::snprintf(name, sizeof(name), "vu1aot_%016" PRIx64, image.hash);

        // Blocks: runs of consecutive pairs from one block start to the next. Each block is its
        // own small function (quick to compile, any of its pairs can be entered through its
        // switch); the image function dispatches on the program counter between blocks.
        std::vector<std::vector<uint32_t>> blocks;
        {
            uint32_t last = ~0u;
            for (uint32_t pc : labels)
            {
                if (blocks.empty() || blockStarts.count(pc) || pc != next(last) || pc == 0u)
                    blocks.emplace_back();
                blocks.back().push_back(pc);
                last = pc;
            }
        }
        std::map<uint32_t, size_t> blockOf;
        for (size_t b = 0; b < blocks.size(); ++b)
            for (uint32_t pc : blocks[b])
                blockOf[pc] = b;

        // Functions ("chunks"): runs of consecutive blocks, up to kChunkPairs pairs. Control flow
        // inside a chunk stays inside it (fall-through, and gotos to static branch targets in the
        // same chunk); only dynamic jumps and targets in other chunks go back to the image
        // dispatcher. Small enough to compile quickly without spilling everything.
        constexpr size_t kChunkPairs = 128;
        std::vector<std::vector<uint32_t>> chunks;
        for (const std::vector<uint32_t> &block : blocks)
        {
            if (chunks.empty() || chunks.back().size() + block.size() > kChunkPairs || block.front() != next(chunks.back().back()) ||
                block.front() == 0u)
                chunks.emplace_back();
            chunks.back().insert(chunks.back().end(), block.begin(), block.end());
        }
        std::map<uint32_t, size_t> chunkOf;
        for (size_t c = 0; c < chunks.size(); ++c)
            for (uint32_t pc : chunks[c])
                chunkOf[pc] = c;

        out << "// ---- code image " << name + 7 << ": " << image.entries.size() << " entry points, " << labels.size()
            << " pairs, " << blocks.size() << " blocks in " << chunks.size() << " functions ----\n";
        uint32_t compiled = 0;
        uint32_t localJumps = 0, pendingChecks = 0;
        for (size_t c = 0; c < chunks.size(); ++c)
        {
            const std::vector<uint32_t> &chunk = chunks[c];
            out << "static VU_AOT_BLOCK int " << name << "_c" << c
                << "(VU1Interpreter &vu, uint8_t *__restrict vuData, uint32_t dataSize, uint64_t budgetEnd, uint64_t &pairs)\n{\n";
            out << "    switch (Vu1Aot::pc(vu))\n    {\n";
            for (uint32_t pc : chunk)
            {
                char line[64];
                std::snprintf(line, sizeof(line), "    case 0x%04xu: goto L_%04x;\n", pc, pc);
                out << line;
            }
            out << "    default: return Vu1Aot::kUnhandled;\n    }\n";
            for (size_t i = 0; i < chunk.size(); ++i)
            {
                const uint32_t pc = chunk[i];
                const Vu1Aot::Pair &p = decode(pc);
                char head[32];
                std::snprintf(head, sizeof(head), "L_%04x: {\n", pc);
                out << head;
                char pcText[16];
                std::snprintf(pcText, sizeof(pcText), "0x%04xu", pc);
                if (p.reservedUpper || p.reservedLower)
                {
                    out << "    Vu1Aot::setPc(vu, " << pcText << ");\n    return Vu1Aot::kUnhandled; // reserved: the interpreter reports it\n}\n";
                    continue;
                }
                ++compiled;
                char ftText[16];
                std::snprintf(ftText, sizeof(ftText), "0x%04xu", next(pc));
                const bool alwaysPost = isBranch(p) || p.eBit || p.dBit || p.tBit;
                // A branch, E bit or halt can only be pending here if the previous pair is a branch
                // (this is its delay slot) or carries E/D/T: elsewhere the runtime check is dropped.
                const Vu1Aot::Pair &before = decode(prev(pc));
                const bool beforeValid = !(before.reservedUpper || before.reservedLower);
                const bool mayBePending = beforeValid && (isBranch(before) || before.eBit || before.dBit || before.tBit);
                // After this pair's control handling: a static branch target in this chunk is a goto.
                std::string dispatch = "return Vu1Aot::kContinue;";
                if (beforeValid && isBranch(before) && hasStaticTarget(before))
                {
                    const uint32_t target = staticTarget(before, prev(pc));
                    auto it = chunkOf.find(target);
                    if (it != chunkOf.end() && it->second == c)
                    {
                        char text[96];
                        std::snprintf(text, sizeof(text), "if (Vu1Aot::pc(vu) == 0x%04xu) goto L_%04x; return Vu1Aot::kContinue;", target, target);
                        dispatch = text;
                        ++localJumps;
                    }
                }
                out << "    static constexpr Vu1Aot::Pair k = " << pairInitializer(pruned.at(pc)) << ";\n";
                out << "    if (Vu1Aot::cycle(vu) >= budgetEnd) { Vu1Aot::setPc(vu, " << pcText << "); return Vu1Aot::kBudget; }\n";
                if (alwaysPost)
                    out << "    Vu1Aot::setPc(vu, " << pcText << ");\n";
                out << "    if (!Vu1Aot::pairCoreWith<Vu1AotStaticExec<k>>(vu, k, vuData, dataSize, budgetEnd, pairs)) { Vu1Aot::setPc(vu, "
                    << pcText << "); return Vu1Aot::kBudget; }\n";
                const std::string post = "if (Vu1Aot::postPair(vu, k, 16384u)) return Vu1Aot::kEnded; if (Vu1Aot::pc(vu) != " +
                                         std::string(ftText) + ") { " + dispatch + " }";
                if (alwaysPost)
                    out << "    " << post << "\n";
                else if (mayBePending)
                {
                    ++pendingChecks;
                    out << "    if (Vu1Aot::pendingControl(vu)) { Vu1Aot::setPc(vu, " << pcText << "); " << post
                        << " } else Vu1Aot::tick(vu);\n";
                }
                else
                    out << "    Vu1Aot::tick(vu);\n";
                out << "}\n";
            }
            char tail[96];
            std::snprintf(tail, sizeof(tail), "    Vu1Aot::setPc(vu, 0x%04xu);\n    return Vu1Aot::kContinue;\n}\n\n",
                          next(chunk.back()));
            out << tail;
        }

        out << "static int " << name
            << "(VU1Interpreter &vu, uint8_t *__restrict vuData, uint32_t dataSize, uint64_t budgetEnd, uint64_t &pairs)\n{\n";
        out << "    for (;;)\n    {\n        int result;\n        switch (Vu1Aot::pc(vu))\n        {\n";
        for (size_t c = 0; c < chunks.size(); ++c)
        {
            for (uint32_t pc : chunks[c])
            {
                char line[48];
                std::snprintf(line, sizeof(line), "        case 0x%04xu:\n", pc);
                out << line;
            }
            out << "            result = " << name << "_c" << c << "(vu, vuData, dataSize, budgetEnd, pairs);\n            break;\n";
        }
        out << "        default:\n            return Vu1Aot::kUnhandled;\n        }\n";
        out << "        if (result != Vu1Aot::kContinue)\n            return result;\n    }\n}\n\n";
        std::fprintf(stderr, "vu1aot_gen: image %016" PRIx64 ": %zu functions, %u branches stay inside a function, %u runtime control checks kept\n",
                     image.hash, chunks.size(), localJumps, pendingChecks);
        std::fprintf(stderr, "vu1aot_gen: image %016" PRIx64 ": stall checks kept for %u lane reads, removed for %u; FMAC flags kept %u, elided %u\n",
                     image.hash, keptLanes, prunedLanes, liveFlags, deadFlags);
        return compiled;
    }
}

int main(int argc, char **argv)
{
    std::string outPath = "vu1_aot_programs.cpp";
    std::vector<fs::path> dirs;
    std::vector<fs::path> extraFiles; // extra entry points: "<hash> <pc>" lines, or performancelogs.txt "vu1-miss" lines
    uint64_t minRuns = 1;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc)
            outPath = argv[++i];
        else if (arg == "--extra" && i + 1 < argc)
            extraFiles.emplace_back(argv[++i]);
        else if (arg == "--min-runs" && i + 1 < argc)
            minRuns = std::strtoull(argv[++i], nullptr, 10);
        else
            dirs.emplace_back(arg);
    }

    std::map<uint64_t, Image> images;
    for (const fs::path &dir : dirs)
    {
        std::ifstream entries(dir / "entries.txt");
        std::string line;
        while (std::getline(entries, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            std::istringstream fields(line);
            std::string hashText, pcText;
            uint64_t runs = 0;
            fields >> hashText >> pcText >> runs;
            if (hashText.empty() || pcText.empty())
                continue;
            const uint64_t hash = std::strtoull(hashText.c_str(), nullptr, 16);
            const uint32_t pc = static_cast<uint32_t>(std::strtoul(pcText.c_str(), nullptr, 16));
            Image &image = images[hash];
            image.hash = hash;
            image.entries.insert(pc);
            image.runs += runs;
        }
        // Runs recorded before entries.txt was first written still name their entry point.
        for (const auto &file : fs::directory_iterator(dir))
        {
            const std::string fname = file.path().filename().string();
            unsigned long long hash = 0;
            unsigned pc = 0, n = 0;
            if (std::sscanf(fname.c_str(), "run_%16llx_%4x_%u.bin", &hash, &pc, &n) == 3)
            {
                Image &image = images[hash];
                image.hash = hash;
                image.entries.insert(pc);
            }
        }
    }

    // Extra entry points (pcs the running game reached that the compiled code had no entry for).
    for (const fs::path &file : extraFiles)
    {
        std::ifstream in(file);
        std::string line;
        uint32_t added = 0;
        while (std::getline(in, line))
        {
            std::string text = line;
            const size_t miss = text.find("vu1-miss ");
            if (miss != std::string::npos)
                text = text.substr(miss + 9);
            else if (text.empty() || text[0] == '#' || text.find_first_not_of("0123456789abcdefABCDEF \t\r") != std::string::npos)
                continue;
            std::istringstream fields(text);
            std::string hashText, pcText;
            fields >> hashText >> pcText;
            if (hashText.size() != 16u || pcText.empty())
                continue;
            const uint64_t hash = std::strtoull(hashText.c_str(), nullptr, 16);
            const uint32_t pc = static_cast<uint32_t>(std::strtoul(pcText.c_str(), nullptr, 16));
            auto it = images.find(hash);
            if (it == images.end())
                continue;
            if (it->second.entries.insert(pc).second)
                ++added;
        }
        std::fprintf(stderr, "vu1aot_gen: %u extra entry points from %s\n", added, file.string().c_str());
    }

    std::ofstream out(outPath);
    out << "// Generated by tools/vu1aot/vu1aot_gen from recorded VU1 code images - do not edit.\n"
           "// Each function runs one 16 KB VU1 code image with the fast core's own pair semantics.\n\n"
           "#include \"ps2_vu_fast_inl.h\"\n#include <bit>\n\n"
           "// Blocks stay separate functions: inlined into one dispatcher they spill everything.\n"
           "#if defined(__GNUC__) || defined(__clang__)\n#define VU_AOT_BLOCK __attribute__((noinline))\n#else\n#define VU_AOT_BLOCK __declspec(noinline)\n#endif\n\n"
           "#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC diagnostic ignored \"-Wunused-label\"\n#endif\n\n";

    std::vector<std::pair<const Image *, uint32_t>> emitted;
    for (auto &[hash, image] : images)
    {
        char name[64];
        std::snprintf(name, sizeof(name), "code_%016" PRIx64 ".bin", hash);
        bool found = false;
        for (const fs::path &dir : dirs)
        {
            if (readFile(dir / name, image.code) && image.code.size() == kCodeSize)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            std::fprintf(stderr, "vu1aot_gen: no %s, skipped\n", name);
            continue;
        }
        if (vufast::codeHash(image.code.data(), image.code.size()) != hash)
        {
            std::fprintf(stderr, "vu1aot_gen: %s does not match its hash, skipped\n", name);
            continue;
        }
        if (image.runs < minRuns && image.runs != 0)
            continue;
        const uint32_t compiled = emitImage(out, image);
        emitted.emplace_back(&image, compiled);
        std::fprintf(stderr, "vu1aot_gen: image %016" PRIx64 ": %zu entries, %u pairs\n", hash, image.entries.size(), compiled);
    }

    out << "extern const Vu1AotProgram g_vu1AotPrograms[] = {\n";
    for (const auto &[image, compiled] : emitted)
    {
        char line[160];
        std::snprintf(line, sizeof(line), "    {0x%016" PRIx64 "ull, &vu1aot_%016" PRIx64 ", %uu, %zuu},\n", image->hash, image->hash,
                      compiled, image->entries.size());
        out << line;
    }
    if (emitted.empty())
        out << "    {0ull, nullptr, 0u, 0u},\n";
    out << "};\nextern const size_t g_vu1AotProgramCount = " << emitted.size() << ";\n";
    std::fprintf(stderr, "vu1aot_gen: wrote %s (%zu images)\n", outPath.c_str(), emitted.size());
    return 0;
}
