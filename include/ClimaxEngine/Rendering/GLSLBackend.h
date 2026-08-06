#pragma once

#include "ClimaxEngine/Rendering/RenderBackend.h"
#include <GL/glew.h>

class GLSLBackend : public IRenderBackend {
public:
    GLSLBackend();
    ~GLSLBackend() override;

    bool Init(void* windowHandle) override;
    void Shutdown() override;

    void BeginFrame(int width, int height, float clearR = 0.05f, float clearG = 0.05f, float clearB = 0.08f) override;
    void EndFrame() override;

    void SetUniforms(const RenderUniforms& uniforms) override;
    void BindTexture(uint32_t textureID) override;
    void DrawMesh(uint32_t vao, uint32_t vertexCount) override;
    void DrawLines(uint32_t vao, uint32_t vertexCount) override;

    BackendType GetBackendType() const override { return BackendType::GLSL; }
    const char* GetBackendName() const override { return "OpenGL 3.3 (GLSL)"; }

private:
    GLuint m_Program = 0;

    // Uniform locations
    GLint m_LocMVP = -1;
    GLint m_LocModel = -1;
    GLint m_LocFlipU = -1;
    GLint m_LocFlipV = -1;
    GLint m_LocUVOffset = -1;
    GLint m_LocUVScale = -1;
    GLint m_LocUseVertexColors = -1;
    GLint m_LocBrightness = -1;
    GLint m_LocRenderMode = -1;
    GLint m_LocEyePos = -1;
    GLint m_LocDepthMax = -1;
    GLint m_LocTexture = -1;
    GLint m_LocUseSkinning = -1;
    GLint m_LocBoneMat = -1;

    void CompileShaders();
};
