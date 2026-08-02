#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SH.ARC / IGC.ARC reader ("A2.0" archives)
//
// Layout, all little-endian:
//
//   Header, 20 bytes
//     0x00  char  magic[4]          "A2.0"
//     0x04  u32   entryCount
//     0x08  u32   firstDataOffset   (== entries[0].offset, redundant)
//     0x0C  u32   nameTableOffset   (also the end of the entry payloads)
//     0x10  u32   nameTableSize     (nameTableOffset + nameTableSize == file size)
//
//   Entry[entryCount], 16 bytes each, starting at 0x14
//     0x00  u32   nameOffset        byte offset into the name table
//     0x04  u32   offset            absolute, always 0x20-aligned
//     0x08  u32   compressedSize    length of the raw zlib stream
//     0x0C  u32   uncompressedSize
//
//   Name table: NUL-terminated names, one per entry, at nameTableOffset.
//
// Every payload is a raw zlib stream (0x78 0xDA). Verified against the retail
// PS2 SH.ARC: 1487/1487 entries inflate to exactly the declared size, offsets
// are monotonic, and nameTableOffset + nameTableSize equals the file size.
// ---------------------------------------------------------------------------

// Which of the two Climax archive formats an open file turned out to be.
//
//   A2_0  Silent Hill Origins. 20-byte header, a name table at the tail.
//   SHSM  Shattered Memories. 16-byte header magic 0x0000FA10, NO name table:
//         each entry carries a 32-bit resource key instead. Entries may
//         themselves be archives in the same format, so the reader flattens
//         them into the entry list.
//
// See docs/SHSM_ARC_FORMAT.md for the layouts and the verification figures.
enum class ArcFormat { A2_0, SHSM };

struct ArcEntry {
    std::string name;
    uint32_t    offset           = 0;
    uint32_t    compressedSize   = 0;
    uint32_t    uncompressedSize = 0;

    // SHSM only: the archive's 32-bit resource key, and the entry's position in
    // the file. `name` is then a label derived from the payload (see
    // ArcArchive::NameFromPayload), not something the archive stored.
    uint32_t    key              = 0;
    bool        derivedName      = false;

    // uncompressedSize is 0 for raw-stored entries, which is every entry of
    // IGC.ARC. Their real length is the "compressed" one, so anything showing
    // a size to the user has to ask for it this way or the whole archive reads
    // as 0 KB.
    bool     Stored() const { return uncompressedSize == 0; }
    uint32_t Size()   const { return Stored() ? compressedSize : uncompressedSize; }
};

class ArcArchive {
public:
    // Reads the header, entry table and name table. Payloads stay on disk.
    bool Open(const std::string& path);
    void Close();

    bool IsOpen() const { return m_open; }
    ArcFormat Format() const { return m_format; }
    const std::string&           Path()    const { return m_path; }
    const std::vector<ArcEntry>& Entries() const { return m_entries; }
    const std::string&           Error()   const { return m_error; }

    // Inflates entry `index` into `out`. False on any read/inflate mismatch.
    bool Read(size_t index, std::vector<uint8_t>& out) const;

    // Inflates only the first `want` bytes -- enough to identify an entry
    // without paying for a multi-megabyte payload.
    bool PeekHead(size_t index, size_t want, std::vector<uint8_t>& out) const;

    // Derives a display name from the head of a payload. Public so the same
    // rule can label nested content elsewhere.
    static std::string NameFromPayload(const std::vector<uint8_t>& head);

    // Case-insensitive exact lookup. Returns -1 when absent.
    int Find(const std::string& name) const;

    // Entries whose name has no '.' — the level/geometry containers.
    std::vector<int> Containers() const;

    // TXD entries that belong to `containerName`. The game names shared texture
    // dictionaries after the rooms they bridge, e.g. for "MO_1_Room102":
    //   MO_1_Room102.txd, MO_1_Room102-MO_1_PoolArea.txd, MO_1_PoolArea-MO_1_Room102.txd
    std::vector<int> TxdsFor(const std::string& containerName) const;

private:
    bool OpenA2(uint64_t fileSize, const uint8_t* hdr);
    bool OpenShsm(uint64_t fileSize);
    // Reads the head of every SHSM entry and labels it from its content: the
    // archive itself stores no names at all.
    void BuildNameCatalogue();

    ArcFormat             m_format = ArcFormat::A2_0;
    std::string           m_path;
    std::string           m_error;
    std::vector<ArcEntry> m_entries;
    mutable std::ifstream m_file;
    bool                  m_open = false;
};

extern ArcArchive g_Arc;
