// Importing saves from a PCSX2 memory card (see runtime/ps2_mc_import.h).
//
// A PS2 memory card file is the card's raw pages: 512 data bytes each, followed by 16 bytes of
// error-correction codes in most dumps (PCSX2's .ps2 files are 8,650,752 bytes = 16,384 x 528).
// Page 0 is the superblock; the file system is a FAT over 1 KB clusters (two pages), reached
// through a list of indirect FAT clusters in the superblock. Directory entries are 512 bytes; a
// directory's first two entries are "." (its length is the number of entries) and "..".
// Cluster numbers in directory entries and in the FAT count from the first allocatable cluster;
// the indirect FAT lists hold absolute cluster numbers.

#include "runtime/ps2_mc_import.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace ps2mcimport
{
    namespace
    {
        constexpr uint16_t kModeFile = 0x0010;
        constexpr uint16_t kModeDirectory = 0x0020;
        constexpr uint16_t kModeExists = 0x8000;
        constexpr uint32_t kChainEnd = 0x7FFFFFFFu;
        constexpr size_t kEntrySize = 512;

        uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
        uint32_t le32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }

        struct Entry
        {
            uint16_t mode = 0;
            uint32_t length = 0;
            uint32_t cluster = 0;
            std::string name;
        };

        class Card
        {
        public:
            bool open(const fs::path &path, std::string &error)
            {
                std::ifstream in(path, std::ios::binary);
                if (!in)
                {
                    error = "The memory card file could not be opened.";
                    return false;
                }
                m_data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                static const char kMagic[] = "Sony PS2 Memory Card Format ";
                if (m_data.size() < 0x154 || std::memcmp(m_data.data(), kMagic, sizeof(kMagic) - 1) != 0)
                {
                    error = "That file is not a PS2 memory card (no PS2 card format inside).";
                    return false;
                }
                const uint8_t *sb = m_data.data();
                m_pageSize = le16(sb + 0x28);
                m_pagesPerCluster = le16(sb + 0x2A);
                const uint32_t clusters = le32(sb + 0x30);
                m_allocOffset = le32(sb + 0x34);
                m_rootCluster = le32(sb + 0x3C);
                for (int i = 0; i < 32; ++i)
                    m_ifc[i] = le32(sb + 0x50 + 4 * i);
                if (m_pageSize != 512 || m_pagesPerCluster == 0 || m_pagesPerCluster > 16 || clusters == 0)
                {
                    error = "This memory card uses a layout the importer does not know.";
                    return false;
                }
                m_clusterSize = m_pageSize * m_pagesPerCluster;
                // With error-correction bytes (16 per page) or without.
                const uint64_t pages = static_cast<uint64_t>(clusters) * m_pagesPerCluster;
                m_rawPage = (m_data.size() >= pages * (m_pageSize + 16u)) ? m_pageSize + 16u : m_pageSize;
                return true;
            }

            std::vector<Entry> directory(uint32_t firstCluster, uint32_t count) const
            {
                std::vector<Entry> entries;
                const uint32_t perCluster = m_clusterSize / kEntrySize;
                std::vector<uint8_t> buffer(m_clusterSize);
                uint32_t cluster = firstCluster;
                for (uint32_t index = 0; index < count;)
                {
                    if (!readCluster(m_allocOffset + cluster, buffer.data()))
                        break;
                    for (uint32_t i = 0; i < perCluster && index < count; ++i, ++index)
                    {
                        const uint8_t *e = buffer.data() + i * kEntrySize;
                        Entry entry;
                        entry.mode = le16(e);
                        entry.length = le32(e + 4);
                        entry.cluster = le32(e + 16);
                        const char *name = reinterpret_cast<const char *>(e + 64);
                        entry.name.assign(name, strnlen(name, 32));
                        entries.push_back(entry);
                    }
                    if (!next(cluster, cluster))
                        break;
                    if (entries.size() > 4096)
                        break; // a broken chain: stop
                }
                return entries;
            }

            // The root directory's entries.
            std::vector<Entry> root() const
            {
                const std::vector<Entry> dot = directory(m_rootCluster, 1);
                if (dot.empty())
                    return {};
                return directory(m_rootCluster, std::min<uint32_t>(dot[0].length, 4096));
            }

            bool readFile(const Entry &file, std::vector<uint8_t> &out) const
            {
                out.clear();
                out.reserve(file.length);
                std::vector<uint8_t> buffer(m_clusterSize);
                uint32_t cluster = file.cluster;
                while (out.size() < file.length)
                {
                    if (!readCluster(m_allocOffset + cluster, buffer.data()))
                        return false;
                    const size_t take = std::min<size_t>(m_clusterSize, file.length - out.size());
                    out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(take));
                    if (out.size() < file.length && !next(cluster, cluster))
                        return false;
                }
                return true;
            }

        private:
            bool readPage(uint32_t page, uint8_t *out) const
            {
                const uint64_t offset = static_cast<uint64_t>(page) * m_rawPage;
                if (offset + m_pageSize > m_data.size())
                    return false;
                std::memcpy(out, m_data.data() + offset, m_pageSize);
                return true;
            }

            bool readCluster(uint32_t absoluteCluster, uint8_t *out) const
            {
                for (uint32_t p = 0; p < m_pagesPerCluster; ++p)
                    if (!readPage(absoluteCluster * m_pagesPerCluster + p, out + p * m_pageSize))
                        return false;
                return true;
            }

            // The FAT entry of an allocatable cluster.
            bool fatEntry(uint32_t cluster, uint32_t &value) const
            {
                const uint32_t perCluster = m_clusterSize / 4;
                const uint32_t fatIndex = cluster % perCluster;
                const uint32_t indirectIndex = cluster / perCluster;
                const uint32_t ifcIndex = indirectIndex / perCluster;
                const uint32_t indirectOffset = indirectIndex % perCluster;
                if (ifcIndex >= 32)
                    return false;
                std::vector<uint8_t> buffer(m_clusterSize);
                if (!readCluster(m_ifc[ifcIndex], buffer.data()))
                    return false;
                const uint32_t fatCluster = le32(buffer.data() + indirectOffset * 4);
                if (!readCluster(fatCluster, buffer.data()))
                    return false;
                value = le32(buffer.data() + fatIndex * 4);
                return true;
            }

            bool next(uint32_t cluster, uint32_t &following) const
            {
                uint32_t value = 0;
                if (!fatEntry(cluster, value) || (value & 0x80000000u) == 0)
                    return false;
                value &= 0x7FFFFFFFu;
                if (value == kChainEnd)
                    return false;
                following = value;
                return true;
            }

            std::vector<uint8_t> m_data;
            uint32_t m_pageSize = 512, m_rawPage = 512, m_pagesPerCluster = 2, m_clusterSize = 1024;
            uint32_t m_allocOffset = 0, m_rootCluster = 0;
            uint32_t m_ifc[32]{};
        };

        // UTF-8 text <-> paths (C++20: u8string is std::u8string).
        fs::path pathOf(const std::string &utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }
        std::string textOf(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        bool startsWith(const std::string &text, const std::string &prefix)
        {
            return text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin(),
                                                             [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); });
        }

        bool safeName(const std::string &name)
        {
            return !name.empty() && name != "." && name != ".." && name.find('/') == std::string::npos &&
                   name.find('\\') == std::string::npos && name.find(':') == std::string::npos;
        }

        std::string stamp()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H-%M-%S", &local);
            return text;
        }

        // A save of the same name already on the port's card goes to saves/backup <time>/ first.
        bool backUp(const fs::path &cardFolder, const std::string &name, const std::string &when)
        {
            std::error_code ec;
            const fs::path existing = cardFolder / pathOf(name);
            if (!fs::exists(existing, ec))
                return true;
            const fs::path backup = cardFolder.parent_path() / pathOf("backup " + when) / cardFolder.filename() / pathOf(name);
            fs::create_directories(backup.parent_path(), ec);
            fs::rename(existing, backup, ec);
            if (ec)
            {
                fs::copy(existing, backup, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                if (ec)
                    return false;
                fs::remove_all(existing, ec);
            }
            return true;
        }

        bool writeFile(const fs::path &path, const std::vector<uint8_t> &bytes)
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(out);
        }

        Result fromCardFile(const fs::path &source, const fs::path &cardFolder, const std::string &prefix)
        {
            Result result;
            Card card;
            if (!card.open(source, result.message))
                return result;
            const std::string when = stamp();
            int skipped = 0;
            for (const Entry &dir : card.root())
            {
                if ((dir.mode & kModeExists) == 0 || (dir.mode & kModeDirectory) == 0 || !safeName(dir.name) || !startsWith(dir.name, prefix))
                    continue;
                if (!backUp(cardFolder, dir.name, when))
                {
                    ++skipped;
                    continue;
                }
                std::error_code ec;
                const fs::path target = cardFolder / pathOf(dir.name);
                fs::create_directories(target, ec);
                bool ok = !ec;
                const std::vector<Entry> files = card.directory(dir.cluster, std::min<uint32_t>(dir.length, 4096));
                std::vector<uint8_t> bytes;
                for (const Entry &file : files)
                {
                    if ((file.mode & kModeExists) == 0 || (file.mode & kModeFile) == 0 || !safeName(file.name))
                        continue; // ".", "..", deleted entries (a save's folder has no sub-folders)
                    if (!card.readFile(file, bytes) || !writeFile(target / pathOf(file.name), bytes))
                        ok = false;
                }
                if (ok)
                    ++result.saves;
                else
                    ++skipped;
            }
            result.ok = result.saves > 0;
            char text[256];
            if (result.saves > 0)
                std::snprintf(text, sizeof(text), "Imported %d save%s from the PCSX2 memory card%s.", result.saves, result.saves == 1 ? "" : "s",
                              skipped ? " (some files could not be read)" : "");
            else if (skipped)
                std::snprintf(text, sizeof(text), "The saves on that card could not be read.");
            else
                std::snprintf(text, sizeof(text), "That memory card has no Twisted Metal: Head-On saves.");
            result.message = text;
            return result;
        }

        Result fromFolderCard(const fs::path &folder, const fs::path &cardFolder, const std::string &prefix)
        {
            Result result;
            std::error_code ec;
            const std::string when = stamp();
            for (const fs::directory_entry &dir : fs::directory_iterator(folder, ec))
            {
                const std::string name = textOf(dir.path().filename());
                if (!dir.is_directory(ec) || !startsWith(name, prefix))
                    continue;
                if (!backUp(cardFolder, name, when))
                    continue;
                const fs::path target = cardFolder / pathOf(name);
                fs::create_directories(target, ec);
                bool ok = true;
                for (const fs::directory_entry &file : fs::directory_iterator(dir.path(), ec))
                {
                    const std::string fileName = textOf(file.path().filename());
                    if (!file.is_regular_file(ec) || fileName.rfind("_pcsx2_", 0) == 0)
                        continue; // PCSX2's own bookkeeping
                    std::error_code copyError;
                    fs::copy_file(file.path(), target / file.path().filename(), fs::copy_options::overwrite_existing, copyError);
                    if (copyError)
                        ok = false;
                }
                if (ok)
                    ++result.saves;
            }
            result.ok = result.saves > 0;
            char text[160];
            if (result.saves > 0)
                std::snprintf(text, sizeof(text), "Imported %d save%s from the PCSX2 folder memory card.", result.saves, result.saves == 1 ? "" : "s");
            else
                std::snprintf(text, sizeof(text), "That folder memory card has no Twisted Metal: Head-On saves.");
            result.message = text;
            return result;
        }
    }

    Result importCard(const fs::path &source, const fs::path &cardFolder, const std::string &prefix)
    {
        std::error_code ec;
        fs::create_directories(cardFolder, ec);
        if (fs::is_directory(source, ec))
            return fromFolderCard(source, cardFolder, prefix);
        // A folder card picked through its superblock file.
        if (source.filename() == "_pcsx2_superblock")
            return fromFolderCard(source.parent_path(), cardFolder, prefix);
        return fromCardFile(source, cardFolder, prefix);
    }
}
