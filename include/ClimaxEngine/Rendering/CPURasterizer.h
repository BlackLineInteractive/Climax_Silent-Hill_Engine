#pragma once

#include "ClimaxEngine/Core/Common.h"
#include "ClimaxEngine/SG/SceneObject.h"
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>

class CPURasterizer {
public:
    CPURasterizer() = default;
    ~CPURasterizer();

    void Init(int width, int height);
    void Clear(float r, float g, float b, float a = 1.0f);
    void Resize(int width, int height);

    // Main render pass for scene geometry
    void RenderScene(const std::vector<std::shared_ptr<ClimaxEngine::SG::CSceneObject>>& objects,
                     const glm::mat4& mvp,
                     const glm::vec3& eyePos);

    // Display the CPU color buffer on screen using a screen quad
    void PresentOnScreen();

    GLuint GetTextureID() const { return m_ScreenTexture; }
    int GetWidth() const { return m_Width; }
    int GetHeight() const { return m_Height; }

private:
    int m_Width = 0;
    int m_Height = 0;

    std::vector<uint32_t> m_ColorBuffer; // RGBA8
    std::vector<float>    m_DepthBuffer; // Z-buffer

    GLuint m_ScreenTexture = 0;
    GLuint m_ScreenVAO = 0;
    GLuint m_ScreenVBO = 0;
    GLuint m_ScreenProgram = 0;

    void InitScreenQuad();
    void RasterizeTriangle(const Vertex& v0, const Vertex& v1, const Vertex& v2,
                           const glm::mat4& mvp, const glm::mat4& model,
                           const RawTexture* tex, const glm::vec3& eyePos);
};

extern CPURasterizer g_CPURasterizer;
