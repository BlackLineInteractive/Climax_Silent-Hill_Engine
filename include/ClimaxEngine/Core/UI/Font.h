#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// `rwID_KFONT` — the game's font metrics. Core code: no GL.
//
// A KFONT payload is a sequence of typed blocks, not one array. Reading it as
// a flat array of glyph records appears to work and is wrong: it yields sixteen
// glyphs, and Font_JAP then has sixteen glyphs too, which no Japanese font does.
//
//     u16 block id, u16 version, u32 size, u16 count, u16 (varies), then data
//
//     id 1   16 x 24 B   controller button glyphs, with UVs into the atlas
//     id 2   16 x  4 B   an RGBA palette: the text colours
//     id 3   n  x  6 B   kerning pairs
//     id 4   n  x 16 B   the character set -- 137 in EUR, 1261 in JAP
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ClimaxEngine {
namespace UI {

// One of the sixteen controller symbols. Addressed by the U+0003 controls in
// the string table: value 1..16 selects index 0..15.
struct ButtonGlyph {
    uint16_t advance = 0;
    int8_t xOffset = 0;
    int8_t yOffset = 0;
    uint8_t width = 0;
    uint8_t height = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

// One character of the alphabet.
struct Glyph {
    uint16_t code = 0;    // UTF-16
    uint16_t advance = 0;
    int8_t xOffset = 0;
    int8_t yOffset = 0;   // negative is up from the baseline
    uint8_t width = 0;
    uint8_t height = 0;
    // Where this character's pixels live in the block's bitmap blob. They are
    // run-length encoded 4-bit alpha; see Rasterise.
    uint32_t dataOffset = 0;
    uint16_t dataLength = 0;
};

struct Rgba {
    uint8_t r = 0, g = 0, b = 0, a = 255;
};

class Font {
public:
    // Reads the `0x1000` chunk that a `rwID_KFONT` section carries. `data`
    // points at the section payload, starting with its u32 size.
    bool Load(const uint8_t *data, size_t size);

    const std::string &Face() const { return m_face; }

    const std::vector<ButtonGlyph> &Buttons() const { return m_buttons; }
    const std::vector<Rgba> &Colours() const { return m_colours; }
    const std::vector<Glyph> &Glyphs() const { return m_glyphs; }

    // Metrics for a character, or nullptr when the font has no glyph for it.
    // Cyrillic returns nullptr on both shipped fonts -- neither carries it,
    // which is the real obstacle to a Ukrainian or Russian translation, not the
    // string table.
    const Glyph *Find(uint16_t code) const;

    // Extra spacing between two characters, zero when the pair is not kerned.
    int16_t Kerning(uint16_t left, uint16_t right) const;

    // Expands a character's bitmap to one byte of coverage per pixel, `width`
    // by `height`, row-major. False when the font has no such character.
    //
    // The stored form is 4-bit alpha, two pixels to a byte with rows padded to
    // a whole byte, run-length encoded PackBits-style: a control byte with the
    // top bit set repeats the following byte 256-c times, otherwise it counts
    // that many literal bytes. Verified by arithmetic rather than by eye --
    // every one of Font_EUR's 134 drawable glyphs and Font_JAP's 1259 expands
    // to exactly the size its width and height demand.
    bool Rasterise(uint16_t code, std::vector<uint8_t> &out, int &width,
                   int &height) const;

    // True when every character of a UTF-8 string has a glyph. `missing`, if
    // given, collects the code points that do not.
    bool CanRender(const std::string &utf8, std::vector<uint32_t> *missing = nullptr) const;

private:
    std::string m_face;
    std::vector<ButtonGlyph> m_buttons;
    std::vector<Rgba> m_colours;
    std::vector<Glyph> m_glyphs;
    std::unordered_map<uint16_t, size_t> m_byCode;
    std::unordered_map<uint32_t, int16_t> m_kerning;   // left << 16 | right
    std::vector<uint8_t> m_bitmaps;                    // the packed glyph pixels
    size_t m_bitmapBase = 0;
};

} // namespace UI
} // namespace ClimaxEngine
