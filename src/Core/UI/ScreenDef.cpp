#include "ClimaxEngine/Core/UI/ScreenDef.h"

#include <cstdlib>

namespace ClimaxEngine {
namespace UI {

// ── Element accessors ────────────────────────────────────────────────────────

const std::string *Element::Get(const std::string &name) const {
    for (const auto &[k, v] : attributes)
        if (k == name)
            return &v;
    return nullptr;
}

std::string Element::Attr(const std::string &name, const std::string &fallback) const {
    const std::string *v = Get(name);
    return v ? *v : fallback;
}

float Element::Float(const std::string &name, float fallback) const {
    const std::string *v = Get(name);
    if (!v || v->empty())
        return fallback;
    return (float)std::strtod(v->c_str(), nullptr);
}

int Element::Int(const std::string &name, int fallback) const {
    const std::string *v = Get(name);
    if (!v || v->empty())
        return fallback;
    return (int)std::strtol(v->c_str(), nullptr, 0);
}

bool Element::Bool(const std::string &name, bool fallback) const {
    const std::string *v = Get(name);
    if (!v || v->empty())
        return fallback;
    return *v == "true" || *v == "1" || *v == "TRUE";
}

uint32_t Element::Colour(const std::string &name, uint32_t fallback) const {
    const std::string *v = Get(name);
    if (!v || v->empty())
        return fallback;
    return (uint32_t)std::strtoul(v->c_str(), nullptr, 0);
}

const Element *Element::Find(const std::string &t) const {
    if (tag == t)
        return this;
    for (const Element &c : children)
        if (const Element *r = c.Find(t))
            return r;
    return nullptr;
}

const Element *Element::FindById(const std::string &id) const {
    if (const std::string *v = Get("id"); v && *v == id)
        return this;
    for (const Element &c : children)
        if (const Element *r = c.FindById(id))
            return r;
    return nullptr;
}

// ── Parser ───────────────────────────────────────────────────────────────────

namespace {

struct Cursor {
    const char *p;
    const char *end;
    bool Eof() const { return p >= end; }
    char Peek() const { return p < end ? *p : '\0'; }
    void Skip() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
    }
    bool Starts(const char *s) const {
        const char *q = p;
        while (*s && q < end && *q == *s) { ++q; ++s; }
        return *s == '\0';
    }
};

bool NameChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':';
}

std::string ReadName(Cursor &c) {
    const char *s = c.p;
    while (c.p < c.end && NameChar(*c.p)) ++c.p;
    return std::string(s, c.p - s);
}

// The five predefined entities; these files use no others.
std::string Unescape(const std::string &in) {
    if (in.find('&') == std::string::npos)
        return in;
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '&') { out += in[i++]; continue; }
        if      (in.compare(i, 4, "&lt;") == 0)   { out += '<';  i += 4; }
        else if (in.compare(i, 4, "&gt;") == 0)   { out += '>';  i += 4; }
        else if (in.compare(i, 5, "&amp;") == 0)  { out += '&';  i += 5; }
        else if (in.compare(i, 6, "&quot;") == 0) { out += '"';  i += 6; }
        else if (in.compare(i, 6, "&apos;") == 0) { out += '\''; i += 6; }
        else { out += in[i++]; }
    }
    return out;
}

// Comments, declarations and processing instructions, none of which carry data.
void SkipNoise(Cursor &c) {
    for (;;) {
        c.Skip();
        if (c.Starts("<!--")) {
            c.p += 4;
            while (c.p < c.end && !c.Starts("-->")) ++c.p;
            if (c.p < c.end) c.p += 3;
        } else if (c.Starts("<?")) {
            c.p += 2;
            while (c.p < c.end && !c.Starts("?>")) ++c.p;
            if (c.p < c.end) c.p += 2;
        } else if (c.Starts("<!")) {
            c.p += 2;
            while (c.p < c.end && *c.p != '>') ++c.p;
            if (c.p < c.end) ++c.p;
        } else {
            return;
        }
    }
}

bool ParseElement(Cursor &c, Element &out, std::string *err);

bool Fail(Cursor &c, std::string *err, const char *what) {
    if (err) *err = std::string(what) + " at byte " + std::to_string((size_t)(c.p - (const char *)0) & 0xFFFFFFF);
    return false;
}

bool ParseElement(Cursor &c, Element &out, std::string *err) {
    SkipNoise(c);
    if (c.Peek() != '<')
        return Fail(c, err, "expected '<'");
    ++c.p;
    out.tag = ReadName(c);
    if (out.tag.empty())
        return Fail(c, err, "empty tag name");

    for (;;) {
        c.Skip();
        if (c.Eof())
            return Fail(c, err, "unterminated tag");
        if (c.Peek() == '/') {              // <tag ... />
            ++c.p;
            if (c.Peek() != '>')
                return Fail(c, err, "expected '>' after '/'");
            ++c.p;
            return true;
        }
        if (c.Peek() == '>') { ++c.p; break; }

        const std::string key = ReadName(c);
        if (key.empty())
            return Fail(c, err, "expected attribute name");
        c.Skip();
        if (c.Peek() != '=')
            return Fail(c, err, "expected '='");
        ++c.p;
        c.Skip();
        const char quote = c.Peek();
        if (quote != '"' && quote != '\'')
            return Fail(c, err, "expected quoted attribute value");
        ++c.p;
        const char *s = c.p;
        while (c.p < c.end && *c.p != quote) ++c.p;
        if (c.Eof())
            return Fail(c, err, "unterminated attribute value");
        out.attributes.emplace_back(key, Unescape(std::string(s, c.p - s)));
        ++c.p;
    }

    // Children until the closing tag. Text content is discarded: none of the
    // forty files carry any, and a renderer would not know what to do with it.
    for (;;) {
        SkipNoise(c);
        if (c.Eof())
            return Fail(c, err, "unterminated element");
        if (c.Starts("</")) {
            c.p += 2;
            const std::string close = ReadName(c);
            c.Skip();
            if (c.Peek() != '>')
                return Fail(c, err, "expected '>' in closing tag");
            ++c.p;
            if (close != out.tag)
                return Fail(c, err, "mismatched closing tag");
            return true;
        }
        if (c.Peek() == '<') {
            Element child;
            if (!ParseElement(c, child, err))
                return false;
            out.children.push_back(std::move(child));
        } else {
            ++c.p;   // stray text
        }
    }
}

} // namespace

bool ParseXml(const char *text, size_t size, Element &root, std::string *error) {
    root = Element{};
    if (!text || size == 0) {
        if (error) *error = "empty document";
        return false;
    }
    Cursor c{text, text + size};
    // A UTF-8 BOM would otherwise be read as part of the first tag.
    if (size >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF)
        c.p += 3;
    return ParseElement(c, root, error);
}

bool ParseXml(const std::string &text, Element &root, std::string *error) {
    return ParseXml(text.data(), text.size(), root, error);
}

} // namespace UI
} // namespace ClimaxEngine
