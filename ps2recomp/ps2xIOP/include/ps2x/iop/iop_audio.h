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
}
