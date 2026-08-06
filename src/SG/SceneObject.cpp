#include "ClimaxEngine/SG/SceneObject.h"
#include <GL/glew.h>
#include <iostream>

namespace ClimaxEngine {
namespace SG {

void CMeshObject::Render(const RenderContext& ctx) {
    // In a full implementation, we bind VAO and draw.
    // For Phase 2, we still rely on main.cpp's render loop but we provide the matrix.
    // We will expand this when we move the full OpenGL render loop inside here.
}

std::vector<MeshChunk*> CMeshObject::GetMeshes() {
    std::vector<MeshChunk*> ptrs;
    for (auto& m : m_meshes) {
        ptrs.push_back(&m);
    }
    return ptrs;
}

CSceneObjectRegistrar& CSceneObjectRegistrar::GetInstance() {
    static CSceneObjectRegistrar instance;
    return instance;
}

void CSceneObjectRegistrar::RegisterObject(std::shared_ptr<CSceneObject> obj) {
    if (obj) m_objects.push_back(obj);
}

void CSceneObjectRegistrar::Clear() {
    m_objects.clear();
}

} // namespace SG
} // namespace ClimaxEngine

void CWorldObject::Render(const RenderContext& ctx) {
    // World space is drawn as is, no custom transformation needed unless we want to shift the whole world
    for (auto& chunk : m_meshes) {
        if (chunk.alphaPass && ctx.pass == 0) continue;
        if (!chunk.alphaPass && ctx.pass == 1) continue;

        // Skip missing texture check here for brevity, we handle it in main
        glUniformMatrix4fv(ctx.uM, 1, GL_FALSE, glm::value_ptr(ctx.viewProj));
        glUniformMatrix4fv(ctx.uModel, 1, GL_FALSE, glm::value_ptr(glm::mat4(1.0f)));
        glUniform1i(ctx.uUseSkinning, 0);

        glBindVertexArray(chunk.vao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)chunk.vertices.size());
    }
}

void CClumpObject::Render(const RenderContext& ctx) {
    // Clumps use the instance transform
    glm::mat4 m = ctx.viewProj * m_transform;
    for (auto& chunk : m_meshes) {
        if (chunk.alphaPass && ctx.pass == 0) continue;
        if (!chunk.alphaPass && ctx.pass == 1) continue;

        glUniformMatrix4fv(ctx.uM, 1, GL_FALSE, glm::value_ptr(m));
        glUniformMatrix4fv(ctx.uModel, 1, GL_FALSE, glm::value_ptr(m_transform));
        
        bool hasSkin = false;
        if (!skeleton.bones.empty() && animClip.duration > 0.0f) {
            // Apply skinning based on animTime
            // (Skipping full skinning logic here for now, it's quite large. We will move it here in full shortly)
        }
        
        glUniform1i(ctx.uUseSkinning, hasSkin ? 1 : 0);
        glBindVertexArray(chunk.vao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)chunk.vertices.size());
    }
}
