#include "runtime/ps2_texture_replace.h"

#include "raylib.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace ps2launcher
{
    std::string executableDirectory(); // ps2_file_dialog.cpp
}

namespace ps2texrep
{
    namespace
    {
        namespace fs = std::filesystem;

        // The game's own textures that can be replaced, and where their pictures go.
        struct Slot
        {
            const char *name;
            const char *file; // relative to the exe
            uint32_t tbp0, tbw, psm, tw, th, cbp;
            bool flipped; // the GS texture is stored bottom-up: pictures are made upright
        };
        // Twisted Metal: Head-On's font: one 512 x 512 PSMT4HH texture (16 x 16 cells of 32 x 32)
        // that every piece of text is drawn from, stored upside down in video memory.
        constexpr Slot kSlots[] = {
            {"font", "textures/font_atlas.png", 1392u, 8u, 0x2Cu, 9u, 9u, 6753u, true},
        };

        fs::path utf8Path(const std::string &text)
        {
            return fs::path(std::u8string(text.begin(), text.end()));
        }

        bool readFile(const fs::path &path, std::vector<unsigned char> &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return !out.empty();
        }

        // Halves a level: 2x2 averages, colours weighted by alpha so transparent texels do not
        // bleed their (arbitrary) colour into the edges of a letter.
        std::vector<uint32_t> halve(const std::vector<uint32_t> &src, uint32_t w, uint32_t h)
        {
            const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
            std::vector<uint32_t> dst(static_cast<size_t>(nw) * nh);
            for (uint32_t y = 0; y < nh; ++y)
                for (uint32_t x = 0; x < nw; ++x)
                {
                    uint32_t sumA = 0, sumR = 0, sumG = 0, sumB = 0, plainR = 0, plainG = 0, plainB = 0;
                    for (uint32_t dy = 0; dy < 2; ++dy)
                        for (uint32_t dx = 0; dx < 2; ++dx)
                        {
                            const uint32_t sx = std::min(w - 1, x * 2 + dx), sy = std::min(h - 1, y * 2 + dy);
                            const uint32_t c = src[static_cast<size_t>(sy) * w + sx];
                            const uint32_t r = c & 0xFFu, g = (c >> 8) & 0xFFu, b = (c >> 16) & 0xFFu, a = c >> 24;
                            sumA += a;
                            sumR += r * a;
                            sumG += g * a;
                            sumB += b * a;
                            plainR += r;
                            plainG += g;
                            plainB += b;
                        }
                    uint32_t r, g, b;
                    if (sumA)
                    {
                        r = (sumR + sumA / 2) / sumA;
                        g = (sumG + sumA / 2) / sumA;
                        b = (sumB + sumA / 2) / sumA;
                    }
                    else
                    {
                        r = (plainR + 2) / 4;
                        g = (plainG + 2) / 4;
                        b = (plainB + 2) / 4;
                    }
                    const uint32_t a = (sumA + 2) / 4;
                    dst[static_cast<size_t>(y) * nw + x] = r | (g << 8) | (b << 16) | (a << 24);
                }
            return dst;
        }

        bool load(const Slot &slot, const std::string &dir, Replacement &out, std::vector<std::string> &notes)
        {
            const std::string path = dir + slot.file;
            std::vector<unsigned char> bytes;
            if (!readFile(utf8Path(path), bytes))
                return false; // no picture: the game's own texture stays
            Image image = LoadImageFromMemory(".png", bytes.data(), static_cast<int>(bytes.size()));
            if (!image.data || image.width <= 0 || image.height <= 0)
            {
                notes.push_back(std::string("could not read ") + path + " (a PNG is needed)");
                return false;
            }
            const uint32_t gsW = 1u << slot.tw, gsH = 1u << slot.th;
            // A power-of-two multiple of the game's texture, 1x to 16x; anything else is resized.
            uint32_t scale = 1;
            while (scale < 16 && gsW * scale * 2 <= static_cast<uint32_t>(image.width) && gsH * scale * 2 <= static_cast<uint32_t>(image.height))
                scale *= 2;
            const uint32_t w = gsW * scale, h = gsH * scale;
            ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            if (static_cast<uint32_t>(image.width) != w || static_cast<uint32_t>(image.height) != h)
            {
                char text[256];
                std::snprintf(text, sizeof(text), "%s is %d x %d: resized to %u x %u (use %u x %u, %u x %u, ... for an exact fit)", path.c_str(), image.width,
                              image.height, w, h, gsW * 2, gsH * 2, gsW * 4, gsH * 4);
                notes.push_back(text);
                ImageResize(&image, static_cast<int>(w), static_cast<int>(h));
            }
            if (slot.flipped)
                ImageFlipVertical(&image);
            std::vector<uint32_t> level(static_cast<size_t>(w) * h);
            const unsigned char *px = static_cast<const unsigned char *>(image.data);
            for (size_t i = 0; i < level.size(); ++i)
            {
                const uint32_t r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2], a = px[i * 4 + 3];
                const uint32_t gsA = (a * 128u + 127u) / 255u; // the GS's alpha: 0x80 is opaque
                level[i] = r | (g << 8) | (b << 16) | (gsA << 24);
            }
            UnloadImage(image);

            out = {};
            out.name = slot.name;
            out.path = path;
            out.tbp0 = slot.tbp0;
            out.tbw = slot.tbw;
            out.psm = slot.psm;
            out.tw = slot.tw;
            out.th = slot.th;
            out.cbp = slot.cbp;
            out.width = w;
            out.height = h;
            // Down to the game's own size and three levels below it (the renderer keeps up to 8).
            uint32_t lw = w, lh = h;
            out.levels.push_back(std::move(level));
            while (out.levels.size() < 8 && lw > 1 && lh > 1 && (lw > gsW / 8 || lh > gsH / 8))
            {
                out.levels.push_back(halve(out.levels.back(), lw, lh));
                lw = std::max(1u, lw / 2);
                lh = std::max(1u, lh / 2);
            }
            return true;
        }
    }

    std::vector<Replacement> loadAll(std::vector<std::string> &notes)
    {
        std::vector<Replacement> result;
        const std::string dir = ps2launcher::executableDirectory();
        for (const Slot &slot : kSlots)
        {
            Replacement r;
            if (load(slot, dir, r, notes))
                result.push_back(std::move(r));
        }
        return result;
    }
}
