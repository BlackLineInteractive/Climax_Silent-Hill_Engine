#include "ClimaxEngine/Core/Arc.h"
#include "ClimaxEngine/Core/Common.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <zlib.h>

ArcArchive g_Arc;

namespace {

uint32_t ru32(const uint8_t* p) {
    uint32_t v; std::memcpy(&v, p, 4); return v;
}

uint32_t rbe32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

std::string StripExt(const std::string& s) {
    size_t dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
}

constexpr uint32_t kShsmMagic = 0x0000FA10;

// Trims a run of printable characters out of a buffer, for the name catalogue.
std::string Printable(const uint8_t* p, size_t max) {
    size_t n = 0;
    while (n < max && p[n] >= 0x20 && p[n] < 0x7F) n++;
    return std::string((const char*)p, n);
}

} // namespace

void ArcArchive::Close() {
    if (m_file.is_open()) m_file.close();
    m_entries.clear();
    m_path.clear();
    m_error.clear();
    m_open = false;
}

bool ArcArchive::Open(const std::string& path) {
    Close();

    m_file.open(path, std::ios::binary);
    if (!m_file) { m_error = "cannot open " + path; return false; }

    m_file.seekg(0, std::ios::end);
    const uint64_t fileSize = (uint64_t)m_file.tellg();
    m_file.seekg(0);

    uint8_t hdr[20];
    if (!m_file.read((char*)hdr, sizeof(hdr))) { m_error = "file too small"; Close(); return false; }

    bool ok = false;
    if (std::memcmp(hdr, "A2.0", 4) == 0) {
        m_format = ArcFormat::A2_0;
        ok = OpenA2(fileSize, hdr);
    } else if (ru32(hdr) == kShsmMagic) {
        m_format = ArcFormat::SHSM;
        ok = OpenShsm(fileSize);
    } else {
        m_error = "not a Climax archive (expected \"A2.0\" or 0x0000FA10)";
    }

    if (!ok) {
        // Close() resets m_error, so carry the reason across it -- without this
        // a failed Open reports an empty string.
        const std::string why = m_error;
        Close();
        m_error = why;
        return false;
    }
    m_path = path;
    m_open = true;
    if (m_format == ArcFormat::SHSM) BuildNameCatalogue();
    return true;
}

