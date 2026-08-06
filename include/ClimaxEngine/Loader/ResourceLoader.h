#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include "ClimaxEngine/Loader/IStreamLoader.h"
#include "ClimaxEngine/Core/RWS/RwStream.h"

namespace ClimaxEngine {
namespace ResourceLoader {

struct RwChunk {
    uint32_t type = 0;
    uint32_t size = 0;
    uint32_t version = 0;
    const uint8_t* payload = nullptr;
    
    bool Read(const uint8_t* d, size_t dataSize, size_t offset);
    bool Read(RWS::RwStream* stream);
};

class CResourceHandler {
public:
    static CResourceHandler& GetInstance();
    
    void RegisterLoader(std::shared_ptr<IStreamLoader> loader);
    std::shared_ptr<IStreamLoader> GetLoader(uint32_t typeId) const;
    
    // Process stream chunks
    void ProcessStream(const char* streamName, RWS::RwStream* stream, uint32_t streamSize);
    
private:
    CResourceHandler();
    std::map<uint32_t, std::shared_ptr<IStreamLoader>> m_loaders;
};

// --- Standard Climax / RenderWare Stream Loaders ---

class CWorldStreamLoader : public IStreamLoader {
public:
    uint32_t GetTypeID() const override { return 0x000B; }
    const char* GetTypeName() const override { return "CWorldStreamLoader"; }
    bool Read(const char* name, RWS::RwStream* stream, uint32_t length) override;
};

class CClumpStreamLoader : public IStreamLoader {
public:
    uint32_t GetTypeID() const override { return 0x0010; }
    const char* GetTypeName() const override { return "CClumpStreamLoader"; }
    bool Read(const char* name, RWS::RwStream* stream, uint32_t length) override;
};

class CTexDictionaryStreamLoader : public IStreamLoader {
public:
    uint32_t GetTypeID() const override { return 0x0016; }
    const char* GetTypeName() const override { return "CTexDictionaryStreamLoader"; }
    bool Read(const char* name, RWS::RwStream* stream, uint32_t length) override;
};

class CAudioCuesStreamLoader : public IStreamLoader {
public:
    uint32_t GetTypeID() const override { return 0x0F00; }
    const char* GetTypeName() const override { return "CAudioCuesStreamLoader"; }
    bool Read(const char* name, RWS::RwStream* stream, uint32_t length) override;
};

// Climax specifically has 0x0716 (SHO Model Container) and 0x071C (SHO World Container)
// Ghost Rider symbols don't have them, but they exist in SHO. We map them explicitly.
class CSHOSceneStreamLoader : public IStreamLoader {
public:
    uint32_t GetTypeID() const override { return 0x071C; } // Or 0x0716
    const char* GetTypeName() const override { return "CSHOSceneStreamLoader"; }
    bool Read(const char* name, RWS::RwStream* stream, uint32_t length) override;
};

} // namespace ResourceLoader
} // namespace ClimaxEngine
