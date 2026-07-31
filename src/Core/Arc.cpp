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

std::string StripExt(const std::string& s) {
    size_t dot = s.rfind('.');
    return dot == std::string::npos ? s : s.substr(0, dot);
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
    if (std::memcmp(hdr, "A2.0", 4) != 0) {
        m_error = "not an A2.0 archive";
        Close();
        return false;
    }

    const uint32_t count     = ru32(hdr + 4);
    const uint32_t nameOff   = ru32(hdr + 12);
    const uint32_t nameSize  = ru32(hdr + 16);

    // The name table always terminates the file; this is the cheapest way to
    // reject a truncated or mis-detected archive before allocating anything.
    if ((uint64_t)nameOff + nameSize != fileSize) {
        m_error = "name table does not end at EOF (truncated archive?)";
        Close();
        return false;
    }
    if (count == 0 || (uint64_t)20 + (uint64_t)count * 16 > fileSize) {
        m_error = "bad entry count";
        Close();
        return false;
    }

    std::vector<uint8_t> table((size_t)count * 16);
    if (!m_file.read((char*)table.data(), (std::streamsize)table.size())) {
        m_error = "truncated entry table";
        Close();
        return false;
    }

    std::vector<char> names(nameSize);
    m_file.seekg(nameOff);
    if (nameSize && !m_file.read(names.data(), (std::streamsize)nameSize)) {
        m_error = "truncated name table";
        Close();
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
            Close();
            return false;
        }
        if (no < nameSize) {
            size_t end = no;
            while (end < nameSize && names[end] != '\0') ++end;
            en.name.assign(names.data() + no, end - no);
        }
        m_entries.push_back(std::move(en));
    }

    m_path = path;
    m_open = true;
    return true;
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
