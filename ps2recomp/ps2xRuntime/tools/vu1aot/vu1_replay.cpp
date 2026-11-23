// vu1_replay: replays VU1 runs recorded with PS2X_VU1_CAPTURE=1 (or VU0 runs recorded with
// PS2X_VU0_CAPTURE=1) and checks the fast core (interpreter and compiled programs) against what
// the game produced on the recording PC.
//
//   vu1_replay <capture dir> [--repeat N] [--mode interp|aot|both]
//
// For every run_<hash>_<pc>_<n>.bin: restores registers and VU1 data memory, runs the program
// from its start address, and compares the final registers, data memory, PATH1 packets and cycle
// count with the recording. Then times both cores over all runs.

#include "../../src/lib/vu/ps2_vu_fast_inl.h"
#include "runtime/gs/gs_frontend.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    bool g_ignoreSticky = false;

    struct Run
    {
        std::string name;
        uint64_t hash = 0;
        uint32_t startPc = 0, top = 0, itop = 0, maxCycles = 0, ended = 0, resume = 0;
        bool vu0 = false;
        uint32_t size = 16384; // code and data memory size of the unit
        VU1State before{}, after{};
        std::vector<uint8_t> dataBefore, dataAfter, gif;
        uint64_t cycles = 0;
    };

    bool readState(const uint8_t *&p, const uint8_t *end, VU1State &s)
    {
        const size_t need = 32 * 4 * 4 + 16 * 4 + 4 * 4 + 3 * 4 + 8 * 4 + 8 + 2 * 4;
        if (static_cast<size_t>(end - p) < need)
            return false;
        std::memcpy(s.vf, p, sizeof(s.vf)); p += sizeof(s.vf);
        std::memcpy(s.vi, p, sizeof(s.vi)); p += sizeof(s.vi);
        std::memcpy(s.acc, p, sizeof(s.acc)); p += sizeof(s.acc);
        float fs[3]; std::memcpy(fs, p, sizeof(fs)); p += sizeof(fs);
        s.q = fs[0]; s.p = fs[1]; s.i = fs[2];
        uint32_t us[8]; std::memcpy(us, p, sizeof(us)); p += sizeof(us);
        s.r = us[0]; s.pc = us[1]; s.mac = us[2]; s.clip = us[3]; s.status = us[4]; s.top = us[5]; s.itop = us[6];
        uint8_t bs[8]; std::memcpy(bs, p, sizeof(bs)); p += sizeof(bs);
        s.ebit = bs[0]; s.haltAfterDelaySlot = bs[1]; s.dBitEnabled = bs[2]; s.tBitEnabled = bs[3];
        s.stoppedByD = bs[4]; s.stoppedByT = bs[5]; s.branchPending = bs[6];
        uint32_t br[2]; std::memcpy(br, p, sizeof(br)); p += sizeof(br);
        s.branchTarget = br[0]; s.branchDelay = br[1];
        return true;
    }

    bool loadRun(const fs::path &path, Run &run)
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const uint8_t *p = bytes.data();
        const uint8_t *end = p + bytes.size();
        if (bytes.size() < 40)
            return false;
        uint32_t header[8];
        std::memcpy(header, p, sizeof(header)); p += sizeof(header);
        if (header[0] != 0x52315556u && header[0] != 0x52305556u) // 'VU1R' / 'VU0R'
            return false;
        run.vu0 = header[0] == 0x52305556u;
        run.size = run.vu0 ? 4096u : 16384u;
        const uint32_t size = run.size;
        run.startPc = header[2]; run.top = header[3]; run.itop = header[4]; run.maxCycles = header[5]; run.ended = header[6];
        run.resume = header[7];
        std::memcpy(&run.hash, p, 8); p += 8;
        if (!readState(p, end, run.before) || end - p < size)
            return false;
        run.dataBefore.assign(p, p + size); p += size;
        if (!readState(p, end, run.after) || end - p < size + 12)
            return false;
        run.dataAfter.assign(p, p + size); p += size;
        std::memcpy(&run.cycles, p, 8); p += 8;
        uint32_t gifSize = 0;
        std::memcpy(&gifSize, p, 4); p += 4;
        if (static_cast<size_t>(end - p) < gifSize)
            return false;
        run.gif.assign(p, p + gifSize);
        run.name = path.filename().string();
        return true;
    }

    struct Outcome
    {
        VU1State state{};
        std::vector<uint8_t> data, gif;
        uint64_t cycles = 0;
    };

    // One interpreter per code image, reused like the game's VU1 (decode cache, compiled-program
    // lookup and flag scan stay warm).
    std::map<uint64_t, std::unique_ptr<VU1Interpreter>> g_units[2];

    void execute(PS2Memory &memory, GS &gs, const std::vector<uint8_t> &code, const Run &run, Outcome &out)
    {
        auto &slot = g_units[Vu1Aot::enabled() ? 1 : 0][run.hash];
        if (!slot)
            slot = std::make_unique<VU1Interpreter>(run.vu0 ? VU1Interpreter::Unit::VU0 : VU1Interpreter::Unit::VU1);
        VU1Interpreter &vu = *slot;
        uint8_t *const vuCode = run.vu0 ? memory.getVU0Code() : memory.getVU1Code();
        uint8_t *const vuData = run.vu0 ? memory.getVU0Data() : memory.getVU1Data();
        const uint32_t size = run.size;
        std::memcpy(vuCode, code.data(), size);
        std::memcpy(vuData, run.dataBefore.data(), size);
        vu.state() = run.before;
        out.gif.clear();
        Vu1Aot::setGifCapture(vu, &out.gif, false);
        const uint64_t start = Vu1Aot::cycle(vu);
        if (run.resume)
        {
            // MSCNT: continue from the recorded state with a drained pipeline.
            Vu1Aot::resetPipeline(vu);
            vu.state() = run.before;
            vu.resume(vuCode, size, vuData, size, gs, &memory, run.top, run.itop, run.maxCycles);
        }
        else
            vu.execute(vuCode, size, vuData, size, gs, &memory, run.startPc, run.top, run.itop, run.maxCycles);
        out.cycles = Vu1Aot::cycle(vu) - start;
        out.state = vu.state();
        out.data.assign(vuData, vuData + size);
    }

    // Returns a description of the first difference, or empty.
    std::string compare(const VU1State &a, const std::vector<uint8_t> &aData, const std::vector<uint8_t> &aGif, uint64_t aCycles,
                        const VU1State &b, const std::vector<uint8_t> &bData, const std::vector<uint8_t> &bGif, uint64_t bCycles)
    {
        char text[256];
        if (std::memcmp(a.vf, b.vf, sizeof(a.vf)) != 0)
        {
            for (int r = 0; r < 32; ++r)
                for (int c = 0; c < 4; ++c)
                {
                    uint32_t x, y;
                    std::memcpy(&x, &a.vf[r][c], 4);
                    std::memcpy(&y, &b.vf[r][c], 4);
                    if (x != y)
                    {
                        std::snprintf(text, sizeof(text), "vf%02d.%c %08x vs %08x", r, "xyzw"[c], x, y);
                        return text;
                    }
                }
        }
        for (int r = 0; r < 16; ++r)
            if (a.vi[r] != b.vi[r])
            {
                std::snprintf(text, sizeof(text), "vi%02d %d vs %d", r, a.vi[r], b.vi[r]);
                return text;
            }
        if (std::memcmp(a.acc, b.acc, sizeof(a.acc)) != 0)
            return "acc";
        if (std::memcmp(&a.q, &b.q, 4) != 0) return "q";
        if (std::memcmp(&a.p, &b.p, 4) != 0) return "p";
        if (std::memcmp(&a.i, &b.i, 4) != 0) return "i";
        if (a.r != b.r) return "r";
        if (a.pc != b.pc)
        {
            std::snprintf(text, sizeof(text), "pc %04x vs %04x", a.pc, b.pc);
            return text;
        }
        if (a.mac != b.mac) return "mac";
        if (a.clip != b.clip) return "clip";
        if (a.status != b.status)
        {
            // Compiled programs skip flags nothing can read; the sticky bits they would have added
            // are unobservable in images that never read them.
            if (((a.status ^ b.status) & ~0xFC0u) != 0u || !g_ignoreSticky)
                return "status";
        }
        if (aData != bData)
        {
            for (size_t i = 0; i < aData.size(); ++i)
                if (aData[i] != bData[i])
                {
                    std::snprintf(text, sizeof(text), "data[%04zx] %02x vs %02x", i, aData[i], bData[i]);
                    return text;
                }
        }
        if (aGif != bGif)
        {
            std::snprintf(text, sizeof(text), "PATH1 output %zu vs %zu bytes", aGif.size(), bGif.size());
            return text;
        }
        if (aCycles != bCycles)
        {
            std::snprintf(text, sizeof(text), "cycles %" PRIu64 " vs %" PRIu64, aCycles, bCycles);
            return text;
        }
        return std::string();
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: vu1_replay <capture dir> [--repeat N] [--mode interp|aot|both]\n");
        return 2;
    }
    const fs::path dir = argv[1];
    int repeat = 20;
    std::string mode = "both";
    std::string traceRun;
    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--repeat" && i + 1 < argc)
            repeat = std::atoi(argv[++i]);
        else if (arg == "--mode" && i + 1 < argc)
            mode = argv[++i];
        else if (arg == "--trace" && i + 1 < argc)
            traceRun = argv[++i];
    }

    std::vector<Run> runs;
    std::map<uint64_t, std::vector<uint8_t>> codes;
    for (const auto &file : fs::directory_iterator(dir))
    {
        const std::string fname = file.path().filename().string();
        if (fname.rfind("run_", 0) != 0)
            continue;
        Run run;
        if (!loadRun(file.path(), run))
        {
            std::fprintf(stderr, "skipping %s (unreadable)\n", fname.c_str());
            continue;
        }
        if (!codes.count(run.hash))
        {
            char name[64];
            std::snprintf(name, sizeof(name), "code_%016" PRIx64 ".bin", run.hash);
            std::ifstream in(dir / name, std::ios::binary);
            std::vector<uint8_t> code((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (code.size() != run.size)
                continue;
            codes[run.hash] = std::move(code);
        }
        runs.push_back(std::move(run));
    }
    std::sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) { return a.name < b.name; });
    std::printf("%zu runs, %zu code images\n", runs.size(), codes.size());

    PS2Memory memory;
    if (!memory.initialize())
        return 1;
    GS gs;

    if (!traceRun.empty())
    {
        for (const Run &run : runs)
        {
            if (run.name != traceRun)
                continue;
            for (int core = 0; core < 2; ++core)
            {
                Vu1Aot::enabledFlag() = core == 1;
                std::fprintf(stderr, "==== %s ====\n", core ? "compiled" : "interpreter");
                g_vu1TracePairs = true;
                Outcome out;
                execute(memory, gs, codes[run.hash], run, out);
                g_vu1TracePairs = false;
            }
        }
        return 0;
    }

    // 1. Correctness against the recording, for each core.
    const bool doInterp = mode == "interp" || mode == "both";
    const bool doAot = mode == "aot" || mode == "both";
    int failures[2] = {0, 0};
    uint64_t totalCycles = 0;
    for (const Run &run : runs)
    {
        totalCycles += run.cycles;
        for (int core = 0; core < 2; ++core)
        {
            if ((core == 0 && !doInterp) || (core == 1 && !doAot))
                continue;
            Vu1Aot::enabledFlag() = core == 1;
            g_ignoreSticky = core == 1 && !run.vu0; // VU0 keeps every flag: the EE reads them afterwards
            Outcome out;
            execute(memory, gs, codes[run.hash], run, out);
            const std::string diff = compare(out.state, out.data, out.gif, out.cycles, run.after, run.dataAfter, run.gif, run.cycles);
            if (!diff.empty())
            {
                if (failures[core] < 12)
                    std::printf("  %s %s: %s\n", core ? "aot   " : "interp", run.name.c_str(), diff.c_str());
                ++failures[core];
            }
        }
    }
    if (doInterp)
        std::printf("interpreter: %d of %zu runs differ from the recording\n", failures[0], runs.size());
    if (doAot)
        std::printf("compiled   : %d of %zu runs differ from the recording\n", failures[1], runs.size());

    // 2. Speed.
    for (int core = 0; core < 2; ++core)
    {
        if ((core == 0 && !doInterp) || (core == 1 && !doAot))
            continue;
        Vu1Aot::enabledFlag() = core == 1;
        Outcome out;
        // Setup alone (copying memories, restoring registers), to subtract from the total.
        const auto s0 = std::chrono::steady_clock::now();
        for (int r = 0; r < repeat; ++r)
            for (const Run &run : runs)
            {
                uint8_t *const vuCode = run.vu0 ? memory.getVU0Code() : memory.getVU1Code();
                uint8_t *const vuData = run.vu0 ? memory.getVU0Data() : memory.getVU1Data();
                std::memcpy(vuCode, codes[run.hash].data(), run.size);
                std::memcpy(vuData, run.dataBefore.data(), run.size);
                out.state = run.before;
                out.data.assign(vuData, vuData + run.size);
            }
        const double setup = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < repeat; ++r)
            for (const Run &run : runs)
                execute(memory, gs, codes[run.hash], run, out);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - setup;
        const double cycles = static_cast<double>(totalCycles) * repeat;
        std::printf("%s: %.3f s (setup %.3f s excluded) for %.2fM VU cycles = %.2f ns per VU cycle\n",
                    core ? "compiled   " : "interpreter", seconds, setup, cycles / 1e6, seconds * 1e9 / cycles);
    }
    return failures[0] + failures[1] == 0 ? 0 : 1;
}
