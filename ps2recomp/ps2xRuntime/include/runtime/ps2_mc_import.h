#ifndef PS2_MC_IMPORT_H
#define PS2_MC_IMPORT_H

// Importing saves from a PCSX2 memory card (V9): an 8 MB card file (.ps2 / .mcd, with or without
// the error-correction bytes) or a PCSX2 folder memory card. The game's save folders on it are
// copied into the port's own card folder (saves/mc0); saves of the same name already there are
// moved to a backup folder first.

#include <filesystem>
#include <string>

namespace ps2mcimport
{
    struct Result
    {
        bool ok = false;
        int saves = 0;       // save folders copied
        std::string message; // for the player (what was copied, or why nothing was)
    };

    // source: the card file, a folder card (or its "_pcsx2_superblock" file). cardFolder: the
    // port's card (saves/mc0). Only folders whose name starts with prefix ("BASCUS-97621") are
    // copied.
    Result importCard(const std::filesystem::path &source, const std::filesystem::path &cardFolder, const std::string &prefix);
}

#endif // PS2_MC_IMPORT_H
