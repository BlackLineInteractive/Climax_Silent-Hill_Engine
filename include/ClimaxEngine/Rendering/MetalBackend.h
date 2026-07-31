#pragma once

#include "ClimaxEngine/Rendering/RenderBackend.h"

#ifdef __APPLE__

class MetalBackend : public IRenderBackend {
public:
    MetalBackend();
    ~MetalBackend() override;

    bool Init(void* windowHandle) override;
    void Shutdown() override;

    void BeginFrame(int width, int height, float clearR = 0.05f, float clearG = 0.05f, float clearB = 0.08f) override;
    void EndFrame() override;

    void SetUniforms(const RenderUniforms& uniforms) override;
    void BindTexture(uint32_t textureID) override;
    void DrawMesh(uint32_t vao, uint32_t vertexCount) override;
    void DrawLines(uint32_t vao, uint32_t vertexCount) override;

    BackendType GetBackendType() const override { return BackendType::Metal; }
    const char* GetBackendName() const override { return "Apple Metal (MSL)"; }

private:
    struct Impl;
    Impl* m_Impl = nullptr;
};

#endif
