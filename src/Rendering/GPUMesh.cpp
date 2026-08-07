// ─────────────────────────────────────────────────────────────────────────────
// GPUMesh.cpp  —  the GL side of the meshes whose data lives in Core/Types.h
//
// Everything in this file is what used to sit inside MeshChunk and
// CollisionMesh as GLuint members and Upload()/Free() methods. Moving it here
// is what lets Types.h -- and with it climax-core and climax-game -- compile
// without an OpenGL header in sight.
// ─────────────────────────────────────────────────────────────────────────────
#include "ClimaxEngine/Render/GPUMesh.h"

#include <cstddef>
#include <vector>

namespace {

// The mirrors. Slots are handed out by index and never move, so a handle stays
// valid for the lifetime of the level.
std::vector<GPUMeshChunk> g_meshMirrors;
std::vector<GPUCollisionMesh> g_collisionMirrors;

// Declares the vertex layout of a Vertex buffer. Identical in both upload
// paths, and it was duplicated verbatim in two files before this.
void DeclareVertexLayout() {
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, uv));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, color));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, boneWeights));
    glEnableVertexAttribArray(3);
    // boneIds are uint8_t indices. GL_UNSIGNED_BYTE with normalise = GL_FALSE
    // converts them to float without scaling, which is what the shader reads.
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, boneIds));
    glEnableVertexAttribArray(4);
}

} // namespace

// ── GPUMeshChunk ─────────────────────────────────────────────────────────────

void GPUMeshChunk::Upload(const MeshChunk &src) {
    if (src.vertices.empty())
        return;
    if (vao)
        Release();

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(src.vertices.size() * sizeof(Vertex)),
                 src.vertices.data(), GL_STATIC_DRAW);
    DeclareVertexLayout();
    glBindVertexArray(0);

    vertCount = (int)src.vertices.size();
}

void GPUMeshChunk::Draw() const {
    if (!vao || vertCount <= 0)
        return;
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)vertCount);
}

void GPUMeshChunk::Release() {
    if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
    vertCount = 0;
}

// ── GPUCollisionMesh ─────────────────────────────────────────────────────────

void GPUCollisionMesh::Upload(const CollisionMesh &src) {
    if (src.verts.empty())
        return;
    if (vao)
        Release();

    // Positions only -- the collision overlay is drawn as flat lines.
    std::vector<float> buf;
    buf.reserve(src.verts.size() * 3);
    for (const glm::vec3 &v : src.verts) {
        buf.push_back(v.x);
        buf.push_back(v.y);
        buf.push_back(v.z);
    }

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(buf.size() * sizeof(float)),
                 buf.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
    glEnableVertexAttribArray(0);

    if (!src.indices.empty()) {
        glGenBuffers(1, &ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)(src.indices.size() * sizeof(uint32_t)),
                     src.indices.data(), GL_STATIC_DRAW);
    }
    glBindVertexArray(0);

    indexCount = (int32_t)src.indices.size();
    uploaded = true;
}

void GPUCollisionMesh::Draw() const {
    if (!vao || indexCount <= 0)
        return;
    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
}

void GPUCollisionMesh::Release() {
    if (ebo) { glDeleteBuffers(1, &ebo); ebo = 0; }
    if (vbo) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (vao) { glDeleteVertexArrays(1, &vao); vao = 0; }
    indexCount = 0;
    uploaded = false;
}

// ── Registry ─────────────────────────────────────────────────────────────────

GPUMeshChunk &GpuFor(MeshChunk &m) {
    if (m.gpuMesh < 0 || m.gpuMesh >= (int)g_meshMirrors.size()) {
        m.gpuMesh = (int)g_meshMirrors.size();
        g_meshMirrors.emplace_back();
    }
    return g_meshMirrors[(size_t)m.gpuMesh];
}

GPUCollisionMesh &GpuFor(CollisionMesh &m) {
    if (m.gpuMesh < 0 || m.gpuMesh >= (int)g_collisionMirrors.size()) {
        m.gpuMesh = (int)g_collisionMirrors.size();
        g_collisionMirrors.emplace_back();
    }
    return g_collisionMirrors[(size_t)m.gpuMesh];
}

const GPUMeshChunk *GpuPeek(const MeshChunk &m) {
    if (m.gpuMesh < 0 || m.gpuMesh >= (int)g_meshMirrors.size())
        return nullptr;
    return &g_meshMirrors[(size_t)m.gpuMesh];
}

const GPUCollisionMesh *GpuPeek(const CollisionMesh &m) {
    if (m.gpuMesh < 0 || m.gpuMesh >= (int)g_collisionMirrors.size())
        return nullptr;
    return &g_collisionMirrors[(size_t)m.gpuMesh];
}

void ReleaseAllGpuMeshes() {
    for (GPUMeshChunk &g : g_meshMirrors)
        g.Release();
    for (GPUCollisionMesh &g : g_collisionMirrors)
        g.Release();
    g_meshMirrors.clear();
    g_collisionMirrors.clear();
}

void ResetCollision(CollisionMesh &m) {
    if (GpuPeek(m))
        GpuFor(m).Release();
    // Drop the handle as well. g_Collision is the one mesh that outlives a
    // level, so a stale index left here would name whatever slot the next level
    // happened to allocate in its place.
    m.gpuMesh = -1;
    m.verts.clear();
    m.indices.clear();
}
