#include "ps2_runtime.h"
#include "games_database.h"
#include "runtime/ps2_launcher.h"
#include "runtime/ps2_perf.h"
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>
#include <cctype>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

namespace
{
    // This port's game, for the setup screen: the disc's boot program must be the one the
    // recompiled code was made from.
    constexpr ps2launcher::GameInfo kGame{
        "TWISTED METAL: HEAD-ON",
        "Extra Twisted Edition  \xC2\xB7  PC port",
        "SCUS-97621",
        "SCUS_976.21",
        2087528ull,
        0x1706384d2dd7ef8full,
    };
    constexpr const char *kGameWindowTitle = "Twisted Metal: Head-On - Extra Twisted Edition";

    // Windows: the file that receives the game's text output (there is no console window), or
    // empty when it goes to a console or to a file a .bat chose.
    std::filesystem::path g_consoleLog;

    // An error that ends the game: logged, and shown in a message box on Windows.
    void fatal(const std::string &message)
    {
        ps2launcher::reportFatalError(kGameWindowTitle, message, g_consoleLog);
    }

#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               fatal("The game stopped because of an unexpected error.");
                               std::abort(); });
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    // Command line: [guest ELF] [--iso <disc image>] [--saves <folder>]. Games that read raw
    // sectors (their own ISO9660 reader, or LBN tables) need the disc image; without --iso, a single
    // .iso beside the ELF's folder or in its parent is used. --saves puts the memory cards
    // (mc0, mc1) in that folder instead of beside the ELF.
    struct LaunchArgs
    {
        std::filesystem::path elf;
        std::filesystem::path iso;
        std::filesystem::path saves;
    };

    LaunchArgs parseLaunchArgs(int argc, char *argv[])
    {
        LaunchArgs args;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i] ? argv[i] : "";
            if ((arg == "--iso" || arg == "-iso") && i + 1 < argc)
            {
                args.iso = std::filesystem::path(argv[++i]);
            }
            else if (arg == "--saves" && i + 1 < argc)
            {
                args.saves = std::filesystem::path(argv[++i]);
            }
            else if (!arg.empty() && args.elf.empty())
            {
                args.elf = std::filesystem::path(arg);
            }
        }
        if (args.iso.empty())
        {
            if (const char *env = std::getenv("PS2X_CD_IMAGE"); env && *env)
            {
                args.iso = std::filesystem::path(env);
            }
        }
        return args;
    }

    std::filesystem::path findDiscImageNear(const std::filesystem::path &elfPath)
    {
        std::error_code ec;
        const std::filesystem::path elfDir = std::filesystem::absolute(elfPath, ec).parent_path();
        for (const std::filesystem::path &dir : {elfDir, elfDir.parent_path()})
        {
            std::vector<std::filesystem::path> images;
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                std::string ext = entry.path().extension().string();
                std::ranges::transform(ext, ext.begin(), [](unsigned char c)
                                       { return static_cast<char>(std::tolower(c)); });
                if (entry.is_regular_file(ec) && ext == ".iso")
                {
                    images.push_back(entry.path());
                }
            }
            if (images.size() == 1u)
            {
                return images.front();
            }
        }
        return {};
    }

    std::filesystem::path getExecutablePath(const LaunchArgs &args)
    {
        if (!args.elf.empty())
        {
            std::cout << "Using argv boot path" << std::endl;
            return args.elf;
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
    g_consoleLog = ps2launcher::setupConsoleOutput(ps2perf::logDirectory());
    setupTerminateLogger();

    try
    {
        LaunchArgs launchArgs = parseLaunchArgs(argc, argv);
        // Started without a game program (double-clicking TMHO.exe): the game setup screen asks for
        // the player's own disc image and builds the game from it.
        const bool setupScreen = launchArgs.elf.empty();
        std::filesystem::path pathObj = setupScreen ? std::filesystem::path() : getExecutablePath(launchArgs);
        std::filesystem::path discImage = setupScreen ? std::filesystem::path()
                                                      : (launchArgs.iso.empty() ? findDiscImageNear(pathObj) : launchArgs.iso);
        if (!discImage.empty())
        {
            PS2Runtime::IoPaths ioPaths = PS2Runtime::getIoPaths();
            ioPaths.cdImage = discImage;
            PS2Runtime::setIoPaths(ioPaths);
            std::cout << "Disc image: " << discImage.string() << std::endl;
        }
        if (setupScreen)
        {
            // Memory cards live in "saves" beside the executable (not in whatever folder it was started from).
            PS2Runtime::IoPaths ioPaths = PS2Runtime::getIoPaths();
            ioPaths.mcRoot = ps2launcher::savesFolder() / "mc0";
            PS2Runtime::setIoPaths(ioPaths);
        }

        std::string windowTitle = std::string(kGameWindowTitle);

        PS2Runtime runtime;
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
        // This hook is to prevent leak rlimgui deps to recompiler etc
        PS2DebugPanel debugPanel;
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->initialize();
            },
            [](PS2Runtime &rt, void *userData)
            {
                static_cast<PS2DebugPanel *>(userData)->draw(rt);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->shutdown();
            },
            &debugPanel);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            fatal("The game window could not be opened. Updating your graphics driver may help.");
            return 1;
        }

        if (setupScreen)
        {
            const ps2launcher::Result setup = ps2launcher::run(kGame);
            if (!setup.play)
            {
                std::cout.flush();
                std::cerr.flush();
                std::_Exit(0); // window closed on the setup screen
            }
            pathObj = setup.program;
            PS2Runtime::IoPaths ioPaths = PS2Runtime::getIoPaths();
            ioPaths.cdImage = setup.disc;
            PS2Runtime::setIoPaths(ioPaths);
            launchArgs.saves = setup.saves;
            std::cout << "Disc image: " << setup.disc.string() << std::endl;
        }

        // Memory cards before loading: loading resets the IOP, which sets up both cards at once.
        if (!launchArgs.saves.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(launchArgs.saves / "mc0", ec);
            std::filesystem::create_directories(launchArgs.saves / "mc1", ec);
            PS2Runtime::IoPaths ioPaths = PS2Runtime::getIoPaths();
            ioPaths.mcRoot = launchArgs.saves / "mc0";
            PS2Runtime::setIoPaths(ioPaths);
            std::cout << "Memory cards: " << launchArgs.saves.string() << std::endl;
        }

        const std::string filePathStr = pathObj.string();
        if (!runtime.loadELF(filePathStr))
        {
            fatal("The game program could not be loaded:\n" + filePathStr);
            return 1;
        }

        runtime.run();

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
        fatal(std::string("The game stopped because of an error:\n") + e.what());
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
        fatal("The game stopped because of an unexpected error.");
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
