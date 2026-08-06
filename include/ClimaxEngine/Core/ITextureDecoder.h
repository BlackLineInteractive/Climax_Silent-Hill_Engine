#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace ClimaxEngine {
namespace Core {

class ITextureDecoder {
public:
    virtual ~ITextureDecoder() = default;

    // Decode a dictionary of textures from a memory buffer.
    // allowedNames: if not empty and fallback is false, only these textures are decoded.
    // fallback: if true, all textures in the dictionary are decoded.
    virtual void LoadDictionary(const std::vector<uint8_t>& data,
                                const std::vector<std::string>& allowedNames,
                                bool fallback) = 0;
};

} // namespace Core
} // namespace ClimaxEngine
