#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include "ClimaxEngine/SG/SceneObject.h"

namespace ClimaxEngine {
namespace ResourceLoader {
    // Decodes a raw RenderWare chunk payload (World or Clump) into Meshes
    // and attaches them to the provided CMeshObject.
    void DecodeRenderWareGeometry(const std::string& name, const uint8_t* payload, size_t length, bool isWorld, SG::CMeshObject* destObj);
}
}
