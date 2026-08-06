#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <map>

namespace ClimaxEngine {
namespace ResourceLoader {

struct RwChunk {
    uint32_t type = 0;
    uint32_t size = 0;
    uint32_t version = 0;
    const uint8_t* payload = nullptr;
    
    // Returns true if parsing the 12-byte header succeeded and the payload fits in the buffer
    bool Read(const uint8_t* d, size_t dataSize, size_t offset);
};

class CStreamLoader {
public:
    virtual ~CStreamLoader() = default;
    
    // Process a chunk. Return true on success.
    virtual bool LoadStream(const RwChunk& chunk, const uint8_t* streamStart, size_t streamSize) = 0;
};

class CResourceHandler {
public:
    static CResourceHandler& GetInstance();
    
    void AddLoader(uint32_t typeId, std::shared_ptr<CStreamLoader> loader);
    std::shared_ptr<CStreamLoader> GetLoader(uint32_t typeId) const;
    
    // Parses a buffer containing a stream of RwChunks, dispatching to loaders.
    void ProcessStream(const uint8_t* data, size_t size);
    
private:
    CResourceHandler() = default;
    std::map<uint32_t, std::shared_ptr<CStreamLoader>> m_loaders;
};

// --- Standard Climax / RenderWare Stream Loaders ---

class CWorldStreamLoader : public CStreamLoader {
public:
    bool LoadStream(const RwChunk& chunk, const uint8_t* streamStart, size_t streamSize) override;
};

class CClumpStreamLoader : public CStreamLoader {
public:
    bool LoadStream(const RwChunk& chunk, const uint8_t* streamStart, size_t streamSize) override;
};

class CTexDictionaryStreamLoader : public CStreamLoader {
public:
    bool LoadStream(const RwChunk& chunk, const uint8_t* streamStart, size_t streamSize) override;
};

// Climax Engine specific loaders
class CAudioCuesStreamLoader : public CStreamLoader {
public:
    bool LoadStream(const RwChunk& chunk, const uint8_t* streamStart, size_t streamSize) override;
};

} // namespace ResourceLoader
} // namespace ClimaxEngine
