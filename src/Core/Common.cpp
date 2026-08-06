#include "ClimaxEngine/Core/Common.h"
#include <cstring>

// Визначення глобальних змінних
ViewerState state;
std::vector<MeshChunk>        g_Chunks;
std::vector<RawTexture>       g_RawTextures;
std::vector<std::string>      g_MaterialNames;
std::map<std::string, GLuint>           g_TextureMap;
std::map<std::string, TexPreviewInfo>   g_TexInfo;
std::map<std::string, bool>             g_TexGradient;
std::map<std::string, bool>             g_TexOpaque;
std::vector<ContainerChunkInfo>         g_ContainerChunks;
std::map<std::string, std::vector<MeshChunk*>> g_MeshTexMap;

std::vector<ShoTypeEntry>  g_ShoTypes;
std::vector<ShoSection>    g_ShoSections;
CollisionMesh              g_Collision;
std::vector<ClumpObject>   g_Clumps;
std::vector<GameObject>    g_GameObjects;
std::vector<LevelCamera>   g_Cameras;
std::vector<AudioClip>     g_Sounds;
std::vector<AudioSourceRef> g_AudioLibrary;

std::string              g_CurrentMeshContainer;
std::vector<std::string> g_CurrentTxdPaths;

void CollisionMesh::Upload() {
    if (verts.empty()) return;
    if (vao) Free();
    // build a flat vertex buffer: just positions (as Vertex structs with green color)
    std::vector<float> buf;
    buf.reserve(verts.size() * 3);
    for (auto& v : verts) { buf.push_back(v.x); buf.push_back(v.y); buf.push_back(v.z); }
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizei)(buf.size()*sizeof(float)), buf.data(), GL_STATIC_DRAW);
    // position only (loc 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void*)0);
    glEnableVertexAttribArray(0);
    if (!indices.empty()) {
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
            (GLsizei)(indices.size()*sizeof(uint32_t)), indices.data(), GL_STATIC_DRAW);
    }
    glBindVertexArray(0);
    uploaded = true;
}

void CollisionMesh::Free() {
    if (ebo) { glDeleteBuffers(1, &ebo); ebo = 0; }
    if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
    // The CPU-side arrays have to go too. Free() is what the loader calls to
    // reset collision between levels, and leaving these filled made every load
    // append to the previous one — a 664-byte door container reported 192
    // collision vertices inherited from the level opened before it.
    verts.clear();
    indices.clear();
    uploaded = false;
}
