#pragma once

#include <cstdint>
#include <string>
#include "ClimaxEngine/Core/RWS/RwStream.h"

namespace ClimaxEngine {
namespace ResourceLoader {

class IStreamLoader {
public:
    virtual ~IStreamLoader() = default;

    virtual uint32_t GetTypeID() const = 0;
    virtual const char* GetTypeName() const = 0;
    
    // Read the object from the stream and register it
    // Returns true if parsing was successful
    virtual bool Read(const char* name, RWS::RwStream* stream, uint32_t length) = 0;
};

} // namespace ResourceLoader
} // namespace ClimaxEngine
