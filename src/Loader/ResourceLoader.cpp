#include "GeometryDecoder.h"
#include "ClimaxEngine/SG/SceneObject.h"
#include "ClimaxEngine/Loader/ResourceLoader.h"
#include "ClimaxEngine/Platform/PS2/PS2Texture.h"
#include "ClimaxEngine/Core/Common.h"


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

bool RwChunk::Read(RWS::RwStream* stream) {
    uint32_t header[3];
    if (stream->Read(header, 12) != 12) {
        return false;
    }
    type = header[0];
    size = header[1];
    version = header[2];
    
    // Payload pointer cannot be used directly with RwStream if it's not memory stream, 
    // but we can assume memory stream for now and use GetCurrentPointer.
    if (auto* memStream = dynamic_cast<RWS::RwMemoryStream*>(stream)) {
        payload = memStream->GetCurrentPointer();
    }
    return true;
}

CResourceHandler::CResourceHandler() {
    // Automatically register standard loaders
    RegisterLoader(std::make_shared<CWorldStreamLoader>());
    RegisterLoader(std::make_shared<CClumpStreamLoader>());
    RegisterLoader(std::make_shared<CTexDictionaryStreamLoader>());
    RegisterLoader(std::make_shared<CAudioCuesStreamLoader>());
    RegisterLoader(std::make_shared<CSHOSceneStreamLoader>());
}

CResourceHandler& CResourceHandler::GetInstance() {
    static CResourceHandler instance;
    return instance;
}

void CResourceHandler::RegisterLoader(std::shared_ptr<IStreamLoader> loader) {
    m_loaders[loader->GetTypeID()] = loader;
}

std::shared_ptr<IStreamLoader> CResourceHandler::GetLoader(uint32_t typeId) const {
    auto it = m_loaders.find(typeId);
    if (it != m_loaders.end()) {
        return it->second;
    }
    return nullptr;
}

void CResourceHandler::ProcessStream(const char* streamName, RWS::RwStream* stream, uint32_t streamSize) {
    size_t startPos = stream->Tell();
    while (stream->Tell() + 12 <= startPos + streamSize && !stream->IsEOF()) {
        RwChunk chunk;
        if (!chunk.Read(stream)) {
            break;
        }
        
        auto loader = GetLoader(chunk.type);
        if (loader) {
            // Loaders read the chunk payload themselves, so they should leave the stream at the end of the chunk
            loader->Read(streamName, stream, chunk.size);
        } else {
            // Unhandled chunk, skip it
            // std::cout << "[ResourceLoader] Unhandled chunk type: 0x" << std::hex << chunk.type << std::dec << "\n";
            stream->Skip(chunk.size);
        }
        
        // Ensure stream is properly aligned if a loader failed to read the entire chunk
    }
}


// --- Specific Loaders Stub ---

bool CWorldStreamLoader::Read(const char* name, RWS::RwStream* stream, uint32_t length) {
    std::cout << "[ResourceLoader] Found CWorldStreamLoader chunk (size: " << length << ")\n";
    std::vector<uint8_t> data(length);
    if (stream->Read(data.data(), length) == length) {
        auto obj = std::make_shared<::ClimaxEngine::SG::CWorldObject>(name);
        DecodeRenderWareGeometry(name, data.data(), length, true, obj.get());
        ::ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().RegisterObject(obj);
    }
    return true;
}

bool CClumpStreamLoader::Read(const char* name, RWS::RwStream* stream, uint32_t length) {
    std::cout << "[ResourceLoader] Found CClumpStreamLoader chunk (size: " << length << ")\n";
    std::vector<uint8_t> data(length);
    if (stream->Read(data.data(), length) == length) {
        auto obj = std::make_shared<::ClimaxEngine::SG::CClumpObject>(name);
        DecodeRenderWareGeometry(name, data.data(), length, false, obj.get());
        ::ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().RegisterObject(obj);
    }
    return true;
}

bool CSHOSceneStreamLoader::Read(const char* name, RWS::RwStream* stream, uint32_t length) {
    if (length < 20) { stream->Skip(length); return false; }
    
    uint32_t header[2];
    if (stream->Read(header, 8) != 8) return false;
    
    auto be = [](uint32_t v) { return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000); };
    uint32_t tagLen = be(header[1]);
    
    if (tagLen > 1024) { stream->Skip(length - 8); return false; }
    
    stream->Skip(tagLen);
    
    uint8_t guidBuf[16];
    if (stream->Read(guidBuf, 16) != 16) return false;
    
    uint32_t nameLenBe;
    if (stream->Read(&nameLenBe, 4) != 4) return false;
    uint32_t nameLen = be(nameLenBe);
    
    if (nameLen > 1024) { stream->Skip(length - 8 - tagLen - 20); return false; }
    
    std::string secName;
    if (nameLen > 0) {
        std::vector<char> nameBuf(nameLen);
        stream->Read(nameBuf.data(), nameLen);
        secName.assign(nameBuf.data(), strnlen(nameBuf.data(), nameLen));
    }
    
    uint32_t remaining = length - (8 + tagLen + 20 + nameLen);
    CResourceHandler::GetInstance().ProcessStream(secName.c_str(), stream, remaining);
    
    return true;
}

bool CTexDictionaryStreamLoader::Read(const char* name, RWS::RwStream* stream, uint32_t length) {
    std::cout << "[ResourceLoader] Found CTexDictionaryStreamLoader chunk (size: " << length << ")\n";
    
    std::vector<uint8_t> data(length + 12);
    // Rewind back 12 bytes because PS2TextureDecoder expects the chunk header
    stream->Seek(stream->Tell() - 12);
    stream->Read(data.data(), length + 12);
    
    // Attempt to load dictionary for all remaining missing materials, or just blindly
    std::vector<std::string> missing;
    for (const auto &mat : ::g_MaterialNames) {
        if (::g_TextureMap.find(mat) == ::g_TextureMap.end()) {
            missing.push_back(mat);
        }
    }
    
    if (!missing.empty() || ::g_MaterialNames.empty()) {
        ::ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(data, missing.empty() ? ::g_MaterialNames : missing, true);
    }
    
    return true;
}

bool CAudioCuesStreamLoader::Read(const char* name, RWS::RwStream* stream, uint32_t length) {
    std::cout << "[ResourceLoader] Found CAudioCuesStreamLoader chunk (size: " << length << ")\n";
    stream->Skip(length);
    return true;
}


} // namespace ResourceLoader
} // namespace ClimaxEngine
