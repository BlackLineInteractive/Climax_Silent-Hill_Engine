#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ClimaxEngine/Core/Common.h"

// ---------------------------------------------------------------------------
// GameCube / Wii native geometry
//
// Shattered Memories stores its meshes the same way Origins does -- a
// BinMeshPLG split table plus a NativeDataPLG blob per sector -- but the blob
// holds GX display lists and indexed attribute arrays instead of PS2 VIF
// packets, and it is big-endian.
//
// See docs/SHSM_ARC_FORMAT.md section 5b for the layout and the figures.
// ---------------------------------------------------------------------------

struct WiiMaterial {
    std::string texName;
    // The 0x0129 material extension names a second texture. It is not a shader
    // layer: in 1727 of 1747 world materials it repeats the base texture, and
    // in the other 20 it is the same surface in its frozen state
    // (EN_SC_BK_ceiling -> EN_SC_BK_ceiling_Frozen, ..._Wall_A -> ..._Wall_B).
    // That is the game's Otherworld swap, and it is the only extra texture the
    // asset binds.
    std::string altTexName;
    glm::vec4   color = glm::vec4(1.0f);
    bool        textured = false;
};

namespace WiiGeom {

// Standard RenderWare MaterialList at `off`.
void ReadMaterialList(const uint8_t* d, size_t size, size_t off,
                      std::vector<WiiMaterial>& out);

// One NativeDataPLG (0x0510) at `off`: decodes every display list it holds
// into a MeshChunk, one per BinMesh split.
bool ReadNative(const uint8_t* d, size_t size, size_t off,
                const std::vector<uint32_t>& meshMaterial,
                const std::vector<WiiMaterial>& materials,
                int matListWindowBase, int sectionIndex,
                std::vector<MeshChunk>& out);

// A whole rwID_WORLD payload: material list plus the recursive sector tree.
void ReadWorld(const uint8_t* d, size_t size, size_t off, size_t len,
               int sectionIndex, std::vector<MeshChunk>& out,
               std::vector<std::string>* materialNames = nullptr);

// A plain (non-native) RenderWare Geometry: the struct carries UVs, a triangle
// list and morph-target positions directly. Character heads are stored this way
// because they also carry delta-morph targets for facial animation.
bool ReadPlainGeometry(const uint8_t* d, size_t size, size_t structOff,
                       uint32_t structSize,
                       const std::vector<WiiMaterial>& materials,
                       int sectionIndex, std::vector<MeshChunk>& out);

// A rwID_CLUMP / rwID_RWS payload: frame hierarchy, geometry list and the
// atomics that pair them. Each atomic's world matrix is baked into its
// vertices, so the result needs no further placement.
void ReadClump(const uint8_t* d, size_t size, size_t off, size_t len,
               int sectionIndex, std::vector<MeshChunk>& out,
               std::vector<std::string>* materialNames = nullptr);

} // namespace WiiGeom
