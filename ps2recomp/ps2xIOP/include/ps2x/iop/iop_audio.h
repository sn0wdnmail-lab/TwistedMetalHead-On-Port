#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2x::iop
{
    // 48 kHz interleaved stereo int16 audio produced by the emulated SPU2.
    void pushAudioFrames(const int16_t *interleaved, size_t frames);
    // Fills `interleaved` with `frames` frames for the sound card (silence while too little is
    // buffered); returns how many came from the game. The playback tempo follows how much is
    // buffered (about 70 ms): small differences by resampling, larger ones (the game running
    // slow for a while) by time stretching, so the sound neither runs dry nor gets cut off.
    size_t pullAudioFrames(int16_t *interleaved, size_t frames);

    // Running totals for the performance log.
    struct AudioStats
    {
        uint64_t pushedFrames = 0;   // made by the SPU2
        uint64_t pulledFrames = 0;   // handed to the sound card
        uint64_t silentFrames = 0;   // silence handed over because too little was buffered
        uint64_t underruns = 0;      // times the buffer ran dry while playing
        uint64_t droppedFrames = 0;  // thrown away because the buffer was full
        uint64_t clippedSamples = 0; // mixed louder than 16 bits can hold
        uint64_t noiseVoiceWrites = 0;  // NON writes that turned a voice to the noise generator
        uint64_t pitchModWrites = 0;    // PMON writes that turned on pitch modulation
        int64_t bufferedFrames = 0;  // now
        int32_t rateTenthsPermille = 0; // current playback tempo change, 0.01% units
        uint64_t callbacks = 0;      // sound card requests
        uint64_t stretchedFrames = 0; // frames played through time stretching (tempo change without pitch change)
    };
    AudioStats audioStats() noexcept;

    // ---- IOP thread: the I/O processor (sound driver, disc and memory card modules) runs on a
    // thread of its own, a little behind the game, and is caught up whenever the game talks to it.
    // Set before the game starts (settings iop_thread; PS2X_IOP_THREAD=0 also turns it off).
    void setIopThreadEnabled(bool enabled) noexcept;
    // Called on the IOP thread as it starts: the runtime names it, raises its priority and adds it
    // to the profiler.
    void setIopThreadStartHook(void (*hook)()) noexcept;
    // The EE scheduler (game thread), as the game goes idle: brings the IOP up to the EE's time and
    // does what the IOP thread did to the EE's side (EE memory writes, SIF commands). True when that
    // did anything: the game may have something to run now, so it should not wait yet.
    bool catchUpIopBeforeIdle() noexcept;
    // The EE scheduler, about to wait: nothing happens on the EE for the next eeCycles (until its
    // next VBlank or timer), so the IOP thread may run that far ahead meanwhile - the IOP work for
    // that time is then done while the game waits instead of after it.
    void runIopAheadWhileIdle(uint64_t eeCycles) noexcept;
    // Running totals for the performance log.
    struct IopThreadStats
    {
        bool running = false;
        uint64_t busyNs = 0;      // the IOP thread running IOP code
        uint64_t aheadNs = 0;     // ... of that, ahead of the game, while it waited for its next VBlank/timer
        uint64_t batches = 0;     // pieces of game time it ran
        uint64_t syncs = 0;       // the game needed the IOP caught up (RPC calls, SIF transfers, IOP memory, going idle)
        uint64_t syncWaitNs = 0;  // ... and waited this long for the IOP thread's piece to finish
        uint64_t inlineNs = 0;    // IOP time the game thread then ran itself (not run by the IOP thread yet)
        uint64_t lagCatchUps = 0; // the IOP thread fell too far behind and the game thread caught it up
        uint64_t effects = 0;     // EE memory writes and SIF commands from the IOP thread, applied on the game thread
    };
    IopThreadStats iopThreadStats() noexcept;
}
