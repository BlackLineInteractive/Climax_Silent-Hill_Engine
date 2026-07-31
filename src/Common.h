#pragma once

#include <vector>
#include <string>
#include <map>
#include <filesystem>
#include <GL/glew.h>
#include <glm/glm.hpp>

namespace fs = std::filesystem;

// strcasecmp is POSIX; MSVC spells it _stricmp.
#ifdef _MSC_VER
  #include <string.h>
  #define sho_stricmp _stricmp
#else
  #include <strings.h>
  #define sho_stricmp strcasecmp
#endif

// ------------------- СТРУКТУРИ -------------------
struct Vertex {
    glm::vec3 pos;
    glm::vec2 uv;
    glm::vec4 color;
};

struct MeshChunk {
    std::vector<Vertex> vertices;
    GLuint vao = 0;
    GLuint vbo = 0;
    int materialIndex = 0;  // kept for backward compat, use texName instead
    std::string texName;    // resolved texture name (directly from per-object MaterialList)
};

struct RawTexture {
    std::string name;
    int width = 0, height = 0, depth = 0;
    std::vector<uint8_t> pixels;
    std::vector<uint8_t> palette;
    GLuint glID = 0;
    bool clampU = false;
    bool clampV = false;
};

// Texture metadata for the TXD preview window
struct TexPreviewInfo {
    GLuint glID = 0;
    int    width = 0, height = 0;
    int    depth = 0;
};

// One entry in the container structure panel
struct ContainerChunkInfo {
    uint32_t    offset;
    uint32_t    typeId;
    std::string label;
};

// Entry from the SHO file header type directory
struct ShoTypeEntry {
    std::string name;   // e.g. "CZone", "CPlayerSpawner"
    uint32_t    count;  // number of instances
};

// A game section inside the container (per 0x716 block)
struct ShoSection {
    uint32_t    offset;
    uint32_t    size;
    std::string name;   // e.g. "rwID_WORLD", "rwID_CBSP", "rwID_CLUMP"
};

// Collision geometry (from CBSP / second RW_WORLD)
struct CollisionMesh {
    std::vector<glm::vec3> verts;
    std::vector<uint32_t>  indices; // triangle list
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    bool   uploaded = false;
    void Upload();
    void Free();
};

// Clump-based game object: position in world space
struct ClumpObject {
    std::string  sectionName; // e.g. "rwID_CLUMP"
    std::string  label;       // short display label (e.g. "CLUMP 0")
    glm::vec3    position;    // world-space position from FrameList
    glm::mat4    transform;   // full 3x4 → 4x4 transform
    int          meshStart;   // first g_Chunks index belonging to this clump (-1 = none)
    int          meshCount;   // how many g_Chunks indices
};

// Render mode for the 3D viewport
enum class RenderMode {
    Textured    = 0,  // texture + vertex colors (default)
    VertexColor = 1,  // only vertex colors, no texture
    FlatShaded  = 2,  // per-face normals + directional light, grey
    Normals     = 3,  // face normals visualised as RGB colour
    Depth       = 4,  // linear depth grey-scale
    Checker     = 5,  // UV checkerboard (no texture)
    Unlit       = 6,  // texture only, no lighting, no vertex color
};

struct ViewerState {
    // Orbit camera — camera rotates around camTarget at distance camDist
    float camTargetX = 0.0f, camTargetY = 2.0f, camTargetZ = 0.0f;
    float camYaw   =  0.0f;  // horizontal rotation, degrees
    float camPitch = 20.0f;  // vertical   rotation, degrees  (-89..89)
    float camDist  = 15.0f;  // zoom distance

    bool flipU = false;
    bool flipV = false;
    float uvOffsetX = 0.0f, uvOffsetY = 0.0f;
    float uvScaleX = 1.0f, uvScaleY = 1.0f;
    bool showWireframe    = false;
    bool linearFilter     = true;
    bool forceRepeat      = false;
    bool useVertexColors  = true;
    float brightness      = 1.3f;
    bool showCollision    = false;  // overlay collision wireframe
    bool showCollisionSolid = false;// fill collision as solid semi-transparent
    bool showClumps       = true;   // show clump object markers
    bool showStructure    = true;   // show Structure hierarchy panel
    bool showTextures     = true;   // show Textures browser panel
    RenderMode renderMode = RenderMode::Textured;

    // Pivot gizmo (ImGuizmo)
    bool  showPivotGizmo  = true;   // draw / allow dragging the pivot gizmo
    bool  pivotSnapOn     = false;  // snap translation to a grid (also forced by Ctrl)
    float pivotSnap       = 1.0f;   // grid step in world units

    // Sky / background
    float skyColorTop[3] = {0.07f, 0.07f, 0.09f};  // horizon-to-top colour
    float skyColorBot[3] = {0.11f, 0.11f, 0.14f};  // ground colour
    bool  skyGradient    = false;                   // draw gradient vs solid
};

// ------------------- ГЛОБАЛЬНІ ДАНІ (Оголошення) -------------------
extern ViewerState state;
extern std::vector<MeshChunk>        g_Chunks;
extern std::vector<std::string>      g_MaterialNames;
extern std::map<std::string, GLuint>          g_TextureMap;
extern std::map<std::string, TexPreviewInfo>  g_TexInfo;
extern std::vector<ContainerChunkInfo>        g_ContainerChunks;
extern std::map<std::string, std::vector<int>> g_MeshTexMap;

// SHO container meta
extern std::vector<ShoTypeEntry>     g_ShoTypes;     // from file header type table
extern std::vector<ShoSection>       g_ShoSections;  // all 0x716 blocks
extern CollisionMesh                 g_Collision;    // CBSP floor collision
extern std::vector<ClumpObject>      g_Clumps;       // animated/placed objects

extern std::string              g_CurrentMeshContainer;
extern std::vector<std::string> g_CurrentTxdPaths;
