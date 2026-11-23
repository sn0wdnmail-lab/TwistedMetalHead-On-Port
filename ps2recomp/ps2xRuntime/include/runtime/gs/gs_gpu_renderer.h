#pragma once

// GPU GS renderer: the PS2 Graphics Synthesizer emulated with Vulkan compute shaders by
// paraLLEl-GS (Arntzen Software AS, LGPLv3+, third_party/parallel-gs). It takes the same GIF
// packet stream the software renderer parses, draws on the graphics card, and at every VBlank
// scans out the displayed buffer and copies it back (asynchronously) for the window to show.
//
// Threads: init/gifTransfer/vsync/shutdown are called on the game thread (init may also run on
// the window thread before the game starts); takeFrame is called on the window thread.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct GSRegisters;

namespace ps2gpu
{
    struct Options
    {
        uint32_t superSampling = 1u; // 1, 2, 4, 8 or 16
    };

    struct Description
    {
        std::string device;  // GPU name
        std::string details; // driver, Vulkan version, memory
    };

    // True when the runtime was built with paraLLEl-GS.
    bool compiledIn() noexcept;

    // Creates the Vulkan device and the GS emulator. On failure returns false and explains why.
    bool init(GSRegisters *privRegs, const Options &options, Description &outDescription, std::string &outError);
    bool active() noexcept;

    // One GIF packet as the arbiter delivers it (path 1..3, multiple of 16 bytes).
    void gifTransfer(uint32_t path, const uint8_t *data, size_t size);

    // Register writes that did not arrive in a GIF packet (HLE stubs).
    void writeRegister(uint8_t reg, uint64_t value);

    // The display (CRTC) registers for one scan-out, captured on the game thread at VBlank. The
    // scan-out itself runs later (on the VU1 thread), when the live registers can already belong
    // to the next field: reading them then paired a buffer with the wrong field - inverted
    // deinterlacing (jagged, doubled text) and sometimes the wrong buffer.
    struct Scanout
    {
        uint64_t pmode, smode1, smode2, srfsh, synch1, synch2, syncv;
        uint64_t dispfb1, display1, dispfb2, display2, extbuf, extdata, extwrite, bgcolor;
        uint64_t csr, imr, busdir, siglblid;
    };
    Scanout captureScanout(const GSRegisters &regs);

    // VBlank: flush the GPU work, scan out the display as captured, queue the read-back of that picture.
    void vsync(const Scanout &scanout);

    // Window thread: the newest picture that came back from the GPU (RGBA8, width x height).
    // Returns false when no new picture arrived since the last call.
    bool takeFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, uint32_t &displayFbp);

    // Waits until everything queued for the GS feed thread has reached paraLLEl-GS.
    void drainFeed();

    // GS -> host transfer data (TRXDIR = 1), for the store-image path.
    uint32_t readLocalToHost(uint8_t *dst, uint32_t bytes);

    // Any thread: the internal resolution (super-sampling rate 1, 4 or 16 = 1x, 2x, 4x scan-out)
    // to switch to at the next VBlank, without restarting.
    void requestSuperSampling(uint32_t rate) noexcept;
    // The rate in use (1 when the GPU renderer is not running).
    uint32_t currentSuperSampling() noexcept;

    void shutdown();
}
