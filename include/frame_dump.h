// Writes a framebuffer of NES palette indices out as a binary PPM (P6).
//
// PPM is chosen because it needs no library at all: a 15-byte ASCII header
// followed by raw RGB triples. Every image viewer and every conversion tool
// reads it, and adding a PNG dependency to look at a picture would be a poor
// trade.
//
// This lives in include/ rather than tests/ because it is not a test concern:
// any frontend wants the same index-to-RGB conversion, and the PPU deliberately
// stores palette indices rather than colours so that the conversion has exactly
// one home.
//
// Nothing here is on a hot path - a dump is a debugging or diagnostic action,
// not something a running emulator does per frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace nes
{

// Turns one palette index into 0xRRGGBB.
//
// The index is masked to six bits because that is the width of palette RAM;
// anything wider is a caller bug, and masking makes it a visible wrong colour
// rather than a read off the end of the table.
inline uint32_t palette_index_to_rgb(const uint8_t index, const uint32_t* palette, const size_t palette_size)
{
    const size_t entry = index & 0x3F;
    return entry < palette_size ? palette[entry] : 0;
}

// Writes `indices` as a PPM. Returns false if the file could not be written,
// which the caller is expected to report rather than ignore - a dump that
// silently did not happen is worse than no dump, because it is looked for.
inline bool write_ppm(const std::string& path,
                      const uint8_t* indices,
                      const int width,
                      const int height,
                      const uint32_t* palette,
                      const size_t palette_size)
{
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }

    // P6 is the binary flavour; "255" is the per-channel maximum.
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);

    bool ok = true;
    for (int i = 0; i < width * height; ++i) {
        const uint32_t rgb = palette_index_to_rgb(indices[i], palette, palette_size);
        const uint8_t triple[3] = {
            static_cast<uint8_t>((rgb >> 16) & 0xFF),
            static_cast<uint8_t>((rgb >> 8) & 0xFF),
            static_cast<uint8_t>(rgb & 0xFF),
        };
        if (std::fwrite(triple, 1, 3, file) != 3) {
            ok = false;
            break;
        }
    }

    // Checked, because a full disk shows up here and nowhere else.
    if (std::fclose(file) != 0) {
        ok = false;
    }
    return ok;
}

}  // namespace nes
