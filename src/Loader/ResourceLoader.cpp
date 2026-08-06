#include "ClimaxEngine/Loader/ResourceLoader.h"
#include <iostream>

namespace ClimaxEngine {
namespace ResourceLoader {

bool RwChunk::Read(const uint8_t* d, size_t dataSize, size_t offset) {
    if (offset + 12 > dataSize) return false;
    type = (uint32_t)d[offset] | ((uint32_t)d[offset + 1] << 8) | ((uint32_t)d[offset + 2] << 16) | ((uint32_t)d[offset + 3] << 24);
    size = (uint32_t)d[offset + 4] | ((uint32_t)d[offset + 5] << 8) | ((uint32_t)d[offset + 6] << 16) | ((uint32_t)d[offset + 7] << 24);
    version = (uint32_t)d[offset + 8] | ((uint32_t)d[offset + 9] << 8) | ((uint32_t)d[offset + 10] << 16) | ((uint32_t)d[offset + 11] << 24);
    
    if (offset + 12 + size > dataSize) return false;
    payload = d + offset + 12;
    return true;
}

CResourceHandler& CResourceHandler::GetInstance() {
    static CResourceHandler instance;
    return instance;
}

void CResourceHandler::AddLoader(uint32_t typeId, std::shared_ptr<CStreamLoader> loader) {
    m_loaders[typeId] = loader;
}

std::shared_ptr<CStreamLoader> CResourceHandler::GetLoader(uint32_t typeId) const {
    auto it = m_loaders.find(typeId);
    if (it != m_loaders.end()) {
        return it->second;
    }
    return nullptr;
}

void CResourceHandler::ProcessStream(const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset + 12 <= size) {
        RwChunk chunk;
        if (!chunk.Read(data, size, offset)) {
            break;
        }
        
        auto loader = GetLoader(chunk.type);
        if (loader) {
            loader->LoadStream(chunk, data, size);
        } else {
            // std::cout << "[loader] Unhandled chunk type: 0x" << std::hex << chunk.type << std::dec << "\n";
        }
        
        offset += 12 + chunk.size;
    }
}

// --- Specific Loaders Stub ---
// These will eventually populate our Scene Graph (CSceneObject) instead of returning raw structs.
// For now they are just stubs to demonstrate the 1:1 architecture mapping.

bool CWorldStreamLoader::LoadStream(const RwChunk& chunk, const uint8_t* streamStart, size_t streamSize) {
    std::cout << "[ResourceLoader] Found World chunk (size: " << chunk.size << ")\n";
    // TODO: Recursively parse World sub-chunks, extract materials, geometry, sectors
    return true;
}

bool CClumpStreamLoader::LoadStream(const RwChunk& chunk, const uint8_t* streamStart, size_t streamSize) {
    std::cout << "[ResourceLoader] Found Clump chunk (size: " << chunk.size << ")\n";
    // TODO: Recursively parse Clump, FrameList, GeometryList, Atomics
    return true;
}

bool CTexDictionaryStreamLoader::LoadStream(const RwChunk& chunk, const uint8_t* streamStart, size_t streamSize) {
    std::cout << "[ResourceLoader] Found TexDictionary chunk (size: " << chunk.size << ")\n";
    // TODO: Parse Texture Native chunks
    return true;
}

bool CAudioCuesStreamLoader::LoadStream(const RwChunk& chunk, const uint8_t* streamStart, size_t streamSize) {
    std::cout << "[ResourceLoader] Found AudioCues chunk (size: " << chunk.size << ")\n";
    return true;
}

} // namespace ResourceLoader
} // namespace ClimaxEngine
