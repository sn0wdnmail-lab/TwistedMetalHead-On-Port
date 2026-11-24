// Files inside the player's disc image (see runtime/ps2_disc_image.h).

#include "runtime/ps2_disc_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace ps2disc
{
    namespace
    {
        namespace fs = std::filesystem;

        FILE *openRead(const fs::path &path)
        {
#if defined(_WIN32)
            return ::_wfopen(path.c_str(), L"rb");
#else
            return std::fopen(path.string().c_str(), "rb");
#endif
        }

        bool seekTo(FILE *file, uint64_t offset)
        {
#if defined(_WIN32)
            return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
            return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }

        bool readAt(FILE *file, uint64_t offset, void *destination, size_t size)
        {
            return seekTo(file, offset) && std::fread(destination, 1, size, file) == size;
        }

        uint32_t le32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
                   static_cast<uint32_t>(p[3]) << 24;
        }

        // "cdrom0:\MOD\LIBSD.IRX;1" style paths, without the device: "MOD/LIBSD.IRX".
        std::string normalize(std::string_view path)
        {
            std::string out;
            out.reserve(path.size());
            for (char c : path)
                out.push_back(c == '\\' ? '/' : static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            const size_t version = out.find(';');
            if (version != std::string::npos)
                out.resize(version);
            while (!out.empty() && out.front() == '/')
                out.erase(out.begin());
            if (!out.empty() && out.back() == '.')
                out.pop_back();
            return out;
        }

        using Index = std::unordered_map<std::string, Entry>;

        bool listDirectory(FILE *file, uint32_t lba, uint32_t size, const std::string &prefix, Index &index, int depth)
        {
            if (depth > 16 || size > (16u << 20))
                return false;
            std::vector<uint8_t> dir(size);
            if (!readAt(file, static_cast<uint64_t>(lba) * 2048u, dir.data(), dir.size()))
                return false;
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
                name = normalize(name);
                if (name.empty() || name.find('/') != std::string::npos)
                    continue;
                const std::string path = prefix.empty() ? name : prefix + "/" + name;
                if (flags & 0x02u)
                {
                    if (!listDirectory(file, extent, dataLength, path, index, depth + 1))
                        return false;
                }
                else
                    index[path] = Entry{static_cast<uint64_t>(extent) * 2048u, dataLength};
            }
            return true;
        }

        std::mutex g_mutex;
        std::map<std::string, std::shared_ptr<const Index>> g_indexes; // by image path

        std::shared_ptr<const Index> indexOf(const fs::path &image)
        {
            const std::string key = image.string();
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto found = g_indexes.find(key);
                if (found != g_indexes.end())
                    return found->second;
            }
            auto index = std::make_shared<Index>();
            if (FILE *file = openRead(image))
            {
                uint8_t sector[2048];
                for (uint32_t lba = 16; lba < 32; ++lba)
                {
                    if (!readAt(file, static_cast<uint64_t>(lba) * 2048u, sector, sizeof(sector)) ||
                        std::memcmp(sector + 1, "CD001", 5) != 0)
                        break;
                    if (sector[0] == 1)
                    {
                        if (!listDirectory(file, le32(sector + 156 + 2), le32(sector + 156 + 10), "", *index, 0))
                            index->clear();
                        break;
                    }
                    if (sector[0] == 255)
                        break;
                }
                std::fclose(file);
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            g_indexes[key] = index; // an unreadable image is remembered as empty
            return index;
        }
    }

    bool find(const fs::path &image, std::string_view discPath, Entry &entry)
    {
        if (image.empty())
            return false;
        const std::shared_ptr<const Index> index = indexOf(image);
        const auto found = index->find(normalize(discPath));
        if (found == index->end())
            return false;
        entry = found->second;
        return true;
    }

    bool read(const fs::path &image, uint64_t offset, void *destination, size_t size)
    {
        FILE *file = openRead(image);
        if (!file)
            return false;
        const bool ok = readAt(file, offset, destination, size);
        std::fclose(file);
        return ok;
    }

    bool extract(const fs::path &image, std::string_view discPath, const fs::path &target)
    {
        Entry entry;
        if (!find(image, discPath, entry))
            return false;
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        FILE *in = openRead(image);
        if (!in)
            return false;
        fs::path part = target;
        part += ".part";
#if defined(_WIN32)
        FILE *out = ::_wfopen(part.c_str(), L"wb");
#else
        FILE *out = std::fopen(part.string().c_str(), "wb");
#endif
        if (!out)
        {
            std::fclose(in);
            return false;
        }
        std::vector<uint8_t> buffer(1u << 20);
        uint64_t copied = 0;
        bool ok = seekTo(in, entry.offset);
        while (ok && copied < entry.size)
        {
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), entry.size - copied));
            ok = std::fread(buffer.data(), 1, chunk, in) == chunk && std::fwrite(buffer.data(), 1, chunk, out) == chunk;
            copied += chunk;
        }
        std::fclose(in);
        ok = std::fclose(out) == 0 && ok;
        if (!ok)
        {
            fs::remove(part, ec);
            return false;
        }
        fs::remove(target, ec);
        fs::rename(part, target, ec);
        return !ec;
    }
}
