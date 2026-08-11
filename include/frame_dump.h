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

// Colour emphasis attenuates the two channels it does not name.
//
// MEASURED, not chosen: 0.816328 is lidnariq's figure from terminated 75-ohm
// voltage measurements, recorded on NESdev's NTSC video page under "Brightness
// Levels". The other number in circulation, 0.746, is NOT a hardware
// measurement - it is a constant inside Bisqwit's NTSC square-wave synthesis
// (`static const float attenuation=.746f`), tuned for a full composite decoder
// rather than for an RGB post-lookup renderer like this one.
constexpr double emphasis_attenuation = 0.816328;

// A DELIBERATE DIVERGENCE, in the sense CLAUDE.md means: two sources disagree
// and this picks a side rather than splitting the difference silently.
//
// Mesen's author describes the model most emulators get wrong, in the nesdev
// thread "Most emulators i've tested don't do dual emphasis bits right":
// "When an emphasis bit is active, it multiplies the corresponding RGB
// component by 1.1 and multiplies the other 2 by 0.9. So when all 3 are active,
// you get 1.1x0.9x0.9 = 0.891 for all components."
//
// The COMPOUNDING is taken from that and is the part everyone agrees on: each
// active bit applies its own factor, so a channel named by no active bit is
// attenuated once per bit and all three set darkens the whole picture.
//
// The 1.1 BOOST is not taken. NESdev's Colour emphasis page is explicit that
// boosting the named channel is RGB PPU behaviour - the 2C03/2C04/2C05 in Vs.
// and PlayChoice hardware - while the 2C02 and 2C07 only attenuate. Applying a
// boost here would brighten a 2C02 picture in a way the hardware cannot.
//
// If a future oracle shows the boost is real on the 2C02, this is the line to
// change, and emphasis_all_three_darkens_every_channel is the test that pins
// the current choice.
inline uint32_t apply_emphasis(const uint32_t rgb, const uint8_t index, const uint8_t emphasis)
{
    // "$1D black is affected by color emphasis, but $0F black is not" - only
    // columns $x0-$xD are attenuated, which is also exactly the set blargg's
    // full_palette draws.
    if (emphasis == 0 || (index & 0x0F) > 0x0D) {
        return rgb;
    }

    // Bit 0 emphasises red, so red survives and the other two are attenuated;
    // and so on. On PAL/Dendy bits 0 and 1 would be swapped - this emulator is
    // NTSC (2C02), and that is the only reason the mapping can be hard-coded.
    double channel[3] = {
        static_cast<double>((rgb >> 16) & 0xFF),
        static_cast<double>((rgb >> 8) & 0xFF),
        static_cast<double>(rgb & 0xFF),
    };

    for (int bit = 0; bit < 3; ++bit) {
        if ((emphasis & (1 << bit)) == 0) {
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            if (c != bit) {
                channel[c] *= emphasis_attenuation;
            }
        }
    }

    return (static_cast<uint32_t>(channel[0]) << 16) | (static_cast<uint32_t>(channel[1]) << 8) |
           static_cast<uint32_t>(channel[2]);
}

// Turns one framebuffer pixel - 6-bit palette index plus 3 emphasis bits - into
// 0xRRGGBB.
//
// The index is masked to six bits because that is the width of palette RAM;
// anything wider is a caller bug, and masking makes it a visible wrong colour
// rather than a read off the end of the table.
inline uint32_t palette_index_to_rgb(const uint16_t pixel, const uint32_t* palette, const size_t palette_size)
{
    const size_t entry = pixel & 0x3F;
    if (entry >= palette_size) {
        return 0;
    }
    return apply_emphasis(palette[entry], static_cast<uint8_t>(entry), static_cast<uint8_t>((pixel >> 6) & 0x07));
}

// Writes `indices` as a PPM. Returns false if the file could not be written,
// which the caller is expected to report rather than ignore - a dump that
// silently did not happen is worse than no dump, because it is looked for.
inline bool write_ppm(const std::string& path,
                      const uint16_t* indices,
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
