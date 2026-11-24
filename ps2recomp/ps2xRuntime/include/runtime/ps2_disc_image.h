#ifndef PS2_DISC_IMAGE_H
#define PS2_DISC_IMAGE_H

// Files inside the player's disc image (ISO 9660), for when the game's files were not copied out
// of it: the Android version reads straight from the .iso instead of a 4 GB "game" folder. A
// cdrom0: file that is not in the game folder is read from the image (or, for the I/O
// processor's modules, copied out of it once).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ps2disc
{
    struct Entry
    {
        uint64_t offset = 0; // bytes from the start of the image
        uint64_t size = 0;
    };

    // Looks up a file by its path on the disc ("MOD/LIBSD.IRX", any case, '\' or '/', with or
    // without the ";1" version). The image's folders are read once and kept.
    bool find(const std::filesystem::path &image, std::string_view discPath, Entry &entry);

    // Copies a file out of the image to target (folders created as needed). False when the file is
    // not on the disc or could not be written.
    bool extract(const std::filesystem::path &image, std::string_view discPath, const std::filesystem::path &target);

    // Reads size bytes at offset of the image.
    bool read(const std::filesystem::path &image, uint64_t offset, void *destination, size_t size);
}

#endif
