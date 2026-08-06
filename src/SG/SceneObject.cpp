#include "ClimaxEngine/SG/SceneObject.h"
#include <GL/glew.h>
#include <iostream>
#include <glm/gtc/type_ptr.hpp>

namespace ClimaxEngine {
namespace SG {

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

void CWorldObject::SetMatrixAndDraw(const RenderContext& ctx, MeshChunk* chunk) {
    // World space is drawn as is, no custom transformation needed unless we want to shift the whole world
    glUniformMatrix4fv(ctx.uM, 1, GL_FALSE, glm::value_ptr(ctx.viewProj));
    glUniformMatrix4fv(ctx.uModel, 1, GL_FALSE, glm::value_ptr(glm::mat4(1.0f)));
    glUniform1i(ctx.uUseSkinning, 0);

    glBindVertexArray(chunk->vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)chunk->vertices.size());
}

void CClumpObject::SetMatrixAndDraw(const RenderContext& ctx, MeshChunk* chunk) {
    // Clumps use the instance transform
    glm::mat4 m = ctx.viewProj * m_transform;
    glUniformMatrix4fv(ctx.uM, 1, GL_FALSE, glm::value_ptr(m));
    glUniformMatrix4fv(ctx.uModel, 1, GL_FALSE, glm::value_ptr(m_transform));
    
    bool hasSkin = false;
    if (!skeleton.bones.empty() && animClip.duration > 0.0f) {
        float t = fmod(animTime, animClip.duration);
        const size_t numBones = std::min(skeleton.bones.size(), (size_t)128);
        std::vector<glm::mat4> boneMats(numBones, glm::mat4(1.0f));
        
        for (size_t b = 0; b < numBones; ++b) {
            glm::mat4 local = skeleton.bones[b].restLocal;
            if (b < animClip.tracks.size() && !animClip.tracks[b].times.empty()) {
                const auto& times = animClip.tracks[b].times;
                const auto& poss  = animClip.tracks[b].pos;
                const auto& rots  = animClip.tracks[b].rot;
                
                int idx0 = 0, idx1 = 0;
                float factor = 0.0f;
                
                if (t <= times.front()) {
                    idx0 = idx1 = 0;
                } else if (t >= times.back()) {
                    idx0 = idx1 = (int)times.size() - 1;
                } else {
                    auto it = std::lower_bound(times.begin(), times.end(), t);
                    idx1 = (int)std::distance(times.begin(), it);
                    idx0 = idx1 - 1;
                    float span = times[idx1] - times[idx0];
                    factor = (span > 1e-6f) ? (t - times[idx0]) / span : 0.0f;
                }
                
                glm::vec3 pos = glm::mix(poss[idx0], poss[idx1], factor);
                glm::quat rot = glm::slerp(rots[idx0], rots[idx1], factor);
                local = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot);
            }
            
            int parent = skeleton.bones[b].parent;
            if (parent >= 0 && parent < (int)b) {
                boneMats[b] = boneMats[parent] * local;
            } else {
                boneMats[b] = local;
            }
        }
        
        // Compute skinning matrices and upload to GPU
        std::vector<glm::mat4> shaderTransforms(numBones);
        for (size_t b = 0; b < numBones; ++b)
            shaderTransforms[b] = boneMats[b] * skeleton.bones[b].invBind;
        
        if (!shaderTransforms.empty()) {
            GLint prog = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
            if (prog) {
                glUniformMatrix4fv(glGetUniformLocation(prog, "boneTransforms"),
                    (GLsizei)shaderTransforms.size(), GL_FALSE,
                    glm::value_ptr(shaderTransforms[0]));
                hasSkin = true;
            }
        }
    }
    
    glUniform1i(ctx.uUseSkinning, hasSkin ? 1 : 0);
    glBindVertexArray(chunk->vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)chunk->vertices.size());
}

} // namespace SG
} // namespace ClimaxEngine