bool ArcArchive::OpenA2(uint64_t fileSize, const uint8_t* hdr) {
    const uint32_t count     = ru32(hdr + 4);
    const uint32_t nameOff   = ru32(hdr + 12);
    const uint32_t nameSize  = ru32(hdr + 16);

    // The name table always terminates the file; this is the cheapest way to
    // reject a truncated or mis-detected archive before allocating anything.
    if ((uint64_t)nameOff + nameSize != fileSize) {
        m_error = "name table does not end at EOF (truncated archive?)";
        return false;
    }
    if (count == 0 || (uint64_t)20 + (uint64_t)count * 16 > fileSize) {
        m_error = "bad entry count";
        return false;
    }

    std::vector<uint8_t> table((size_t)count * 16);
    m_file.seekg(20);
    if (!m_file.read((char*)table.data(), (std::streamsize)table.size())) {
        m_error = "truncated entry table";
        return false;
    }

    std::vector<char> names(nameSize);
    m_file.seekg(nameOff);
    if (nameSize && !m_file.read(names.data(), (std::streamsize)nameSize)) {
        m_error = "truncated name table";
        return false;
    }

    m_entries.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = table.data() + (size_t)i * 16;
        ArcEntry en;
        const uint32_t no = ru32(e);
        en.offset           = ru32(e + 4);
        en.compressedSize   = ru32(e + 8);
        en.uncompressedSize = ru32(e + 12);

        if ((uint64_t)en.offset + en.compressedSize > fileSize) {
            m_error = "entry " + std::to_string(i) + " points past EOF";
            return false;
        }
        if (no < nameSize) {
            size_t end = no;
            while (end < nameSize && names[end] != '\0') ++end;
            en.name.assign(names.data() + no, end - no);
        }
        m_entries.push_back(std::move(en));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Shattered Memories
//
//   0x00  u32  magic 0x0000FA10
//   0x04  u32  entryCount
//   0x08  u32  firstDataOffset
//   0x0C  u32  reserved (0)
//   then entryCount records of [key][offset][compressedSize][uncompressedSize]
//
// An entry whose payload begins with the same magic is itself an archive, with
// offsets relative to its own start. The ten of those in igc.arc are flattened
// into the entry list here, so the rest of the toolkit never has to know.
// ---------------------------------------------------------------------------
bool ArcArchive::OpenShsm(uint64_t fileSize) {
    auto readTable = [&](uint64_t base, uint64_t limit,
                         std::vector<uint8_t>& table, uint32_t& count) -> bool {
        uint8_t h[16];
        m_file.clear();
        m_file.seekg((std::streamoff)base);
        if (!m_file.read((char*)h, 16)) return false;
        if (ru32(h) != kShsmMagic) return false;
        count = ru32(h + 4);
        const uint32_t firstData = ru32(h + 8);
        if (count == 0 || (uint64_t)16 + (uint64_t)count * 16 > limit) return false;
        // The payloads start at or after the end of the table. The top-level
        // archives leave a padding gap (832 bytes in data.arc, 1504 in igc.arc);
        // the nested ones start immediately.
        if (firstData < 16 + count * 16 || firstData > limit) return false;
        table.resize((size_t)count * 16);
        return (bool)m_file.read((char*)table.data(), (std::streamsize)table.size());
    };

    std::vector<uint8_t> table;
    uint32_t count = 0;
    if (!readTable(0, fileSize, table, count)) {
        m_error = "bad SHSM archive header";
        return false;
    }

    m_entries.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = table.data() + (size_t)i * 16;
        ArcEntry en;
        en.key              = ru32(e);
        en.offset           = ru32(e + 4);
        en.compressedSize   = ru32(e + 8);
        en.uncompressedSize = ru32(e + 12);
        if ((uint64_t)en.offset + en.compressedSize > fileSize) {
            m_error = "entry " + std::to_string(i) + " points past EOF";
            return false;
        }

        // A stored entry that is itself an archive: expand it in place.
        if (en.uncompressedSize == 0 && en.compressedSize > 16) {
            std::vector<uint8_t> sub;
            uint32_t subCount = 0;
            if (readTable(en.offset, en.compressedSize, sub, subCount)) {
                for (uint32_t j = 0; j < subCount; j++) {
                    const uint8_t* se = sub.data() + (size_t)j * 16;
                    ArcEntry s2;
                    s2.key              = ru32(se);
                    s2.offset           = en.offset + ru32(se + 4);
                    s2.compressedSize   = ru32(se + 8);
                    s2.uncompressedSize = ru32(se + 12);
                    if ((uint64_t)ru32(se + 4) + s2.compressedSize > en.compressedSize)
                        continue;
                    s2.name = "sub_" + std::to_string(i) + "/" + std::to_string(j);
                    m_entries.push_back(std::move(s2));
                }
                continue;
            }
        }
        m_entries.push_back(std::move(en));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Name catalogue
//
// The SHSM archives store no names, and the key is not derived from one -- see
// docs/SHSM_ARC_FORMAT.md section 2.4 for what was ruled out. What the payloads
// do carry is enough to label every entry usefully:
//
//   audio bank        its own file name in the first 64 bytes
//   container         the artist's build path, and the names of its textures
//   JPEG / CSV / XML  recognisable from the first bytes
//
// Reading 8 KB of each entry costs about a second on the 1995-entry data.arc,
// which is cheap enough to do at mount time and avoids a cache file that could
// go stale.
// ---------------------------------------------------------------------------
void ArcArchive::BuildNameCatalogue() {
    std::vector<uint8_t> head;
    size_t named = 0;

    for (size_t i = 0; i < m_entries.size(); i++) {
        ArcEntry& e = m_entries[i];
        const std::string fallback =
            e.name.empty() ? ("entry_" + std::to_string(i)) : e.name;

        if (!PeekHead(i, 8192, head) || head.size() < 16) {
            e.name = fallback;
            continue;
        }

        std::string n = NameFromPayload(head);
        if (n.empty()) {
            e.name = fallback;
        } else {
            e.name = n;
            e.derivedName = true;
            named++;
        }
    }
    std::cout << "[arc] SHSM: " << m_entries.size() << " entries, " << named
              << " named from their contents\n";
}

// Inflates (or copies) the first `want` bytes of an entry.
bool ArcArchive::PeekHead(size_t index, size_t want, std::vector<uint8_t>& out) const {
    out.clear();
    if (!m_open || index >= m_entries.size()) return false;
    const ArcEntry& e = m_entries[index];

    const size_t take = std::min<size_t>(e.compressedSize, 1u << 16);
    std::vector<uint8_t> comp(take);
    m_file.clear();
    m_file.seekg(e.offset);
    if (take && !m_file.read((char*)comp.data(), (std::streamsize)take)) return false;

    if (e.uncompressedSize == 0) {
        out.assign(comp.begin(), comp.begin() + std::min(take, want));
        return !out.empty();
    }

    out.resize(std::min<size_t>(want, e.uncompressedSize));
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = comp.data();
    zs.avail_in = (uInt)take;
    zs.next_out = out.data();
    zs.avail_out = (uInt)out.size();
    const int rc = inflate(&zs, Z_NO_FLUSH);
    const size_t got = out.size() - zs.avail_out;
    inflateEnd(&zs);
    if (rc != Z_OK && rc != Z_STREAM_END) { out.clear(); return false; }
    out.resize(got);
    return !out.empty();
}

std::string ArcArchive::NameFromPayload(const std::vector<uint8_t>& d) {
    const uint8_t* p = d.data();
    const size_t n = d.size();
    if (n < 16) return {};

    // Audio bank: a 64-byte NUL-padded file name, then the module.
    if (n >= 64 && (std::memcmp(p, "AFX_", 4) == 0 || std::memcmp(p, "SH1R", 4) == 0)) {
        const std::string s = Printable(p, 63);
        if (s.size() >= 4) return s;
    }
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "image.jpg";
    if (p[0] == '<') return "config.xml";

    // Climax container: label it from the build path, which names the asset and
    // the part of the project tree it came from.
    if (ru32(p) == 0x0716) {
        std::string best;
        for (size_t i = 0; i + 4 < n; i++) {
            if (p[i + 1] != ':' || (p[i + 2] != '\\' && p[i + 2] != '/')) continue;
            const std::string path = Printable(p + i, std::min<size_t>(n - i, 200));
            if (path.size() < 8) continue;
            // "z:\SH1R\Content\RVL\Characters\Adult_Cheryl\Textures"
            // -> "Characters/Adult_Cheryl"
            std::vector<std::string> parts;
            std::string cur;
            for (char ch : path) {
                if (ch == '\\' || ch == '/') { if (!cur.empty()) parts.push_back(cur); cur.clear(); }
                else cur += ch;
            }
            if (!cur.empty()) parts.push_back(cur);
            for (size_t k = 0; k + 1 < parts.size(); k++) {
                const std::string& q = parts[k];
                if (sho_stricmp(q.c_str(), "Content") == 0 ||
                    sho_stricmp(q.c_str(), "Design") == 0) {
                    std::string label;
                    for (size_t m = k + 1; m < parts.size() && m <= k + 4; m++) {
                        if (parts[m].size() > 3 && parts[m][0] == '{') break;
                        if (sho_stricmp(parts[m].c_str(), "RVL") == 0) continue;
                        if (sho_stricmp(parts[m].c_str(), "Build Output") == 0) break;
                        if (!label.empty()) label += '/';
                        label += parts[m];
                    }
                    if (label.size() > best.size()) best = label;
                    break;
                }
            }
        }
        if (!best.empty()) return best;

        // No build path: many containers ship with those strings empty. Walk
        // into the first section instead. A texture dictionary names every
        // texture in its raster header, which is a far better label than the
        // section tag.
        const uint32_t headerSize = rbe32(p + 12);
        const size_t tagLen = rbe32(p + 16);
        std::string tag;
        if (tagLen < 64 && 20 + tagLen + 20 < n) {
            const size_t nameLen = rbe32(p + 20 + tagLen + 16);
            if (nameLen < 64 && 20 + tagLen + 20 + nameLen <= n)
                tag = Printable(p + 20 + tagLen + 20, nameLen);
        }

        const size_t dataStart = 12 + 4 + headerSize;   // [u32 BE size][chunk]
        if (tag == "rwID_TEXDICTIONARY" && dataStart + 8 < n) {
            size_t q = dataStart + 4;
            if (q + 12 <= n && ru32(p + q) == 0x16) {
                q += 12;                                 // into the dictionary
                if (q + 12 <= n && ru32(p + q) == 0x01)  // Struct: count, device
                    q += 12 + ru32(p + q + 4);
                if (q + 12 <= n && ru32(p + q) == 0x15) {   // first TextureNative
                    q += 12;
                    if (q + 12 <= n && ru32(p + q) == 0x01) {
                        const size_t st = q + 12;           // raster struct
                        if (st + 0x38 <= n) {
                            const std::string tn = Printable(p + st + 0x18, 31);
                            if (tn.size() >= 3) return tn;
                        }
                    }
                }
            }
        }
        if (!tag.empty()) return tag;
        return "container";
    }

    // CSV / plain text.
    bool text = true;
    for (size_t i = 0; i < std::min<size_t>(n, 96); i++)
        if (!(p[i] >= 0x20 && p[i] < 0x7F) && p[i] != '\n' && p[i] != '\r' && p[i] != '\t')
            { text = false; break; }
    if (text) {
        const std::string first = Printable(p, std::min<size_t>(n, 40));
        if (!first.empty()) return "table (" + first.substr(0, 24) + "...).csv";
    }
    return {};
}

bool ArcArchive::Read(size_t index, std::vector<uint8_t>& out) const {
    out.clear();
    if (!m_open || index >= m_entries.size()) return false;
    const ArcEntry& e = m_entries[index];

    std::vector<uint8_t> comp(e.compressedSize);
    m_file.clear();
    m_file.seekg(e.offset);
    if (e.compressedSize && !m_file.read((char*)comp.data(), (std::streamsize)comp.size()))
        return false;

    // An entry with uncompressedSize == 0 is stored raw, not deflated. Every
    // payload in SH.ARC is zlib (all 1487 begin 78 DA), but all 35 entries of
    // IGC.ARC are uncompressed cutscene streams that start 10 FF and declare a
    // zero uncompressed size. Inflating those returned Z_DATA_ERROR.
    if (e.uncompressedSize == 0) {
      out.swap(comp);
      return !out.empty();
    }

    out.resize(e.uncompressedSize);
    uLongf destLen = (uLongf)e.uncompressedSize;
    const int rc = uncompress(out.data(), &destLen, comp.data(), (uLong)comp.size());
    if (rc != Z_OK || destLen != e.uncompressedSize) {
        std::cerr << "[arc] inflate failed for '" << e.name << "' (zlib " << rc << ")\n";
        out.clear();
        return false;
    }
    return true;
}

int ArcArchive::Find(const std::string& name) const {
    for (size_t i = 0; i < m_entries.size(); i++)
        if (sho_stricmp(m_entries[i].name.c_str(), name.c_str()) == 0)
            return (int)i;
    return -1;
}

std::vector<int> ArcArchive::Containers() const {
    std::vector<int> out;
    for (size_t i = 0; i < m_entries.size(); i++)
        if (m_entries[i].name.find('.') == std::string::npos)
            out.push_back((int)i);
    return out;
}

std::vector<int> ArcArchive::TxdsFor(const std::string& containerName) const {
    std::vector<int> out;
    if (containerName.empty()) return out;

    for (size_t i = 0; i < m_entries.size(); i++) {
        const std::string& n = m_entries[i].name;
        if (n.size() < 5) continue;
        if (sho_stricmp(n.c_str() + n.size() - 4, ".txd") != 0) continue;

        // Split the stem on '-' and accept the dictionary if any side names
        // this container.
        const std::string stem = StripExt(n);
        size_t start = 0;
        bool match = false;
        while (start <= stem.size()) {
            const size_t dash = stem.find('-', start);
            const std::string part = stem.substr(start, dash - start);
            if (sho_stricmp(part.c_str(), containerName.c_str()) == 0) { match = true; break; }
            if (dash == std::string::npos) break;
            start = dash + 1;
        }
        if (match) out.push_back((int)i);
    }
    return out;
}
