#pragma once
#include <memory>
#include "ClimaxEngine/Core/ITextureDecoder.h"

namespace ClimaxEngine {
namespace Core {

enum class PlatformType {
    PS2,
    Wii
};

class IPlatformContext {
public:
    virtual ~IPlatformContext() = default;

    virtual ITextureDecoder* GetTextureDecoder() = 0;
    
    // Factory method to instantiate the correct platform context
    static std::unique_ptr<IPlatformContext> Create(PlatformType type);
};

} // namespace Core
} // namespace ClimaxEngine
