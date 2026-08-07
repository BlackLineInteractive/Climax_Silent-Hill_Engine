#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace ClimaxEngine {
namespace RWS {
namespace FileSystem {

// GR_PROTO is the August 2006 PSP prototype's GR.ARC -- an early form of A2.0
// with no magic, a 16-byte header, and entry offsets relative to the end of the
// table of contents rather than to the file.
enum class ArcFormat { A2_0, SHSM, GR_PROTO };

struct ArcEntry {
    std::string name;
    uint32_t    offset           = 0;
    uint32_t    compressedSize   = 0;
    uint32_t    uncompressedSize = 0;
    uint32_t    key              = 0;
    bool        derivedName      = false;

    bool     Stored() const { return uncompressedSize == 0; }
    uint32_t Size()   const { return Stored() ? compressedSize : uncompressedSize; }
};

class CArchive {
public:
    bool Open(const std::string& path);
    void Close();

    bool IsOpen() const { return m_open; }
    ArcFormat Format() const { return m_format; }
    const std::string&           Path()    const { return m_path; }
    const std::vector<ArcEntry>& Entries() const { return m_entries; }
    const std::string&           Error()   const { return m_error; }

    bool Read(size_t index, std::vector<uint8_t>& out) const;
    bool PeekHead(size_t index, size_t want, std::vector<uint8_t>& out) const;
    static std::string NameFromPayload(const std::vector<uint8_t>& head);

    int Find(const std::string& name) const;
    std::vector<int> Containers() const;
    std::vector<int> TxdsFor(const std::string& containerName) const;

private:
    bool OpenA2(uint64_t fileSize, const uint8_t* hdr);
    bool OpenProto(uint64_t fileSize, const uint8_t* hdr);
    bool OpenShsm(uint64_t fileSize);
    void BuildNameCatalogue();

    ArcFormat             m_format = ArcFormat::A2_0;
    std::string           m_path;
    std::string           m_error;
    std::vector<ArcEntry> m_entries;
    mutable std::ifstream m_file;
    bool                  m_open = false;
};

} // namespace FileSystem
} // namespace RWS
} // namespace ClimaxEngine
