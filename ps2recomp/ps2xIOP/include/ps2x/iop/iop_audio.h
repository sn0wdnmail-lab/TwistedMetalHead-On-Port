#pragma once

#include <cstddef>
#include <cstdint>

namespace ps2x::iop
{
    // 48 kHz interleaved stereo int16 audio produced by the emulated SPU2.
    void pushAudioFrames(const int16_t *interleaved, size_t frames);
    // Copies up to `frames` frames into `interleaved` (zero-filling the rest); returns frames copied.
    size_t pullAudioFrames(int16_t *interleaved, size_t frames);
}
