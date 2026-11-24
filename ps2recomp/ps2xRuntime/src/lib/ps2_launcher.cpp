// Game setup screen - see include/runtime/ps2_launcher.h.

#include "runtime/ps2_launcher.h"
#include "runtime/ps2_settings.h"
#include "runtime/gs/gs_gpu_renderer.h"
#include "ps2_settings_ui.h"

#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps2launcher
{
    std::string openDiscImageDialog(void *ownerWindow); // ps2_file_dialog.cpp
    std::string executableDirectory();                 // ps2_file_dialog.cpp
    std::string userDataDirectory();                   // ps2_file_dialog.cpp

    namespace
    {
        namespace fs = std::filesystem;
        namespace ui = ps2settings::ui;

        // ---------------------------------------------------------------- files
        fs::path utf8Path(const std::string &text)
        {
            return fs::path(std::u8string(text.begin(), text.end()));
        }

        std::string pathText(const fs::path &path)
        {
            const std::u8string u = path.u8string();
            return std::string(u.begin(), u.end());
        }

        FILE *openFile(const fs::path &path, const char *mode)
        {
#if defined(_WIN32)
            const std::wstring wideMode(mode, mode + std::strlen(mode));
            return _wfopen(path.wstring().c_str(), wideMode.c_str());
#else
            return std::fopen(path.c_str(), mode);
#endif
        }

        bool seekTo(FILE *file, uint64_t offset)
        {
#if defined(_WIN32)
            return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
            return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }

        uint64_t fnv1a(uint64_t hash, const uint8_t *data, size_t size)
        {
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= data[i];
                hash *= 0x100000001b3ull;
            }
            return hash;
        }
        constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull;

        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        std::string megabytes(uint64_t bytes)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
            return text;
        }

        // ---------------------------------------------------------------- ISO 9660
        struct IsoFile
        {
            std::string path; // "LVL/ARENA.DAT"
            uint64_t offset = 0;
            uint64_t size = 0;
        };

        uint32_t le32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
                   static_cast<uint32_t>(p[3]) << 24;
        }

        class Iso
        {
        public:
            ~Iso()
            {
                if (m_file)
                    std::fclose(m_file);
            }

            bool open(const fs::path &path, std::string &error)
            {
                m_file = openFile(path, "rb");
                if (!m_file)
                {
                    error = "The file can't be opened.";
                    return false;
                }
                uint8_t sector[2048];
                for (uint32_t lba = 16; lba < 32; ++lba)
                {
                    if (!read(static_cast<uint64_t>(lba) * 2048u, sector, sizeof(sector)) || std::memcmp(sector + 1, "CD001", 5) != 0)
                        break;
                    if (sector[0] == 1)
                    {
                        m_rootLba = le32(sector + 156 + 2);
                        m_rootSize = le32(sector + 156 + 10);
                        return true;
                    }
                    if (sector[0] == 255)
                        break;
                }
                error = "This file isn't a disc image (no ISO 9660 file system found).";
                return false;
            }

            bool read(uint64_t offset, void *dst, size_t size)
            {
                return seekTo(m_file, offset) && std::fread(dst, 1, size, m_file) == size;
            }

            bool list(std::vector<IsoFile> &files, std::string &error)
            {
                return listDirectory(m_rootLba, m_rootSize, "", files, error, 0);
            }

        private:
            bool listDirectory(uint32_t lba, uint32_t size, const std::string &prefix, std::vector<IsoFile> &files,
                               std::string &error, int depth)
            {
                if (depth > 16 || size > (16u << 20))
                {
                    error = "The disc's folders are damaged.";
                    return false;
                }
                std::vector<uint8_t> dir(size);
                if (!read(static_cast<uint64_t>(lba) * 2048u, dir.data(), dir.size()))
                {
                    error = "The disc image is cut short (it couldn't be read to the end).";
                    return false;
                }
                size_t pos = 0;
                while (pos < dir.size())
                {
                    const uint8_t length = dir[pos];
                    if (length == 0)
                    {
                        pos = (pos / 2048u + 1u) * 2048u; // records never cross a sector
                        continue;
                    }
                    if (length < 34u || pos + length > dir.size())
                        break;
                    const uint8_t *record = dir.data() + pos;
                    pos += length;
                    const uint32_t extent = le32(record + 2);
                    const uint32_t dataLength = le32(record + 10);
                    const uint8_t flags = record[25];
                    const uint8_t nameLength = record[32];
                    if (33u + nameLength > length)
                        continue;
                    std::string name(reinterpret_cast<const char *>(record + 33), nameLength);
                    if (nameLength == 1 && (name[0] == '\0' || name[0] == '\1'))
                        continue; // "." and ".."
                    const size_t version = name.find(';');
                    if (version != std::string::npos)
                        name.resize(version);
                    if (!name.empty() && name.back() == '.')
                        name.pop_back();
                    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos)
                        continue;
                    const std::string path = prefix.empty() ? name : prefix + "/" + name;
                    if (flags & 0x02u)
                    {
                        if (!listDirectory(extent, dataLength, path, files, error, depth + 1))
                            return false;
                    }
                    else
                        files.push_back({path, static_cast<uint64_t>(extent) * 2048u, dataLength});
                }
                return true;
            }

            FILE *m_file = nullptr;
            uint32_t m_rootLba = 0;
            uint32_t m_rootSize = 0;
        };

        const IsoFile *findFile(const std::vector<IsoFile> &files, const std::string &path)
        {
            const std::string wanted = upper(path);
            for (const IsoFile &file : files)
                if (upper(file.path) == wanted)
                    return &file;
            return nullptr;
        }

        // ---------------------------------------------------------------- disc check
        enum class Disc
        {
            None,
            Checking,
            Good,      // the game this port was made from
            Different, // same game, but the game program differs (other version or patch)
            WrongGame,
            Unreadable,
            Missing,   // chosen before, no longer there
        };

        struct DiscResult
        {
            Disc state = Disc::None;
            std::string message;
            std::vector<IsoFile> files;
            uint64_t totalBytes = 0;
            uint64_t programHash = 0;
        };

        DiscResult checkDisc(const fs::path &disc, const GameInfo &game)
        {
            DiscResult out;
            Iso iso;
            std::string error;
            if (!iso.open(disc, error) || !iso.list(out.files, error))
            {
                out.state = Disc::Unreadable;
                out.message = error;
                return out;
            }
            const IsoFile *config = findFile(out.files, "SYSTEM.CNF");
            if (!config || config->size == 0 || config->size > 4096)
            {
                out.state = Disc::WrongGame;
                out.message = "This isn't a PS2 game disc (it has no SYSTEM.CNF).";
                return out;
            }
            std::string text(static_cast<size_t>(config->size), '\0');
            if (!iso.read(config->offset, text.data(), text.size()))
            {
                out.state = Disc::Unreadable;
                out.message = "The disc image couldn't be read.";
                return out;
            }
            // BOOT2 = cdrom0:\SCUS_976.21;1
            std::string boot;
            const size_t key = upper(text).find("BOOT2");
            if (key != std::string::npos)
            {
                const size_t colon = text.find(':', key);
                if (colon != std::string::npos)
                {
                    size_t start = colon + 1;
                    while (start < text.size() && (text[start] == '\\' || text[start] == '/'))
                        ++start;
                    size_t end = start;
                    while (end < text.size() && text[end] != ';' && text[end] != '\r' && text[end] != '\n' && text[end] != ' ')
                        ++end;
                    boot = upper(text.substr(start, end - start));
                }
            }
            if (boot != upper(game.bootFile))
            {
                out.state = Disc::WrongGame;
                out.message = boot.empty() ? std::string("This disc doesn't name a game program.")
                                           : "This is a different game (" + boot + "), not " + game.serial + ".";
                return out;
            }
            const IsoFile *program = findFile(out.files, game.bootFile);
            if (!program)
            {
                out.state = Disc::WrongGame;
                out.message = std::string("The disc's game program (") + game.bootFile + ") is missing.";
                return out;
            }
            std::vector<uint8_t> bytes(static_cast<size_t>(program->size));
            if (!iso.read(program->offset, bytes.data(), bytes.size()))
            {
                out.state = Disc::Unreadable;
                out.message = "The disc image is cut short (it couldn't be read to the end).";
                return out;
            }
            out.programHash = fnv1a(kFnvBasis, bytes.data(), bytes.size());
            for (const IsoFile &file : out.files)
                out.totalBytes += file.size;
            if (program->size == game.programSize && out.programHash == game.programHash)
            {
                out.state = Disc::Good;
                out.message = std::string(game.serial) + " - the version this port was made from";
            }
            else
            {
                out.state = Disc::Different;
                out.message = "Same game, but a different version or patch: it may not run correctly.";
            }
            return out;
        }

        // ---------------------------------------------------------------- settings file
        struct Config
        {
            std::string disc;           // UTF-8 path of the disc image
            bool built = false;
            uint64_t programHash = 0;   // of the game program that was built
            bool showAtStart = true;
        };

        fs::path g_appDir;

        fs::path configPath() { return g_appDir / "launcher.ini"; }

        Config loadConfig()
        {
            Config config;
            FILE *file = openFile(configPath(), "rb");
            if (!file)
                return config;
            char line[4096];
            while (std::fgets(line, sizeof(line), file))
            {
                std::string text(line);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                if (text.empty() || text[0] == '#')
                    continue;
                const size_t eq = text.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string name = text.substr(0, eq);
                const std::string value = text.substr(eq + 1);
                if (name == "disc")
                    config.disc = value;
                else if (name == "built")
                    config.built = value == "1";
                else if (name == "program_hash")
                    config.programHash = std::strtoull(value.c_str(), nullptr, 16);
                else if (name == "show_at_start")
                    config.showAtStart = value != "0";
            }
            std::fclose(file);
            return config;
        }

        void saveConfig(const Config &config)
        {
            FILE *file = openFile(configPath(), "wb");
            if (!file)
                return;
            std::fprintf(file, "# Game setup (written by the setup screen when TMHO.exe starts).\n");
            std::fprintf(file, "disc=%s\n", config.disc.c_str());
            std::fprintf(file, "built=%d\n", config.built ? 1 : 0);
            std::fprintf(file, "program_hash=%016llx\n", static_cast<unsigned long long>(config.programHash));
            std::fprintf(file, "# show_at_start: 1 = this screen at every start, 0 = straight into the game once it is built\n");
            std::fprintf(file, "show_at_start=%d\n", config.showAtStart ? 1 : 0);
            std::fclose(file);
        }

        // ---------------------------------------------------------------- shared setup
        // Shared by every copy of the game on this PC (each folder still has its own launcher.ini):
        // the disc that checked out and the last finished build, so a new copy - another version
        // folder - finds the disc by itself and its Build links the files of the last build.
        struct Shared
        {
            std::string disc;
            std::string build; // a finished "game" folder
        };

        fs::path sharedConfigPath(const GameInfo &game)
        {
            const std::string base = userDataDirectory();
            if (base.empty())
                return {};
            return utf8Path(base) / "PS2 PC ports" / utf8Path(game.serial) / "setup.ini";
        }

        Shared loadShared(const GameInfo &game)
        {
            Shared shared;
            const fs::path path = sharedConfigPath(game);
            FILE *file = path.empty() ? nullptr : openFile(path, "rb");
            if (!file)
                return shared;
            char line[4096];
            while (std::fgets(line, sizeof(line), file))
            {
                std::string text(line);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                const size_t eq = text.find('=');
                if (text.empty() || text[0] == '#' || eq == std::string::npos)
                    continue;
                const std::string name = text.substr(0, eq);
                if (name == "disc")
                    shared.disc = text.substr(eq + 1);
                else if (name == "build")
                    shared.build = text.substr(eq + 1);
            }
            std::fclose(file);
            return shared;
        }

        void saveShared(const GameInfo &game, const Shared &shared)
        {
            const fs::path path = sharedConfigPath(game);
            if (path.empty())
                return;
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            FILE *file = openFile(path, "wb");
            if (!file)
                return;
            std::fprintf(file, "# Shared by every copy of %s on this PC (written by its setup screen).\n", game.title);
            std::fprintf(file, "disc=%s\n", shared.disc.c_str());
            std::fprintf(file, "build=%s\n", shared.build.c_str());
            std::fclose(file);
        }

        uint64_t hashFile(const fs::path &path, uint64_t &size)
        {
            size = 0;
            FILE *file = openFile(path, "rb");
            if (!file)
                return 0;
            uint64_t hash = kFnvBasis;
            std::vector<uint8_t> buffer(1u << 20);
            size_t got;
            while ((got = std::fread(buffer.data(), 1, buffer.size(), file)) > 0)
            {
                hash = fnv1a(hash, buffer.data(), got);
                size += got;
            }
            std::fclose(file);
            return hash;
        }

        // ---------------------------------------------------------------- build
        struct Build
        {
            enum State
            {
                Idle,
                Running,
                Done,
                Failed
            };
            std::atomic<int> state{Idle};
            std::atomic<uint64_t> done{0};
            std::atomic<uint64_t> total{0};
            std::atomic<uint32_t> filesDone{0};
            uint32_t fileCount = 0;
            std::mutex mutex;
            std::string current; // file being copied
            std::string error;
            uint64_t programHash = 0;
            std::atomic<bool> linking{false}; // files come from the last build (hard links), not the disc
        };

        // reuseDir: the "game" folder of the last finished build of this same game program on this PC
        // (another version folder). Its files are hard-linked instead of copied from the disc: no
        // extra disk space and no 4 GB copy. Falls back to copying (another drive, no hard links).
        void runBuild(fs::path disc, std::vector<IsoFile> files, fs::path gameDir, fs::path savesDir, std::string bootFile, fs::path reuseDir, Build *build)
        {
            auto fail = [build](const std::string &message)
            {
                std::lock_guard<std::mutex> lock(build->mutex);
                build->error = message;
                build->state = Build::Failed;
            };
            Iso iso;
            std::string error;
            if (!iso.open(disc, error))
                return fail(error);
            std::error_code ec;
            fs::create_directories(gameDir, ec);
            bool spaceChecked = false; // only what is copied needs space; links take none
            int linkFailures = 0;
            std::vector<uint8_t> buffer(4u << 20);
            for (const IsoFile &file : files)
            {
                {
                    std::lock_guard<std::mutex> lock(build->mutex);
                    build->current = file.path;
                }
                const fs::path target = gameDir / utf8Path(file.path);
                fs::create_directories(target.parent_path(), ec);
                if (!reuseDir.empty())
                {
                    const fs::path source = reuseDir / utf8Path(file.path);
                    std::error_code linkEc;
                    const uintmax_t sourceSize = fs::file_size(source, linkEc);
                    if (!linkEc && sourceSize == file.size)
                    {
                        fs::remove(target, linkEc);
                        linkEc.clear();
                        fs::create_hard_link(source, target, linkEc);
                        if (!linkEc)
                        {
                            build->linking = true;
                            linkFailures = 0;
                            build->done.fetch_add(file.size, std::memory_order_relaxed);
                            build->filesDone.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                    }
                    if (++linkFailures >= 3)
                        reuseDir.clear(); // another drive or no hard links there: copy the rest from the disc
                }
                build->linking = false;
                if (!spaceChecked)
                {
                    spaceChecked = true;
                    const uint64_t remaining = build->total.load() - std::min(build->total.load(), build->done.load());
                    const fs::space_info space = fs::space(gameDir, ec);
                    if (!ec && space.available < remaining + (64ull << 20))
                        return fail("Not enough free space: the game needs " + megabytes(remaining) + " on this drive.");
                }
                fs::path part = target;
                part += ".part";
                FILE *out = openFile(part, "wb");
                if (!out)
                    return fail("Couldn't write " + file.path + " (is the folder read-only?).");
                uint64_t copied = 0;
                while (copied < file.size)
                {
                    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), file.size - copied));
                    if (!iso.read(file.offset + copied, buffer.data(), chunk))
                    {
                        std::fclose(out);
                        fs::remove(part, ec);
                        return fail("The disc image couldn't be read at " + file.path + ".");
                    }
                    if (std::fwrite(buffer.data(), 1, chunk, out) != chunk)
                    {
                        std::fclose(out);
                        fs::remove(part, ec);
                        return fail("Writing " + file.path + " failed (disk full?).");
                    }
                    copied += chunk;
                    build->done.fetch_add(chunk, std::memory_order_relaxed);
                }
                std::fclose(out);
                fs::remove(target, ec);
                fs::rename(part, target, ec);
                if (ec)
                    return fail("Couldn't finish " + file.path + ".");
                build->filesDone.fetch_add(1, std::memory_order_relaxed);
            }
            uint64_t size = 0;
            build->programHash = hashFile(gameDir / utf8Path(bootFile), size);
            if (size == 0)
                return fail("The game program wasn't written.");
            fs::create_directories(savesDir / "mc0", ec);
            fs::create_directories(savesDir / "mc1", ec);
            build->state = Build::Done;
        }

        // ---------------------------------------------------------------- shader builder
        // Compiles the GS shader variants into shadercache.bin beside the executable (see
        // ps2gpu::buildShaderCache), so the game neither compiles them on its first frames nor
        // hitches the first time an effect shows up.
        struct ShaderBuild
        {
            enum State
            {
                Running,
                Done,
                Failed
            };
            std::atomic<int> state{Running};
            ps2gpu::ShaderBuildProgress progress;
            std::mutex mutex;
            std::string gpuName;
            std::string error;
            double startTime = 0.0;
            double seconds = 0.0;
            bool handled = false;
        };

        void runShaderBuild(ShaderBuild *build)
        {
            std::string gpuName, error;
            const bool ok = ps2gpu::buildShaderCache(build->progress, gpuName, error);
            {
                std::lock_guard<std::mutex> lock(build->mutex);
                build->gpuName = gpuName;
                build->error = error;
            }
            build->state = ok ? ShaderBuild::Done : ShaderBuild::Failed;
        }

        std::string seconds(double value)
        {
            char text[32];
            if (value >= 60.0)
                std::snprintf(text, sizeof(text), "%d min %02d s", static_cast<int>(value) / 60, static_cast<int>(value) % 60);
            else if (value >= 10.0)
                std::snprintf(text, sizeof(text), "%.0f s", value);
            else
                std::snprintf(text, sizeof(text), "%.1f s", value);
            return text;
        }

        // ---------------------------------------------------------------- screen
        enum Item
        {
            ItemBrowse,
            ItemBuild,
            ItemShaders,
            ItemShowAtStart,
            ItemPlay,
            ItemCount
        };

        struct Screen
        {
            const GameInfo *game = nullptr;
            Config config;
            fs::path disc;
            Disc discState = Disc::None;
            std::string discMessage;
            std::vector<IsoFile> discFiles;
            uint64_t discBytes = 0;
            uint64_t discProgramHash = 0;
            fs::path reuseDir; // last finished build elsewhere with this disc's game program (Build links it)
            bool built = false;
            bool confirmDifferent = false; // second click on Build for a different version
            int focus = ItemBrowse;
            std::unique_ptr<std::thread> worker;
            std::unique_ptr<Build> build;
            std::unique_ptr<std::thread> shaderWorker;
            std::unique_ptr<ShaderBuild> shaders;
            uint64_t shaderBytes = 0; // shadercache.bin, 0 when there is none
            // disc check running in the background
            std::unique_ptr<std::thread> checker;
            std::mutex checkMutex;
            bool checkReady = false;
            DiscResult checkResult;
        };

        fs::path appDirectory()
        {
            const std::string dir = executableDirectory();
            fs::path path = dir.empty() ? utf8Path(GetApplicationDirectory()) : utf8Path(dir);
            path = path.lexically_normal();
            if (path.filename().empty() && path.has_parent_path())
                path = path.parent_path(); // "...\bin\" -> "...\bin", so parent_path() is the folder above
            return path;
        }

        fs::path gameDir() { return g_appDir / "game"; }
        fs::path savesDir() { return g_appDir / "saves"; }

        void startCheck(Screen &screen, const fs::path &disc)
        {
            if (screen.checker && screen.checker->joinable())
                screen.checker->join();
            screen.disc = disc;
            screen.discState = Disc::Checking;
            screen.discMessage = "Checking the disc...";
            screen.confirmDifferent = false;
            screen.checkReady = false;
            const GameInfo *game = screen.game;
            Screen *target = &screen;
            screen.checker = std::make_unique<std::thread>([target, disc, game]()
                                                           {
                                                               DiscResult result = checkDisc(disc, *game);
                                                               std::lock_guard<std::mutex> lock(target->checkMutex);
                                                               target->checkResult = std::move(result);
                                                               target->checkReady = true; });
        }

        // The last finished build on this PC, when it is another folder and holds the game program
        // of the disc just checked.
        fs::path reusableBuild(const Screen &screen)
        {
            const Shared shared = loadShared(*screen.game);
            if (shared.build.empty() || screen.discProgramHash == 0)
                return {};
            const fs::path dir = utf8Path(shared.build);
            std::error_code ec;
            if (!fs::is_directory(dir, ec) || fs::equivalent(dir, gameDir(), ec))
                return {};
            uint64_t size = 0;
            if (hashFile(dir / utf8Path(screen.game->bootFile), size) != screen.discProgramHash || size == 0)
                return {};
            return dir;
        }

        void pollCheck(Screen &screen)
        {
            std::lock_guard<std::mutex> lock(screen.checkMutex);
            if (!screen.checkReady)
                return;
            screen.checkReady = false;
            screen.discState = screen.checkResult.state;
            screen.discMessage = screen.checkResult.message;
            screen.discFiles = std::move(screen.checkResult.files);
            screen.discBytes = screen.checkResult.totalBytes;
            screen.discProgramHash = screen.checkResult.programHash;
            screen.reuseDir.clear();
            if (screen.discState == Disc::Good || screen.discState == Disc::Different)
            {
                Shared shared = loadShared(*screen.game);
                if (shared.disc != pathText(screen.disc))
                {
                    shared.disc = pathText(screen.disc);
                    saveShared(*screen.game, shared);
                }
                screen.reuseDir = reusableBuild(screen);
            }
            // The same game program was built before: no need to build again.
            uint64_t size = 0;
            const bool sameBuild = screen.config.built && screen.config.programHash == screen.discProgramHash &&
                                   hashFile(gameDir() / utf8Path(screen.game->bootFile), size) == screen.discProgramHash && size > 0;
            if ((screen.discState == Disc::Good || screen.discState == Disc::Different) && sameBuild)
            {
                screen.built = true;
                screen.config.disc = pathText(screen.disc);
                saveConfig(screen.config);
                screen.focus = ItemPlay;
            }
            else
            {
                screen.built = false;
                if (screen.discState == Disc::Good)
                    screen.focus = ItemBuild;
            }
        }

        bool canBuild(const Screen &screen)
        {
            const bool running = screen.build && screen.build->state == Build::Running;
            return !running && (screen.discState == Disc::Good || screen.discState == Disc::Different);
        }

        void startBuild(Screen &screen)
        {
            if (!canBuild(screen))
                return;
            if (screen.discState == Disc::Different && !screen.confirmDifferent)
            {
                screen.confirmDifferent = true; // the button now says "Build anyway"
                return;
            }
            if (screen.worker && screen.worker->joinable())
                screen.worker->join();
            screen.build = std::make_unique<Build>();
            screen.build->total = screen.discBytes;
            screen.build->fileCount = static_cast<uint32_t>(screen.discFiles.size());
            screen.build->state = Build::Running;
            screen.built = false;
            screen.worker = std::make_unique<std::thread>(runBuild, screen.disc, screen.discFiles, gameDir(), savesDir(),
                                                          std::string(screen.game->bootFile), screen.reuseDir, screen.build.get());
        }

        void pollBuild(Screen &screen)
        {
            if (!screen.build || screen.build->state != Build::Done || screen.built)
                return;
            screen.built = true;
            screen.config.disc = pathText(screen.disc);
            screen.config.built = true;
            screen.config.programHash = screen.build->programHash;
            saveConfig(screen.config);
            screen.focus = ItemPlay;
            Shared shared = loadShared(*screen.game);
            shared.disc = pathText(screen.disc);
            shared.build = pathText(gameDir());
            saveShared(*screen.game, shared);
            screen.reuseDir.clear();
        }

        bool shadersRunning(const Screen &screen)
        {
            return screen.shaders && screen.shaders->state == ShaderBuild::Running;
        }

        void startShaderBuild(Screen &screen)
        {
            if (shadersRunning(screen))
            {
                screen.shaders->progress.cancel = true; // the button says "Stop" while it runs
                return;
            }
            if (!ps2gpu::compiledIn())
                return;
            if (screen.shaderWorker && screen.shaderWorker->joinable())
                screen.shaderWorker->join();
            screen.shaders = std::make_unique<ShaderBuild>();
            screen.shaders->startTime = GetTime();
            screen.shaderWorker = std::make_unique<std::thread>(runShaderBuild, screen.shaders.get());
        }

        void pollShaderBuild(Screen &screen)
        {
            if (!screen.shaders || screen.shaders->handled || shadersRunning(screen))
                return;
            screen.shaders->handled = true;
            screen.shaders->seconds = GetTime() - screen.shaders->startTime;
            screen.shaderBytes = ps2gpu::shaderCacheBytes();
            if (screen.shaders->state == ShaderBuild::Done && screen.focus == ItemShaders)
                screen.focus = screen.built ? ItemPlay : ItemBuild;
        }

        void selectDisc(Screen &screen, const std::string &path)
        {
            if (path.empty() || (screen.build && screen.build->state == Build::Running))
                return;
            startCheck(screen, utf8Path(path));
        }

        // First start: look for a single .iso near the executable (also ..\..\Game Rom).
        fs::path findDisc(const GameInfo &game)
        {
            std::error_code ec;
            const fs::path candidates[] = {g_appDir, g_appDir.parent_path(), g_appDir.parent_path().parent_path(),
                                           g_appDir.parent_path().parent_path() / "Game Rom", g_appDir.parent_path() / "Game Rom"};
            for (const fs::path &dir : candidates)
            {
                std::vector<fs::path> images;
                for (const auto &entry : fs::directory_iterator(dir, ec))
                {
                    std::string ext = upper(entry.path().extension().string());
                    if (ext == ".ISO" && entry.is_regular_file(ec))
                        images.push_back(entry.path());
                }
                if (images.size() == 1u)
                    return images.front();
            }
            // The disc another copy of the game on this PC used.
            const Shared shared = loadShared(game);
            if (!shared.disc.empty() && fs::is_regular_file(utf8Path(shared.disc), ec))
                return utf8Path(shared.disc);
            return {};
        }

        // ---- drawing helpers
        struct Look
        {
            float s;
            uint32_t accent;
            Color accentColor;
        };

        const Color kMuted{138, 147, 163, 255};
        const Color kBody{230, 233, 239, 255};
        const Color kGood{63, 185, 80, 255};
        const Color kWarn{227, 179, 65, 255};
        const Color kBad{248, 81, 73, 255};

        bool button(const Look &look, const Rectangle &rect, const char *label, bool focused, bool enabled, bool primary)
        {
            const float s = look.s;
            const bool hover = enabled && CheckCollisionPointRec(GetMousePosition(), rect);
            Color fill = enabled ? (primary || focused || hover ? look.accentColor : Color{48, 54, 66, 255}) : Color{34, 38, 46, 255};
            if (enabled && primary && !(focused || hover))
                fill = ui::rgb(look.accent, 190);
            DrawRectangleRounded(rect, 0.35f, 8, fill);
            if (focused && enabled)
                DrawRectangleRoundedLinesEx(Rectangle{rect.x - 3.0f * s, rect.y - 3.0f * s, rect.width + 6.0f * s, rect.height + 6.0f * s},
                                            0.35f, 8, 2.0f * s, ui::rgb(look.accent, 120));
            const float size = (primary ? 20.0f : 16.0f) * s;
            const float w = ui::textWidth(label, size, true);
            const Color ink = enabled ? ((primary || focused || hover) ? ui::inkFor(look.accent) : WHITE) : Color{100, 106, 118, 255};
            ui::text(label, rect.x + (rect.width - w) * 0.5f, rect.y + (rect.height - size) * 0.5f - 1.0f * s, size, ink, true);
            return hover && ui::clicked();
        }

        void statusIcon(const Look &look, float x, float y, Color color, bool good)
        {
            const float r = 8.0f * look.s;
            DrawCircleV(Vector2{x + r, y + r}, r, color);
            if (good)
            {
                DrawLineEx(Vector2{x + r * 0.55f, y + r * 1.05f}, Vector2{x + r * 0.9f, y + r * 1.4f}, 2.2f * look.s, Color{14, 17, 22, 255});
                DrawLineEx(Vector2{x + r * 0.9f, y + r * 1.4f}, Vector2{x + r * 1.5f, y + r * 0.65f}, 2.2f * look.s, Color{14, 17, 22, 255});
            }
            else
            {
                DrawLineEx(Vector2{x + r, y + r * 0.5f}, Vector2{x + r, y + r * 1.15f}, 2.2f * look.s, Color{14, 17, 22, 255});
                DrawCircleV(Vector2{x + r, y + r * 1.48f}, 1.3f * look.s, Color{14, 17, 22, 255});
            }
        }

        // Fits a long path into width by keeping its end ("...\Game Rom\TMHO.iso").
        std::string fitPath(const std::string &text, float size, float width)
        {
            if (ui::textWidth(text.c_str(), size, false) <= width)
                return text;
            for (size_t cut = 1; cut < text.size(); ++cut)
            {
                const std::string candidate = "..." + text.substr(cut);
                if (ui::textWidth(candidate.c_str(), size, false) <= width)
                    return candidate;
            }
            return text;
        }

        void stepBadge(const Look &look, float x, float y, int number, bool active, bool done)
        {
            const float r = 14.0f * look.s;
            const Vector2 c{x + r, y + r};
            if (done)
                DrawCircleV(c, r, kGood);
            else if (active)
                DrawCircleV(c, r, look.accentColor);
            else
                DrawRing(c, r - 1.5f * look.s, r, 0.0f, 360.0f, 32, Color{90, 98, 112, 255});
            char digit[4];
            std::snprintf(digit, sizeof(digit), "%d", number);
            const float size = 15.0f * look.s;
            const float w = ui::textWidth(digit, size, true);
            const Color ink = done ? Color{14, 17, 22, 255} : active ? ui::inkFor(look.accent) : kMuted;
            ui::text(digit, c.x - w * 0.5f, c.y - size * 0.5f - 1.0f * look.s, size, ink, true);
        }
    }

    std::filesystem::path savesFolder()
    {
        if (g_appDir.empty())
            g_appDir = appDirectory();
        return savesDir();
    }

    void requestSetupAtNextStart()
    {
        if (g_appDir.empty())
            g_appDir = appDirectory();
        Config config = loadConfig();
        config.showAtStart = true;
        saveConfig(config);
    }

    Result run(const GameInfo &game)
    {
        Result result;
        g_appDir = appDirectory();
        ui::ensureFonts();

        Screen screen;
        screen.game = &game;
        screen.config = loadConfig();
        screen.shaderBytes = ps2gpu::shaderCacheBytes();

        // Built before: check that it is still all there.
        uint64_t programSize = 0;
        const fs::path program = gameDir() / utf8Path(game.bootFile);
        std::error_code ec;
        if (screen.config.built && !screen.config.disc.empty())
        {
            const uint64_t hash = hashFile(program, programSize);
            screen.built = programSize > 0 && hash == screen.config.programHash;
            screen.disc = utf8Path(screen.config.disc);
            if (screen.built && fs::exists(screen.disc, ec))
            {
                screen.discState = hash == game.programHash ? Disc::Good : Disc::Different;
                screen.discMessage = screen.discState == Disc::Good ? std::string(game.serial) + " - the version this port was made from"
                                                                    : "Same game, but a different version or patch: it may not run correctly.";
                screen.focus = ItemPlay;
                if (!screen.config.showAtStart && std::getenv("PS2X_SETUP") == nullptr)
                {
                    result.play = true;
                    result.program = program;
                    result.disc = screen.disc;
                    result.saves = savesDir();
                    return result;
                }
            }
            else if (screen.built)
            {
                screen.built = false; // the game needs its disc image for streamed music and movies
                screen.discState = Disc::Missing;
                screen.discMessage = "Your disc image was moved or deleted - choose it again.";
            }
        }
        if (screen.discState == Disc::None)
        {
            const fs::path found = findDisc(game);
            if (!found.empty())
                startCheck(screen, found);
        }
        if (const char *pick = std::getenv("PS2X_SETUP_DISC"); pick && *pick) // tests: as if chosen with Browse
            selectDisc(screen, pick);

        while (!WindowShouldClose() && !ps2settings::quitRequested())
        {
            pollCheck(screen);
            pollBuild(screen);
            pollShaderBuild(screen);
            ps2settings::update(nullptr); // F2 drawer (accent colour, window size...) works here too

            const bool buildRunning = screen.build && screen.build->state == Build::Running;
            const bool shaderRunning = shadersRunning(screen);
            const bool menuOpen = ps2settings::menuOpen();
            bool activate[ItemCount] = {};
            if (!menuOpen)
            {
                if (IsFileDropped())
                {
                    FilePathList dropped = LoadDroppedFiles();
                    if (dropped.count > 0)
                        selectDisc(screen, dropped.paths[0]);
                    UnloadDroppedFiles(dropped);
                }
                auto pad = [](int b)
                {
                    for (int p = 0; p < 4; ++p)
                        if (IsGamepadAvailable(p) && IsGamepadButtonPressed(p, b))
                            return true;
                    return false;
                };
                if (ui::keyRepeat(KEY_DOWN) || pad(GAMEPAD_BUTTON_LEFT_FACE_DOWN))
                    screen.focus = std::min(screen.focus + 1, ItemCount - 1);
                if (ui::keyRepeat(KEY_UP) || pad(GAMEPAD_BUTTON_LEFT_FACE_UP))
                    screen.focus = std::max(screen.focus - 1, 0);
                if (ui::keyTapped(KEY_ENTER) || ui::keyTapped(KEY_KP_ENTER) || ui::keyTapped(KEY_SPACE) || pad(GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
                    activate[screen.focus] = true;
                if (pad(GAMEPAD_BUTTON_MIDDLE_RIGHT))
                    activate[ItemPlay] = true;
            }

            // ---- layout
            const float sw = static_cast<float>(GetScreenWidth());
            const float sh = static_cast<float>(GetScreenHeight());
            Look look;
            look.s = std::clamp(sh / 720.0f, 0.6f, 3.0f);
            look.accent = ui::accent();
            look.accentColor = ui::rgb(look.accent, 255);
            const float s = look.s;
            const float cardW = std::min(sw - 40.0f * s, 700.0f * s);
            const float cardH = std::min(sh - 40.0f * s, 650.0f * s);
            const Rectangle card{std::floor((sw - cardW) * 0.5f), std::floor((sh - cardH) * 0.5f), cardW, cardH};
            const float pad = 32.0f * s;
            const float left = card.x + pad;
            const float right = card.x + card.width - pad;

            BeginDrawing();
            ClearBackground(Color{9, 11, 15, 255});
            // A faint glow of the accent colour behind the card.
            DrawCircleGradient(static_cast<int>(card.x + card.width * 0.15f), static_cast<int>(card.y), card.width * 0.9f,
                               ui::rgb(look.accent, 34), Color{9, 11, 15, 0});
            DrawRectangleRounded(card, 0.035f, 10, Color{14, 17, 22, 250});
            DrawRectangleRoundedLinesEx(card, 0.035f, 10, 1.0f, Color{255, 255, 255, 26});

            // Header.
            float y = card.y + 30.0f * s;
            DrawRectangle(static_cast<int>(left), static_cast<int>(y), static_cast<int>(4.0f * s), static_cast<int>(58.0f * s), look.accentColor);
            ui::text("GAME SETUP", left + 16.0f * s, y - 2.0f * s, 13.0f * s, look.accentColor, true);
            ui::text(game.title, left + 16.0f * s, y + 14.0f * s, 30.0f * s, WHITE, true);
            const std::string subtitle = std::string(game.subtitle) + " " + ps2settings::kPortVersion;
            ui::text(subtitle.c_str(), left + 16.0f * s, y + 46.0f * s, 15.0f * s, kMuted, false);
            const char *f2 = "F2  SETTINGS";
            const float f2Size = 12.0f * s;
            const float f2W = ui::textWidth(f2, f2Size, true) + 16.0f * s;
            const Rectangle f2Box{right - f2W, y + 2.0f * s, f2W, 22.0f * s};
            DrawRectangleRoundedLinesEx(f2Box, 0.3f, 6, 1.0f, Color{255, 255, 255, 60});
            ui::text(f2, f2Box.x + 8.0f * s, f2Box.y + (f2Box.height - f2Size) * 0.5f, f2Size, kMuted, true);
            y += 84.0f * s;
            DrawRectangle(static_cast<int>(left), static_cast<int>(y), static_cast<int>(right - left), 1, Color{255, 255, 255, 26});
            y += 22.0f * s;

            const float buttonW = 150.0f * s;
            const float buttonH = 36.0f * s;
            const float textLeft = left + 44.0f * s;
            const float textWidth = right - buttonW - 20.0f * s - textLeft;
            const bool discOk = screen.discState == Disc::Good || screen.discState == Disc::Different;

            // ---- 1. Game disc
            stepBadge(look, left, y, 1, !discOk, discOk);
            ui::text("Game disc", textLeft, y + 2.0f * s, 18.0f * s, kBody, true);
            {
                const std::string where = screen.disc.empty() ? std::string("Choose your disc image (.iso), or drag it onto this window.")
                                                              : fitPath(pathText(screen.disc), 14.0f * s, textWidth);
                ui::text(where.c_str(), textLeft, y + 26.0f * s, 14.0f * s, kMuted, false);
                if (screen.discState != Disc::None)
                {
                    Color color = kMuted;
                    bool goodIcon = false;
                    switch (screen.discState)
                    {
                    case Disc::Good: color = kGood; goodIcon = true; break;
                    case Disc::Different: color = kWarn; break;
                    case Disc::Checking: color = kMuted; break;
                    default: color = kBad; break;
                    }
                    if (screen.discState != Disc::Checking)
                        statusIcon(look, textLeft, y + 48.0f * s, color, goodIcon);
                    const std::string line = fitPath(screen.discMessage, 14.0f * s, textWidth - 24.0f * s);
                    ui::text(line.c_str(), textLeft + (screen.discState == Disc::Checking ? 0.0f : 24.0f * s), y + 47.0f * s, 14.0f * s,
                             screen.discState == Disc::Checking ? kMuted : color, false);
                }
            }
            const Rectangle browseRect{right - buttonW, y + 4.0f * s, buttonW, buttonH};
            if (button(look, browseRect, screen.disc.empty() ? "Browse..." : "Change...", screen.focus == ItemBrowse, !buildRunning && !menuOpen, false))
                activate[ItemBrowse] = true;
            if (!menuOpen && CheckCollisionPointRec(GetMousePosition(), browseRect) && (GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f))
                screen.focus = ItemBrowse;
            y += 92.0f * s;

            // ---- 2. Build
            stepBadge(look, left, y, 2, discOk && !screen.built, screen.built);
            ui::text("Build", textLeft, y + 2.0f * s, 18.0f * s, kBody, true);
            {
                const bool failed = screen.build && screen.build->state == Build::Failed;
                if (buildRunning)
                {
                    const uint64_t done = screen.build->done.load();
                    const uint64_t total = std::max<uint64_t>(1, screen.build->total.load());
                    const float t = static_cast<float>(static_cast<double>(done) / static_cast<double>(total));
                    const Rectangle track{textLeft, y + 30.0f * s, right - textLeft, 8.0f * s};
                    DrawRectangleRounded(track, 1.0f, 8, Color{40, 45, 56, 255});
                    DrawRectangleRounded(Rectangle{track.x, track.y, std::max(track.height, track.width * t), track.height}, 1.0f, 8, look.accentColor);
                    std::string current;
                    {
                        std::lock_guard<std::mutex> lock(screen.build->mutex);
                        current = screen.build->current;
                    }
                    char line[256];
                    std::snprintf(line, sizeof(line), "%.0f%%  -  %s %s  (%u of %u files)", t * 100.0f,
                                  screen.build->linking.load() ? "linking" : "copying", current.c_str(),
                                  std::min(screen.build->filesDone.load() + 1u, screen.build->fileCount), screen.build->fileCount);
                    ui::text(fitPath(line, 14.0f * s, right - textLeft).c_str(), textLeft, y + 46.0f * s, 14.0f * s, kMuted, false);
                }
                else if (screen.built)
                {
                    statusIcon(look, textLeft, y + 27.0f * s, kGood, true);
                    ui::text("Built - ready to play.", textLeft + 24.0f * s, y + 26.0f * s, 14.0f * s, kGood, false);
                    const std::string where = "Saved in the \"game\" folder beside TMHO.exe.";
                    ui::text(fitPath(where, 14.0f * s, textWidth).c_str(), textLeft, y + 47.0f * s, 14.0f * s, kMuted, false);
                }
                else if (failed)
                {
                    std::string message;
                    {
                        std::lock_guard<std::mutex> lock(screen.build->mutex);
                        message = screen.build->error;
                    }
                    statusIcon(look, textLeft, y + 27.0f * s, kBad, false);
                    ui::text(fitPath(message, 14.0f * s, textWidth - 24.0f * s).c_str(), textLeft + 24.0f * s, y + 26.0f * s, 14.0f * s, kBad, false);
                }
                else
                {
                    std::string line = "Copies the game's files from your disc";
                    if (discOk)
                        line += " (" + megabytes(screen.discBytes) + ")";
                    line += " and gets them ready to play.";
                    if (discOk && !screen.reuseDir.empty())
                        line = "Reuses the files of your last build, so it takes no extra disk space.";
                    ui::text(fitPath(line, 14.0f * s, textWidth).c_str(), textLeft, y + 26.0f * s, 14.0f * s, kMuted, false);
                    if (screen.discState == Disc::Different && screen.confirmDifferent)
                        ui::text("Click again to build this version anyway.", textLeft, y + 47.0f * s, 14.0f * s, kWarn, false);
                }
            }
            if (!buildRunning)
            {
                const char *label = screen.built ? "Build again" : (screen.discState == Disc::Different && screen.confirmDifferent ? "Build anyway" : "Build game");
                const Rectangle buildRect{right - buttonW, y + 4.0f * s, buttonW, buttonH};
                if (button(look, buildRect, label, screen.focus == ItemBuild, canBuild(screen) && !menuOpen, false))
                    activate[ItemBuild] = true;
                if (!menuOpen && CheckCollisionPointRec(GetMousePosition(), buildRect) && (GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f))
                    screen.focus = ItemBuild;
            }
            y += 92.0f * s;

            // ---- 3. Shaders (optional)
            {
                const bool haveCache = screen.shaderBytes > 0;
                const bool failed = screen.shaders && screen.shaders->handled && screen.shaders->state == ShaderBuild::Failed;
                const bool justBuilt = screen.shaders && screen.shaders->handled && screen.shaders->state == ShaderBuild::Done;
                stepBadge(look, left, y, 3, !haveCache && !shaderRunning, haveCache && !shaderRunning);
                ui::text("Shaders", textLeft, y + 2.0f * s, 18.0f * s, kBody, true);
                ui::text("optional", textLeft + ui::textWidth("Shaders", 18.0f * s, true) + 10.0f * s, y + 5.0f * s, 13.0f * s, kMuted, false);
                if (shaderRunning)
                {
                    const uint32_t done = screen.shaders->progress.done.load();
                    const uint32_t total = screen.shaders->progress.total.load();
                    const float t = total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
                    const Rectangle track{textLeft, y + 30.0f * s, right - buttonW - 20.0f * s - textLeft, 8.0f * s};
                    DrawRectangleRounded(track, 1.0f, 8, Color{40, 45, 56, 255});
                    DrawRectangleRounded(Rectangle{track.x, track.y, std::max(track.height, track.width * t), track.height}, 1.0f, 8, look.accentColor);
                    char line[160];
                    if (total == 0)
                        std::snprintf(line, sizeof(line), "Starting the graphics card...");
                    else if (screen.shaders->progress.cancel.load())
                        std::snprintf(line, sizeof(line), "Stopping - keeping the %u shaders already compiled...", done);
                    else
                        std::snprintf(line, sizeof(line), "%.0f%%  -  %u of %u shaders, %u CPU threads, %s", t * 100.0f, done, total,
                                      screen.shaders->progress.threads.load(), seconds(GetTime() - screen.shaders->startTime).c_str());
                    ui::text(fitPath(line, 14.0f * s, track.width).c_str(), textLeft, y + 46.0f * s, 14.0f * s, kMuted, false);
                }
                else if (failed)
                {
                    std::string message;
                    {
                        std::lock_guard<std::mutex> lock(screen.shaders->mutex);
                        message = screen.shaders->error;
                    }
                    statusIcon(look, textLeft, y + 27.0f * s, kBad, false);
                    ui::text(fitPath(message, 14.0f * s, textWidth - 24.0f * s).c_str(), textLeft + 24.0f * s, y + 26.0f * s, 14.0f * s, kBad, false);
                }
                else if (haveCache)
                {
                    std::string line;
                    if (justBuilt)
                    {
                        std::lock_guard<std::mutex> lock(screen.shaders->mutex);
                        line = "Built in " + seconds(screen.shaders->seconds) + " - " + megabytes(screen.shaderBytes) + " for " + screen.shaders->gpuName + ".";
                    }
                    else
                        line = "Shader cache ready (" + megabytes(screen.shaderBytes) + ") - the game loads it at start.";
                    statusIcon(look, textLeft, y + 27.0f * s, kGood, true);
                    ui::text(fitPath(line, 14.0f * s, textWidth - 24.0f * s).c_str(), textLeft + 24.0f * s, y + 26.0f * s, 14.0f * s, kGood, false);
                    ui::text("Build them again after a graphics driver update.", textLeft, y + 47.0f * s, 14.0f * s, kMuted, false);
                }
                else
                {
                    ui::text(fitPath("Compiles the game's GPU shaders now and saves them,", 14.0f * s, textWidth).c_str(), textLeft, y + 26.0f * s, 14.0f * s, kMuted, false);
                    ui::text(fitPath("so starts are quicker and new effects don't stutter.", 14.0f * s, textWidth).c_str(), textLeft, y + 47.0f * s, 14.0f * s, kMuted, false);
                }
                const char *label = shaderRunning ? "Stop" : (haveCache ? "Rebuild shaders" : "Build shaders");
                const Rectangle shaderRect{right - buttonW, y + 4.0f * s, buttonW, buttonH};
                const bool enabled = ps2gpu::compiledIn() && !menuOpen && !(shaderRunning && screen.shaders->progress.cancel.load());
                if (button(look, shaderRect, label, screen.focus == ItemShaders, enabled, false))
                    activate[ItemShaders] = true;
                if (!menuOpen && CheckCollisionPointRec(GetMousePosition(), shaderRect) && (GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f))
                    screen.focus = ItemShaders;
            }
            y += 92.0f * s;

            // ---- Show this screen at start
            {
                const Rectangle row{left, y - 6.0f * s, right - left, 40.0f * s};
                const bool focused = screen.focus == ItemShowAtStart;
                if (focused)
                {
                    DrawRectangleRounded(row, 0.25f, 6, ui::rgb(look.accent, 36));
                    DrawRectangle(static_cast<int>(row.x), static_cast<int>(row.y + 8.0f * s), static_cast<int>(3.0f * s),
                                  static_cast<int>(row.height - 16.0f * s), look.accentColor);
                }
                ui::text("Show this screen at start", textLeft, y + 4.0f * s, 16.0f * s, kBody, false);
                const bool on = screen.config.showAtStart;
                const float cy = row.y + row.height * 0.5f;
                const Rectangle pill{right - 46.0f * s, cy - 12.0f * s, 46.0f * s, 24.0f * s};
                DrawRectangleRounded(pill, 1.0f, 12, on ? look.accentColor : Color{58, 64, 76, 255});
                DrawCircleV(Vector2{on ? pill.x + pill.width - 12.0f * s : pill.x + 12.0f * s, cy}, 9.0f * s,
                            on ? ui::inkFor(look.accent) : Color{200, 205, 214, 255});
                if (!menuOpen && CheckCollisionPointRec(GetMousePosition(), row))
                {
                    if (GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f)
                        screen.focus = ItemShowAtStart;
                    if (ui::clicked())
                        activate[ItemShowAtStart] = true;
                }
                y += 52.0f * s;
            }

            // ---- Play
            const Rectangle playRect{left, card.y + card.height - pad - 52.0f * s, right - left, 52.0f * s};
            const char *playLabel = !screen.built ? "PLAY  -  build the game first" : (shaderRunning ? "PLAY  -  wait for the shaders" : "PLAY");
            if (button(look, playRect, playLabel, screen.focus == ItemPlay, screen.built && !buildRunning && !shaderRunning && !menuOpen, screen.built))
                activate[ItemPlay] = true;
            if (!menuOpen && CheckCollisionPointRec(GetMousePosition(), playRect) && (GetMouseDelta().x != 0.0f || GetMouseDelta().y != 0.0f))
                screen.focus = ItemPlay;

            // Footer hints.
            const char *hint = "\xE2\x86\x91\xE2\x86\x93  select     Enter  choose     F2  settings";
            const float hintSize = 12.5f * s;
            ui::text(hint, card.x + (card.width - ui::textWidth(hint, hintSize, false)) * 0.5f, card.y + card.height + 10.0f * s, hintSize,
                     Color{100, 108, 122, 255}, false);

            ps2settings::draw(-1.0);
            EndDrawing();

            // ---- actions (after drawing: the file dialog blocks)
            if (activate[ItemBrowse] && !buildRunning)
            {
                const std::string picked = openDiscImageDialog(GetWindowHandle());
                selectDisc(screen, picked);
            }
            if (activate[ItemBuild])
                startBuild(screen);
            if (activate[ItemShaders])
                startShaderBuild(screen);
            if (activate[ItemShowAtStart])
            {
                screen.config.showAtStart = !screen.config.showAtStart;
                saveConfig(screen.config);
            }
            if (activate[ItemPlay] && screen.built && !buildRunning && !shaderRunning)
            {
                result.play = true;
                result.program = program;
                result.disc = screen.disc;
                result.saves = savesDir();
                break;
            }
        }

        if (screen.checker && screen.checker->joinable())
            screen.checker->join();
        if (screen.worker && screen.worker->joinable())
            screen.worker->join(); // closing the window while building: let the copy finish its file
        if (screen.shaderWorker && screen.shaderWorker->joinable())
        {
            if (screen.shaders)
                screen.shaders->progress.cancel = true; // keeps what was compiled
            screen.shaderWorker->join();
        }
        return result;
    }
}
