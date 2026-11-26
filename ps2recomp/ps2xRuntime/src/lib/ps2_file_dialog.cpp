// Native helpers for the game setup screen (open-file dialog, the executable's folder). Kept apart
// from raylib, whose names clash with <windows.h>.

#include "runtime/ps2_launcher.h"
#include "runtime/ps2_android.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <io.h>
#else
#include <unistd.h>
#include <climits>
#endif

namespace ps2launcher
{
    // Folder of the running executable as UTF-8, with a trailing separator.
    std::string executableDirectory()
    {
#if defined(__ANDROID__)
        return ps2android::dataDirectory();
#elif defined(_WIN32)
        wchar_t path[32768];
        const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
        if (length == 0 || length >= sizeof(path) / sizeof(path[0]))
            return {};
        std::wstring wide(path, length);
        const size_t slash = wide.find_last_of(L"\\/");
        wide = slash == std::wstring::npos ? std::wstring() : wide.substr(0, slash + 1);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr);
        return utf8;
#else
        char path[PATH_MAX];
        const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (length <= 0)
            return {};
        std::string text(path, static_cast<size_t>(length));
        const size_t slash = text.find_last_of('/');
        return slash == std::string::npos ? std::string() : text.substr(0, slash + 1);
#endif
    }

    // Per-user application data folder as UTF-8 (Windows: %LOCALAPPDATA%; elsewhere
    // $XDG_DATA_HOME or ~/.local/share), or an empty string.
    std::string userDataDirectory()
    {
#if defined(_WIN32)
        const wchar_t *local = _wgetenv(L"LOCALAPPDATA");
        if (!local || !*local)
            return {};
        const std::wstring wide(local);
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr);
        return utf8;
#else
        if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
            return xdg;
        if (const char *home = std::getenv("HOME"); home && *home)
            return std::string(home) + "/.local/share";
        return {};
#endif
    }

    // Returns the chosen file as UTF-8, or an empty string when cancelled / unavailable.
    std::string openDiscImageDialog(void *ownerWindow)
    {
#if defined(__ANDROID__)
        (void)ownerWindow;
        return {}; // the setup screen lists the disc images it finds on the phone instead
#elif defined(_WIN32)
        wchar_t file[4096] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = static_cast<HWND>(ownerWindow);
        ofn.lpstrFilter = L"PS2 disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = file;
        ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
        ofn.lpstrTitle = L"Choose your Twisted Metal: Head-On disc image";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!GetOpenFileNameW(&ofn))
            return {};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 1)
            return {};
        std::string utf8(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8.data(), bytes, nullptr, nullptr);
        return utf8;
#else
        (void)ownerWindow;
        // Linux: zenity / kdialog when installed; otherwise the window takes a dropped file.
        const char *commands[] = {
            "zenity --file-selection --title='Choose your Twisted Metal: Head-On disc image' --file-filter='PS2 disc images | *.iso *.ISO' 2>/dev/null",
            "kdialog --getopenfilename . '*.iso *.ISO' 2>/dev/null",
        };
        for (const char *command : commands)
        {
            FILE *pipe = popen(command, "r");
            if (!pipe)
                continue;
            char buffer[4096];
            std::string result;
            while (std::fgets(buffer, sizeof(buffer), pipe))
                result += buffer;
            const int status = pclose(pipe);
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
            if (status == 0 && !result.empty())
                return result;
        }
        return {};
#endif
    }

    std::string openMemoryCardDialog(void *ownerWindow)
    {
#if defined(__ANDROID__)
        (void)ownerWindow;
        return {}; // the import looks for the card file in the app's folder and Download instead
#elif defined(_WIN32)
        wchar_t file[4096] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = static_cast<HWND>(ownerWindow);
        ofn.lpstrFilter = L"PCSX2 memory cards (*.ps2, *.mcd, *.bin, _pcsx2_superblock)\0*.ps2;*.mcd;*.mc2;*.bin;_pcsx2_superblock\0All files (*.*)\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrFile = file;
        ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
        ofn.lpstrTitle = L"Choose a PCSX2 memory card (for a folder card: its _pcsx2_superblock file)";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_EXPLORER;
        if (!GetOpenFileNameW(&ofn))
            return {};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 1)
            return {};
        std::string utf8(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, file, -1, utf8.data(), bytes, nullptr, nullptr);
        return utf8;
