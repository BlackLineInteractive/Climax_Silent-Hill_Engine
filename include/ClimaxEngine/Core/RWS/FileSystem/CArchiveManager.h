#pragma once
#include "CArchive.h"
#include <memory>
#include <vector>

namespace ClimaxEngine {
namespace RWS {
namespace FileSystem {

class CArchiveManager {
public:
    static CArchiveManager& GetInstance() {
        static CArchiveManager instance;
        return instance;
    }

    // Mounts an archive and adds it to the managed list.
    bool Mount(const std::string& path) {
        auto arc = std::make_unique<CArchive>();
        if (arc->Open(path)) {
            m_archives.push_back(std::move(arc));
            return true;
        }
        return false;
    }

    void UnmountAll() {
        m_archives.clear();
    }

    // Searches across all mounted archives for the specified entry.
    // Returns the index within the archive, and sets outArc.
    // Returns -1 if not found.
    int FindEntry(const std::string& name, CArchive** outArc) {
        if (outArc) *outArc = nullptr;
        for (auto& arc : m_archives) {
            int idx = arc->Find(name);
            if (idx >= 0) {
                if (outArc) *outArc = arc.get();
                return idx;
            }
        }
        return -1;
    }

    // Helper for the toolkit UI, which assumes a single active archive.
    CArchive* GetFirstArchive() {
        if (m_archives.empty()) return nullptr;
        return m_archives.front().get();
    }

private:
    CArchiveManager() = default;
    std::vector<std::unique_ptr<CArchive>> m_archives;
};

} // namespace FileSystem
} // namespace RWS
} // namespace ClimaxEngine
