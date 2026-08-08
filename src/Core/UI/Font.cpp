#include "ClimaxEngine/Core/UI/Font.h"

#include <cstring>

namespace ClimaxEngine {
namespace UI {

namespace {

uint32_t Rd32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t Rd16(const uint8_t *p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
float RdF(const uint8_t *p)     { float v;    std::memcpy(&v, p, 4); return v; }

constexpr uint32_t kChunkKFont = 0x1000;

} // namespace

bool Font::Load(const uint8_t *data, size_t size) {
    m_face.clear();
    m_buttons.clear();
    m_colours.clear();
    m_glyphs.clear();
    m_byCode.clear();
    m_kerning.clear();

    if (!data || size < 0x44)
        return false;

    const uint32_t payload = Rd32(data);
    if (payload + 4 > size)
        return false;
    if (Rd32(data + 4) != kChunkKFont)
        return false;

    // The face name sits at a fixed offset in the header, NUL-padded.
    {
        const char *n = (const char *)(data + 0x18);
        size_t len = 0;
        while (len < 16 && n[len]) ++len;
        m_face.assign(n, len);
    }

    const size_t end = 4 + payload;
    size_t p = 0x38;   // first block header

    while (p + 12 <= end) {
        const uint16_t id = Rd16(data + p);
        const uint32_t blockSize = Rd32(data + p + 4);
        const uint16_t count = Rd16(data + p + 8);
        const size_t body = p + 12;
        if (blockSize == 0 || body + blockSize > end)
            break;

        switch (id) {
        case 1: // controller glyphs
            for (uint16_t i = 0; i < count && body + (size_t)(i + 1) * 24 <= end; ++i) {
                const uint8_t *e = data + body + (size_t)i * 24;
                ButtonGlyph g;
                g.advance = Rd16(e + 2);
                g.xOffset = (int8_t)e[4];
                g.yOffset = (int8_t)e[5];
                g.width = e[6];
                g.height = e[7];
                g.u0 = RdF(e + 8);
                g.v0 = RdF(e + 12);
                g.u1 = RdF(e + 16);
                g.v1 = RdF(e + 20);
                m_buttons.push_back(g);
            }
            break;

        case 2: // colour palette
            for (uint16_t i = 0; i < count && body + (size_t)(i + 1) * 4 <= end; ++i) {
                const uint8_t *e = data + body + (size_t)i * 4;
                m_colours.push_back({e[0], e[1], e[2], e[3]});
            }
            break;

        case 3: // kerning pairs
            for (uint16_t i = 0; i < count && body + (size_t)(i + 1) * 6 <= end; ++i) {
                const uint8_t *e = data + body + (size_t)i * 6;
                const uint16_t l = Rd16(e), r = Rd16(e + 2);
                int16_t d;
                std::memcpy(&d, e + 4, 2);
                m_kerning[((uint32_t)l << 16) | r] = d;
            }
            break;

        case 4: { // the character set
            // Four bytes of prologue, then 16 bytes per character.
            const size_t first = body + 4;
            for (uint16_t i = 0; i < count && first + (size_t)(i + 1) * 16 <= end; ++i) {
                const uint8_t *e = data + first + (size_t)i * 16;
                Glyph g;
                g.code = Rd16(e);
                g.advance = Rd16(e + 2);
                g.xOffset = (int8_t)e[4];
                g.yOffset = (int8_t)e[5];
                g.width = e[6];
                g.height = e[7];
                m_byCode[g.code] = m_glyphs.size();
                m_glyphs.push_back(g);
            }
            break;
        }

        default:
            break;   // an unknown block is skipped, not fatal
        }

        p = body + blockSize;
    }

    return !m_glyphs.empty() || !m_buttons.empty();
}

const Glyph *Font::Find(uint16_t code) const {
    auto it = m_byCode.find(code);
    return it == m_byCode.end() ? nullptr : &m_glyphs[it->second];
}

int16_t Font::Kerning(uint16_t left, uint16_t right) const {
    auto it = m_kerning.find(((uint32_t)left << 16) | right);
    return it == m_kerning.end() ? 0 : it->second;
}

bool Font::CanRender(const std::string &utf8, std::vector<uint32_t> *missing) const {
    bool all = true;
    for (size_t i = 0; i < utf8.size();) {
        const unsigned char c = utf8[i];
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        if (i + len > utf8.size()) break;
        for (size_t k = 1; k < len; ++k)
            cp = (cp << 6) | (utf8[i + k] & 0x3F);
        i += len;

        // The two U+0001 markers and the U+0003 controls are not drawn.
        if (cp == 1 || cp == 3) continue;
        if (cp > 0xFFFF || !Find((uint16_t)cp)) {
            all = false;
            if (missing) missing->push_back(cp);
        }
    }
    return all;
}

} // namespace UI
} // namespace ClimaxEngine
