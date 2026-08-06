#include "ClimaxEngine/Rendering/CPURasterizer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <glm/gtc/matrix_transform.hpp>

CPURasterizer g_CPURasterizer;

static const char* s_ScreenVS = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTex;
out vec2 TexCoord;
void main() {
    TexCoord = aTex;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

static const char* s_ScreenFS = R"(
#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D uScreenTex;
void main() {
    FragColor = texture(uScreenTex, TexCoord);
}
)";

CPURasterizer::~CPURasterizer() {
    if (m_ScreenTexture) glDeleteTextures(1, &m_ScreenTexture);
    if (m_ScreenVAO) glDeleteVertexArrays(1, &m_ScreenVAO);
    if (m_ScreenVBO) glDeleteBuffers(1, &m_ScreenVBO);
    if (m_ScreenProgram) glDeleteProgram(m_ScreenProgram);
}

void CPURasterizer::Init(int width, int height) {
    if (width <= 0 || height <= 0) return;
    Resize(width, height);
    if (!m_ScreenProgram) InitScreenQuad();
}

void CPURasterizer::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (m_Width == width && m_Height == height) return;

    m_Width = width;
    m_Height = height;
    m_ColorBuffer.assign((size_t)width * height, 0xFF000000);
    m_DepthBuffer.assign((size_t)width * height, 1.0f);

    if (!m_ScreenTexture) {
        glGenTextures(1, &m_ScreenTexture);
    }
    glBindTexture(GL_TEXTURE_2D, m_ScreenTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void CPURasterizer::InitScreenQuad() {
    float quadVerts[] = {
        // Pos      // UV
        -1.0f,  1.0f, 0.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,

        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f
    };

    glGenVertexArrays(1, &m_ScreenVAO);
    glGenBuffers(1, &m_ScreenVBO);
    glBindVertexArray(m_ScreenVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_ScreenVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    auto compileShader = [](GLenum type, const char* src) -> GLuint {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        return sh;
    };
    GLuint vs = compileShader(GL_VERTEX_SHADER, s_ScreenVS);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, s_ScreenFS);
    m_ScreenProgram = glCreateProgram();
    glAttachShader(m_ScreenProgram, vs);
    glAttachShader(m_ScreenProgram, fs);
    glLinkProgram(m_ScreenProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);
}

void CPURasterizer::Clear(float r, float g, float b, float a) {
    uint8_t ur = (uint8_t)(std::clamp(r, 0.0f, 1.0f) * 255.0f);
    uint8_t ug = (uint8_t)(std::clamp(g, 0.0f, 1.0f) * 255.0f);
    uint8_t ub = (uint8_t)(std::clamp(b, 0.0f, 1.0f) * 255.0f);
    uint8_t ua = (uint8_t)(std::clamp(a, 0.0f, 1.0f) * 255.0f);
    uint32_t clearCol = (ua << 24) | (ub << 16) | (ug << 8) | ur;

    std::fill(m_ColorBuffer.begin(), m_ColorBuffer.end(), clearCol);
    std::fill(m_DepthBuffer.begin(), m_DepthBuffer.end(), 1.0f);
}

void CPURasterizer::RasterizeTriangle(const Vertex& v0, const Vertex& v1, const Vertex& v2,
                                       const glm::mat4& mvp, const glm::mat4& model,
                                       const RawTexture* tex, const glm::vec3& eyePos) {
    // Transform positions to Clip Space
    glm::vec4 clip0 = mvp * glm::vec4(v0.pos, 1.0f);
    glm::vec4 clip1 = mvp * glm::vec4(v1.pos, 1.0f);
    glm::vec4 clip2 = mvp * glm::vec4(v2.pos, 1.0f);

    // Simple Near-plane clipping check
    if (clip0.w <= 0.001f && clip1.w <= 0.001f && clip2.w <= 0.001f) return;

    // Perspective divide to NDC
    glm::vec3 ndc0 = glm::vec3(clip0) / clip0.w;
    glm::vec3 ndc1 = glm::vec3(clip1) / clip1.w;
    glm::vec3 ndc2 = glm::vec3(clip2) / clip2.w;

    // Map to Screen Coordinates
    float fw = (float)m_Width;
    float fh = (float)m_Height;

    auto toScreen = [fw, fh](const glm::vec3& ndc) -> glm::vec2 {
        return glm::vec2((ndc.x + 1.0f) * 0.5f * fw, (1.0f - ndc.y) * 0.5f * fh);
    };

    glm::vec2 p0 = toScreen(ndc0);
    glm::vec2 p1 = toScreen(ndc1);
    glm::vec2 p2 = toScreen(ndc2);

    // Compute bounding box
    int minX = (int)std::floor(std::min({p0.x, p1.x, p2.x}));
    int maxX = (int)std::ceil(std::max({p0.x, p1.x, p2.x}));
    int minY = (int)std::floor(std::min({p0.y, p1.y, p2.y}));
    int maxY = (int)std::ceil(std::max({p0.y, p1.y, p2.y}));

    minX = std::clamp(minX, 0, m_Width - 1);
    maxX = std::clamp(maxX, 0, m_Width - 1);
    minY = std::clamp(minY, 0, m_Height - 1);
    maxY = std::clamp(maxY, 0, m_Height - 1);

    if (minX >= maxX || minY >= maxY) return;

    // Edge function / Barycentric setup
    float area = (p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x);
    if (std::abs(area) < 1e-5f) return;
    float invArea = 1.0f / area;

    // World positions for lighting/depth
    glm::vec3 wp0 = glm::vec3(model * glm::vec4(v0.pos, 1.0f));
    glm::vec3 wp1 = glm::vec3(model * glm::vec4(v1.pos, 1.0f));
    glm::vec3 wp2 = glm::vec3(model * glm::vec4(v2.pos, 1.0f));

    // Normal calculation
    glm::vec3 e1 = wp1 - wp0;
    glm::vec3 e2 = wp2 - wp0;
    glm::vec3 N  = glm::length(glm::cross(e1, e2)) > 1e-5f ? glm::normalize(glm::cross(e1, e2)) : glm::vec3(0, 1, 0);

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            glm::vec2 p((float)x + 0.5f, (float)y + 0.5f);

            float w0 = ((p1.x - p.x) * (p2.y - p.y) - (p1.y - p.y) * (p2.x - p.x)) * invArea;
            float w1 = ((p2.x - p.x) * (p0.y - p.y) - (p2.y - p.y) * (p0.x - p.x)) * invArea;
            float w2 = 1.0f - w0 - w1;

            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) continue;

            // Interpolate depth (Z)
            float depth = w0 * ndc0.z + w1 * ndc1.z + w2 * ndc2.z;
            size_t idx = (size_t)y * m_Width + x;
            if (depth >= m_DepthBuffer[idx] || depth < -1.0f || depth > 1.0f) continue;

            // Interpolate UV
            glm::vec2 uv = w0 * v0.uv + w1 * v1.uv + w2 * v2.uv;
            if (state.flipU) uv.x = 1.0f - uv.x;
            if (state.flipV) uv.y = 1.0f - uv.y;
            uv = (uv * glm::vec2(state.uvScaleX, state.uvScaleY)) + glm::vec2(state.uvOffsetX, state.uvOffsetY);

            // Interpolate vertex color
            glm::vec4 col = w0 * v0.color + w1 * v1.color + w2 * v2.color;
            glm::vec3 fragPos = w0 * wp0 + w1 * wp1 + w2 * wp2;

            // Software shading according to renderMode
            glm::vec3 finalColor(1.0f);
            float alpha = 1.0f;

            if (state.renderMode == RenderMode::VertexColor) {
                finalColor = glm::vec3(col) * state.brightness;
                alpha = col.a;
            } else if (state.renderMode == RenderMode::FlatShaded) {
                glm::vec3 L = glm::normalize(glm::vec3(0.55f, 1.0f, 0.45f));
                float d = std::max(glm::dot(N, L), 0.0f) * 0.72f + 0.28f;
                finalColor = glm::vec3(0.70f, 0.72f, 0.76f) * d * state.brightness;
            } else if (state.renderMode == RenderMode::Normals) {
                finalColor = N * 0.5f + glm::vec3(0.5f);
            } else if (state.renderMode == RenderMode::Depth) {
                float dist = glm::distance(fragPos, eyePos);
                float v = std::clamp(1.0f - dist / (state.camDist * 4.5f), 0.0f, 1.0f);
                v = v * v;
                finalColor = glm::vec3(v);
            } else if (state.renderMode == RenderMode::Checker) {
                glm::vec2 ch = glm::floor(uv * 8.0f);
                float c = (std::fmod(ch.x + ch.y, 2.0f) < 1.0f) ? 0.82f : 0.18f;
                finalColor = glm::vec3(c);
            } else { // Textured or Unlit
                glm::vec4 texColor(1.0f);
                if (tex && !tex->pixels.empty() && tex->width > 0 && tex->height > 0) {
                    // Texture sampling (Repeat wrapping)
                    float u = uv.x - std::floor(uv.x);
                    float v = uv.y - std::floor(uv.y);
                    int tx = std::clamp((int)(u * tex->width), 0, tex->width - 1);
                    int ty = std::clamp((int)(v * tex->height), 0, tex->height - 1);

                    // Decode palette or pixel data
                    size_t pixelIdx = ((size_t)ty * tex->width + tx);
                    if (tex->depth == 8 && !tex->palette.empty() && pixelIdx < tex->pixels.size()) {
                        uint8_t palIdx = tex->pixels[pixelIdx];
                        if ((size_t)palIdx * 4 + 3 < tex->palette.size()) {
                            texColor.r = tex->palette[palIdx * 4 + 0] / 255.0f;
                            texColor.g = tex->palette[palIdx * 4 + 1] / 255.0f;
                            texColor.b = tex->palette[palIdx * 4 + 2] / 255.0f;
                            texColor.a = tex->palette[palIdx * 4 + 3] / 255.0f;
                        }
                    } else if (pixelIdx * 4 + 3 < tex->pixels.size()) {
                        texColor.r = tex->pixels[pixelIdx * 4 + 0] / 255.0f;
                        texColor.g = tex->pixels[pixelIdx * 4 + 1] / 255.0f;
                        texColor.b = tex->pixels[pixelIdx * 4 + 2] / 255.0f;
                        texColor.a = tex->pixels[pixelIdx * 4 + 3] / 255.0f;
                    }
                }

                if (texColor.a < 0.1f) continue; // Alpha mask test

                finalColor = glm::vec3(texColor);
                if (state.renderMode == RenderMode::Textured) {
                    if (state.useVertexColors) finalColor *= glm::vec3(col);
                    glm::vec3 L = glm::normalize(glm::vec3(0.55f, 1.0f, 0.45f));
                    float d = std::max(glm::dot(N, L), 0.0f) * 0.45f + 0.55f;
                    finalColor *= d * state.brightness;
                } else if (state.renderMode == RenderMode::Unlit) {
                    finalColor *= state.brightness;
                }
                alpha = texColor.a;
            }

            m_DepthBuffer[idx] = depth;

            uint8_t ur = (uint8_t)(std::clamp(finalColor.r, 0.0f, 1.0f) * 255.0f);
            uint8_t ug = (uint8_t)(std::clamp(finalColor.g, 0.0f, 1.0f) * 255.0f);
            uint8_t ub = (uint8_t)(std::clamp(finalColor.b, 0.0f, 1.0f) * 255.0f);
            uint8_t ua = (uint8_t)(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);

            m_ColorBuffer[idx] = (ua << 24) | (ub << 16) | (ug << 8) | ur;
        }
    }
}

