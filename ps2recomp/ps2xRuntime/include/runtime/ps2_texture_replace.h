#pragma once

// Replacement textures: higher-resolution pictures the GPU renderer samples instead of one of the
// game's own GS textures (so far: the font, from textures\font_atlas.png next to the exe).

#include <cstdint>
#include <string>
#include <vector>

namespace ps2texrep
{
    struct Replacement
    {
        std::string name; // for the log, e.g. "font"
        std::string path; // the picture it came from
        // The GS texture it stands in for (TEX0 fields).
        uint32_t tbp0 = 0, tbw = 0, psm = 0, tw = 0, th = 0, cbp = 0;
        // The replacement: width x height RGBA8 as the GS keeps colours (alpha 0x80 = opaque),
        // rows in the GS texture's own order, with its mip chain (level 0 first).
        uint32_t width = 0, height = 0;
        std::vector<std::vector<uint32_t>> levels;
    };

    // The replacements whose pictures exist next to the exe, loaded and ready to hand to the
    // renderer. Problems go to `notes` (one line each) for the graphics log.
    std::vector<Replacement> loadAll(std::vector<std::string> &notes);
}
