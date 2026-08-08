#include "ClimaxEngine/Core/UI/StringTable.h"

#include <cstring>

namespace ClimaxEngine {
namespace UI {

uint32_t StringHash(const char *s) {
    uint32_t h = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        h = (h * 33u) ^ *p;
    return h;
}

namespace {

// UTF-16LE to UTF-8. Written out rather than pulled from a library because
// climax-core carries no dependency beyond zlib and glm, and the input is a
// fixed, known encoding.
void AppendUtf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

uint32_t Rd32(const uint8_t *p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

uint16_t Rd16(const uint8_t *p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

} // namespace

bool StringTable::Load(const uint8_t *data, size_t size) {
    m_byHash.clear();
    m_version = 0;
    if (!data || size < 8)
        return false;

    const uint32_t version = Rd32(data);
    const uint32_t count = Rd32(data + 4);
    // Sanity before trusting the count: the retail files are version 2 with
    // 2115 entries, and a table has to fit its own index.
    if (version == 0 || version > 16 || count == 0 || count > 1u << 20)
        return false;
    const size_t table = 8;
    const size_t blob = table + (size_t)count * 8;
    if (blob > size)
        return false;

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t hash = Rd32(data + table + i * 8);
        const uint32_t at = Rd32(data + table + i * 8 + 4);
        size_t p = blob + (size_t)at * 2;
        if (p + 2 > size)
            continue;

        std::string text;
        while (p + 2 <= size) {
            const uint16_t u = Rd16(data + p);
            p += 2;
            if (u == 0)
                break;
            // Surrogate pair, should it ever appear.
            if (u >= 0xD800 && u < 0xDC00 && p + 2 <= size) {
                const uint16_t lo = Rd16(data + p);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    p += 2;
                    AppendUtf8(text, 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u));
                    continue;
                }
            }
            AppendUtf8(text, u);
        }
        // Later entries win, matching how the table is read in order.
        m_byHash[hash] = std::move(text);
    }

    m_version = version;
    return !m_byHash.empty();
}

const std::string *StringTable::Find(uint32_t hash) const {
    auto it = m_byHash.find(hash);
    return it == m_byHash.end() ? nullptr : &it->second;
}

std::string StringTable::Text(const std::string &id) const {
    const std::string *s = Find(id);
    if (!s)
        return {};
    // Drop the leading U+0001 U+0001, which is not visible text. They arrive as
    // one UTF-8 byte each, so this is a two-byte prefix.
    size_t skip = 0;
    while (skip < s->size() && skip < 2 && (unsigned char)(*s)[skip] == 0x01)
        ++skip;
    return s->substr(skip);
}

} // namespace UI
} // namespace ClimaxEngine