void CPURasterizer::RenderScene(const std::vector<std::shared_ptr<ClimaxEngine::SG::CSceneObject>>& objects,
                                const glm::mat4& mvp,
                                const glm::vec3& eyePos) {
    const glm::mat4 identity(1.0f);

    for (const auto& obj : objects) {
        for (const auto* chunkPtr : obj->GetMeshes()) {
            const auto& chunk = *chunkPtr;
            const RawTexture* tex = nullptr;
            for (const auto& t : g_RawTextures) {
                if (t.name == chunk.texName) { tex = &t; break; }
            }

            glm::mat4 modelMat = obj->GetTransform();
            glm::mat4 m = mvp * modelMat;
            for (size_t i = 0; i + 2 < chunk.vertices.size(); i += 3) {
                RasterizeTriangle(chunk.vertices[i], chunk.vertices[i + 1], chunk.vertices[i + 2],
                                  m, modelMat, tex, eyePos);
            }
        }
    }
}

void CPURasterizer::PresentOnScreen() {
    if (!m_ScreenTexture || m_Width <= 0 || m_Height <= 0) return;

    glBindTexture(GL_TEXTURE_2D, m_ScreenTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_Width, m_Height, GL_RGBA, GL_UNSIGNED_BYTE, m_ColorBuffer.data());

    glDisable(GL_DEPTH_TEST);
    glUseProgram(m_ScreenProgram);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ScreenTexture);
    glUniform1i(glGetUniformLocation(m_ScreenProgram, "uScreenTex"), 0);

    glBindVertexArray(m_ScreenVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}
