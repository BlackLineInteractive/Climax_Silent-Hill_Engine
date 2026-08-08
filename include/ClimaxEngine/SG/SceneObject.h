#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include "ClimaxEngine/Core/Common.h"

namespace ClimaxEngine {
namespace SG {

struct RenderContext {
    glm::mat4 viewProj;
    glm::vec3 eye;
    int pass; // 0 for opaque, 1 for blended
    // OpenGL Uniform locations
    int uM, uModel, uUntex, uAdd, uUnlit, uMatCol, uIce, uUseSkinning;
    std::map<std::string, GLuint>* textureMap;
};

class CSceneObject {
public:
    CSceneObject(const std::string& name) : m_name(name), m_transform(1.0f) {}
    virtual ~CSceneObject() = default;

    virtual void SetMatrixAndDraw(const RenderContext& ctx, MeshChunk* mesh) = 0;
    
    const std::string& GetName() const { return m_name; }
    void SetTransform(const glm::mat4& t) { m_transform = t; }
    const glm::mat4& GetTransform() const { return m_transform; }
    
    // Extract mesh chunks for software rasterizer or legacy UI
    virtual std::vector<MeshChunk*> GetMeshes() { return {}; }

protected:
    std::string m_name;
    glm::mat4 m_transform;
};

class CMeshObject : public CSceneObject {
public:
    CMeshObject(const std::string& name) : CSceneObject(name) {}
    
    void AddMesh(MeshChunk&& mesh) {
        m_meshes.push_back(std::move(mesh));
    }

    // The chunk just added. The decoder uploads through this rather than
    // through its own local, because AddMesh moves the chunk and m_meshes is a
    // vector -- the address the decoder held does not survive either the move
    // or the next reallocation.
    MeshChunk& LastMesh() { return m_meshes.back(); }


    std::vector<MeshChunk*> GetMeshes() override;

protected:
    std::vector<MeshChunk> m_meshes;
};

class CWorldObject : public CMeshObject {
public:
    CWorldObject(const std::string& name) : CMeshObject(name) {}
    void SetMatrixAndDraw(const RenderContext& ctx, MeshChunk* mesh) override;
};

class CClumpObject : public CMeshObject {
public:
    CClumpObject(const std::string& name) : CMeshObject(name) {}
    
    // Custom Climax Clump data
    Skeleton skeleton;
    AnimClip animClip;
    float animTime = 0.0f;
    std::vector<glm::mat4> currentBoneMats;
    // The rest pose alongside the animated one. An attachment -- a head that
    // is its own clump -- needs both to work out the delta its parent bone
    // moved through.
    std::vector<glm::mat4> restBoneMats;
    
    void SetMatrixAndDraw(const RenderContext& ctx, MeshChunk* mesh) override;
};

class CSceneObjectRegistrar {
public:
    static CSceneObjectRegistrar& GetInstance();
    
    void RegisterObject(std::shared_ptr<CSceneObject> obj);
    void Clear();
    
    const std::vector<std::shared_ptr<CSceneObject>>& GetObjects() const { return m_objects; }
    
private:
    CSceneObjectRegistrar() = default;
    std::vector<std::shared_ptr<CSceneObject>> m_objects;
};

} // namespace SG
} // namespace ClimaxEngine
