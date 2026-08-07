#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClimaxEngine/Render/GPUMesh.h  —  GPU-side mesh handles
//
// Owns the GL objects that mirror the CPU data in Types.h.
// Must NOT be included by climax-core or climax-game.
// ─────────────────────────────────────────────────────────────────────────────

#include <GL/glew.h>
#include <cstdint>
#include "ClimaxEngine/Core/Types.h"

// GPU mirror of MeshChunk (CPU verts live in Types.h::MeshChunk).
struct GPUMeshChunk {
    GLuint vao = 0;
    GLuint vbo = 0;
    int    vertCount = 0;

    void Upload(const MeshChunk& src);
    void Draw() const;
    void Release();
};

// GPU mirror of CollisionMesh (CPU arrays live in Types.h::CollisionMesh).
struct GPUCollisionMesh {
    GLuint  vao        = 0;
    GLuint  vbo        = 0;
    GLuint  ebo        = 0;
    int32_t indexCount = 0;
    bool    uploaded   = false;

    void Upload(const CollisionMesh& src);
    void Draw()    const;
    void Release();
};

// ── The registry ─────────────────────────────────────────────────────────────
//
// A CPU mesh knows nothing about its GPU mirror -- that is the point of keeping
// Types.h free of GL -- so the mirrors live here, and the mesh carries only an
// opaque `gpuMesh` index into this table.
//
// An index rather than the object's address: a chunk is uploaded and then moved
// into the object that owns it, so its address does not survive, while an index
// copied along with the struct still names the right buffers.
GPUMeshChunk&     GpuFor(MeshChunk& m);
GPUCollisionMesh& GpuFor(CollisionMesh& m);

// The mirror of a mesh that has been uploaded, or nullptr when it has not.
// For asking "is this on the GPU yet" without creating a slot.
const GPUMeshChunk*     GpuPeek(const MeshChunk& m);
const GPUCollisionMesh* GpuPeek(const CollisionMesh& m);

// Releases every GL object the registry holds and empties it. Called when a
// level is unloaded.
void ReleaseAllGpuMeshes();

// Empties a collision mesh on both sides at once.
//
// The CPU arrays have to go with the buffers: this is what the loader calls to
// reset collision between levels, and leaving them filled made every load
// append to the previous one -- a 664-byte door container once reported 192
// collision vertices inherited from the level opened before it.
void ResetCollision(CollisionMesh& m);
