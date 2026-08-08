#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// The game's UI text: `Strings.Eng` and its five siblings.
//
// Core code: no GL, no SDL, no ImGui. Takes bytes, gives text.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ClimaxEngine {
namespace UI {

// The id the game looks a string up by.
//
// djb2 with xor, seeded with **zero** rather than the textbook 5381 -- which is
// why a first pass over the usual nine hash functions matched nothing. All 59
// ids the front-end XML references resolve, and the 2115 entries of the shipped
// table contain no collisions.
//
// Ghost Rider has a `UTILS::GetStringHash` and it is a different function
// (`h = (h*7) ^ c ^ (h >>> 29)`); it resolves none of them. Shared codebase,
// unshared routine.
uint32_t StringHash(const char *s);
inline uint32_t StringHash(const std::string &s) { return StringHash(s.c_str()); }

// One language's text.
//
//     u32   version        2 in every retail file
//     u32   count          2115 in every retail file
//     count x { u32 hash; u32 charOffset; }   offset in UTF-16 units, not bytes
//     UTF-16LE blob, NUL-terminated strings
//
// Text is exposed as UTF-8. Two things inside it are left alone rather than
// stripped, because a renderer needs them:
//
//   * every string opens with U+0001 U+0001;
//   * U+0003 followed by one unit is a control -- values 1..16 select a glyph
//     from the font's button set, anything else selects one of its sixteen
//     colours.
class StringTable {
public:
    // False when the buffer is not a string table. Never throws, never partially
    // loads: on failure the table is left empty.
    bool Load(const uint8_t *data, size_t size);

    // Text for a hash or an id, or nullptr when absent. The pointer stays valid
    // until the next Load.
    const std::string *Find(uint32_t hash) const;
    const std::string *Find(const std::string &id) const { return Find(StringHash(id)); }

    // Text with the leading U+0001 U+0001 removed, empty when absent. For
    // callers that only want something to draw.
    std::string Text(const std::string &id) const;

    size_t Count() const { return m_byHash.size(); }
    uint32_t Version() const { return m_version; }

private:
    uint32_t m_version = 0;
    std::unordered_map<uint32_t, std::string> m_byHash;
};

} // namespace UI
} // namespace ClimaxEngine
