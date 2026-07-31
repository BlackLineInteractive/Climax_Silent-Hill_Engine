#pragma once

#include <vector>
#include <string>
#include <memory>
#include <glm/glm.hpp>
#include "ClimaxEngine/Core/Common.h"

enum class BackendType {
    GLSL,   // OpenGL 3.3 Core (GLSL)
    Metal   // Apple Metal (MSL)
};

struct RenderUniforms {
    glm::mat4 mvp = glm::mat4(1.0f);
    glm::mat4 model = glm::mat4(1.0f);
    bool flipU = false;
    bool flipV = false;
    glm::vec2 uvOffset = glm::vec2(0.0f);
    glm::vec2 uvScale = glm::vec2(1.0f);
    bool useVertexColors = false;
    float brightness = 1.0f;
    int renderMode = 0; // 0=Textured 1=VertexColor 2=FlatShaded 3=Normals 4=Depth 5=Checker 6=Unlit
    glm::vec3 eyePos = glm::vec3(0.0f);
    float depthMax = 100.0f;
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual bool Init(void* windowHandle) = 0;
    virtual void Shutdown() = 0;

    virtual void BeginFrame(int width, int height, float clearR = 0.05f, float clearG = 0.05f, float clearB = 0.08f) = 0;
    virtual void EndFrame() = 0;

    virtual void SetUniforms(const RenderUniforms& uniforms) = 0;
    virtual void BindTexture(uint32_t textureID) = 0;
    virtual void DrawMesh(uint32_t vao, uint32_t vertexCount) = 0;
    virtual void DrawLines(uint32_t vao, uint32_t vertexCount) = 0;

    virtual BackendType GetBackendType() const = 0;
    virtual const char* GetBackendName() const = 0;
};

// Factory function
std::unique_ptr<IRenderBackend> CreateRenderBackend(BackendType type);
