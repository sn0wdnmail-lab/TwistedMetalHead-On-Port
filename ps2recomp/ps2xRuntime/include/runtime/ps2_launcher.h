#ifndef PS2_LAUNCHER_H
#define PS2_LAUNCHER_H

// Game setup screen, shown when the game is started without a command line (double-clicking
// TMHO.exe). The player chooses their own disc image; "Build game" copies the game's files from
// it next to the executable and checks that the game program is the one this port was made from.
// Then Play boots it. Same look as the F2 settings drawer.

#include <cstdint>
#include <filesystem>
#include <string>

namespace ps2launcher
{
    struct GameInfo
    {
        const char *title;       // e.g. "TWISTED METAL: HEAD-ON"
        const char *subtitle;    // e.g. "Extra Twisted Edition"
        const char *serial;      // e.g. "SCUS-97621"
        const char *bootFile;    // boot program named in SYSTEM.CNF, e.g. "SCUS_976.21"
        uint64_t programSize;    // the game program this port was recompiled from
        uint64_t programHash;    // FNV-1a 64 of it
    };

    struct Result
    {
        bool play = false;
        std::filesystem::path program; // built boot program; its folder serves cdrom0:
        std::filesystem::path disc;    // the disc image (for direct sector reads)
        std::filesystem::path saves;   // memory cards (mc0, mc1 inside)
    };

    // Needs the window (call after PS2Runtime::initialize). Returns at once when the game is
    // built and the player turned off "Show this screen at start".
    Result run(const GameInfo &game);

    // F2 drawer: show the setup screen again at the next start.
    void requestSetupAtNextStart();

    // Memory cards folder used by the setup screen ("saves" next to the executable).
    std::filesystem::path savesFolder();

    // Windows: the game has no console window, so unless its output is already redirected (a .bat
    // with "> file"), stdout and stderr go to <logFolder>/console.txt; returns that file, or an
    // empty path when nothing was changed. PS2X_CONSOLE=1 opens a console window instead.
    // Elsewhere it does nothing. logFolder is UTF-8.
    std::filesystem::path setupConsoleOutput(const std::string &logFolder);

    // Prints a fatal error to stderr and, on Windows, shows it in a message box (with the log
    // file to look at, when there is one).
    void reportFatalError(const char *title, const std::string &message, const std::filesystem::path &logFile);
}

#endif
