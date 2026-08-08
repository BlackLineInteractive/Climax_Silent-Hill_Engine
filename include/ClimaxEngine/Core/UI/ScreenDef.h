#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// The front-end screens: `mainmenu.xml`, `pausemenu.xml` and 38 more.
//
// Core code: no GL. Turns the XML into a tree; drawing it is somebody else's
// job, and deliberately so -- the layout carries both widescreen and 4:3
// coordinates, so the renderer picks, not the parser.
//
// The parser is hand-written and small. These files use no namespaces, no
// entities beyond the five standard ones, no CDATA and no processing
// instructions, so a dependency would buy nothing and cost climax-core its
// property of building from zlib and glm alone.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <vector>

namespace ClimaxEngine {
namespace UI {

struct Element {
    std::string tag;                                    // SCREEN, BUTTON, TEXTBOX…
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<Element> children;

    // Attribute lookup. `Attr` returns the fallback when absent, so callers do
    // not have to test every optional one.
    const std::string *Get(const std::string &name) const;
    std::string Attr(const std::string &name, const std::string &fallback = {}) const;
    float Float(const std::string &name, float fallback = 0.0f) const;
    int Int(const std::string &name, int fallback = 0) const;
    bool Bool(const std::string &name, bool fallback = false) const;
    // `colour="0xFFFFFFFF"` — parsed as RGBA in that byte order.
    uint32_t Colour(const std::string &name, uint32_t fallback = 0xFFFFFFFFu) const;

    // Depth-first search by tag, first match or nullptr.
    const Element *Find(const std::string &tag) const;
    // Depth-first search by id attribute.
    const Element *FindById(const std::string &id) const;
};

// Parses a whole document. False on malformed input; `error` then says where.
bool ParseXml(const char *text, size_t size, Element &root, std::string *error = nullptr);
bool ParseXml(const std::string &text, Element &root, std::string *error = nullptr);

} // namespace UI
} // namespace ClimaxEngine