#else
        (void)ownerWindow;
        const char *commands[] = {
            "zenity --file-selection --title='Choose a PCSX2 memory card (for a folder card: its _pcsx2_superblock file)' "
            "--file-filter='PCSX2 memory cards | *.ps2 *.mcd *.mc2 *.bin _pcsx2_superblock' 2>/dev/null",
            "kdialog --getopenfilename . '*.ps2 *.mcd *.mc2 *.bin _pcsx2_superblock' 2>/dev/null",
        };
        for (const char *command : commands)
        {
            FILE *pipe = popen(command, "r");
            if (!pipe)
                continue;
            char buffer[4096];
            std::string result;
            while (std::fgets(buffer, sizeof(buffer), pipe))
                result += buffer;
            const int status = pclose(pipe);
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
            if (status == 0 && !result.empty())
                return result;
        }
        return {};
#endif
    }

#if defined(_WIN32)
    namespace
    {
        std::wstring widenUtf8(const std::string &text)
        {
            if (text.empty())
                return std::wstring();
            const int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(static_cast<size_t>(count), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), count);
            return wide;
        }
    }
#endif

    std::filesystem::path commandLinePath(int argc, char *argv[], const char *option)
    {
#if defined(_WIN32)
        (void)argc;
        (void)argv;
        int count = 0;
        wchar_t **args = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!args)
            return {};
        const std::wstring flag = widenUtf8(option);
        const std::wstring joined = flag + L"=";
        std::filesystem::path found;
        for (int i = 1; i < count; ++i)
        {
            const std::wstring arg = args[i];
            if (arg == flag && i + 1 < count)
            {
                found = std::filesystem::path(args[i + 1]);
                break;
            }
            if (arg.rfind(joined, 0) == 0)
            {
                found = std::filesystem::path(arg.substr(joined.size()));
                break;
            }
        }
        LocalFree(args);
        return found;
#else
        const std::string flag(option);
        const std::string joined = flag + "=";
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i] ? argv[i] : "";
            if (arg == flag && i + 1 < argc)
                return std::filesystem::path(argv[i + 1]);
            if (arg.rfind(joined, 0) == 0)
                return std::filesystem::path(arg.substr(joined.size()));
        }
        return {};
#endif
    }

    std::filesystem::path setupConsoleOutput(const std::string &logFolder)
    {
#if defined(_WIN32)
        if (const char *console = std::getenv("PS2X_CONSOLE"); console && *console && *console != '0')
        {
            if (AllocConsole())
            {
                (void)std::freopen("CONOUT$", "w", stdout);
                (void)std::freopen("CONOUT$", "w", stderr);
                (void)std::freopen("CONIN$", "r", stdin);
                std::cout.clear();
                std::cerr.clear();
                return std::filesystem::path();
            }
        }

        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        const bool alreadyGoesSomewhere = out != nullptr && out != INVALID_HANDLE_VALUE &&
                                          GetFileType(out) != FILE_TYPE_UNKNOWN && _fileno(stdout) >= 0;
        if (alreadyGoesSomewhere || logFolder.empty())
            return std::filesystem::path();

        const std::filesystem::path folder(widenUtf8(logFolder));
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        const std::filesystem::path log = folder / L"console.txt";
        const std::filesystem::path previous = folder / L"console (previous run).txt";
        std::filesystem::remove(previous, ec);
        std::filesystem::rename(log, previous, ec);

        // Both streams share the file in append mode, so their lines interleave instead of
        // overwriting each other; unbuffered, so nothing is lost if the game stops suddenly
        // (the game only prints a few lines a second).
        if (FILE *create = _wfopen(log.c_str(), L"w"))
            std::fclose(create);
        if (!_wfreopen(log.c_str(), L"a", stdout))
            return std::filesystem::path();
        (void)_wfreopen(log.c_str(), L"a", stderr);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
        std::cout.clear();
        std::cerr.clear();
        return log;
#else
        (void)logFolder;
        return std::filesystem::path();
#endif
    }

    void reportFatalError(const char *title, const std::string &message, const std::filesystem::path &logFile)
    {
        std::fprintf(stderr, "[fatal] %s\n", message.c_str());
        std::fflush(stderr);
#if defined(_WIN32)
        std::wstring text = widenUtf8(message);
        if (!logFile.empty())
            text += L"\n\nDetails: " + logFile.wstring();
        MessageBoxW(nullptr, text.c_str(), widenUtf8(title ? title : "Error").c_str(),
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
#else
        (void)title;
        (void)logFile;
#endif
    }
}
