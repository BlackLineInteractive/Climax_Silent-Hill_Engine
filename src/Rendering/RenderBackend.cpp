#include "ClimaxEngine/Rendering/RenderBackend.h"
#include "ClimaxEngine/Rendering/GLSLBackend.h"

#ifdef __APPLE__
#include "ClimaxEngine/Rendering/MetalBackend.h"
#endif

std::unique_ptr<IRenderBackend> CreateRenderBackend(BackendType type) {
#ifdef __APPLE__
    if (type == BackendType::Metal) {
        return std::make_unique<MetalBackend>();
    }
#endif
    return std::make_unique<GLSLBackend>();
}
