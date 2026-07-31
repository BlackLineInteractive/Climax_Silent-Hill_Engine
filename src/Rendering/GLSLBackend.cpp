#include "ClimaxEngine/Rendering/GLSLBackend.h"
#include <iostream>

static const char* s_VS = R"(
#version 330 core
layout(location=0) in vec3 P;
layout(location=1) in vec2 T;
layout(location=2) in vec4 C;
out vec2  TC;
out vec4  VC;
out vec3  fragWorldPos;
uniform mat4  m;
uniform mat4  model;     // instance placement, identity for world geometry
uniform bool  flipU;
uniform bool  flipV;
uniform vec2  uvOffset;
uniform vec2  uvScale;
void main(){
    gl_Position  = m * vec4(P, 1.0);
    fragWorldPos = vec3(model * vec4(P, 1.0));
    vec2 coord = T;
    if(flipU) coord.x = 1.0 - coord.x;
    if(flipV) coord.y = 1.0 - coord.y;
    TC = (coord * uvScale) + uvOffset;
    VC = C;
}
)";

static const char* s_FS = R"(
#version 330 core
out vec4 FragColor;
in vec2  TC;
in vec4  VC;
in vec3  fragWorldPos;
uniform sampler2D t;
uniform bool  useVertexColors;
uniform float brightness;
uniform int   renderMode;
// 0=Textured 1=VertexColor 2=FlatShaded 3=Normals 4=Depth 5=Checker 6=Unlit
uniform vec3  eyePos;
uniform float depthMax;

void main(){
    vec3 dx = dFdx(fragWorldPos);
    vec3 dy = dFdy(fragWorldPos);
    vec3 N  = normalize(cross(dx, dy));

    if(renderMode == 1){
        if(VC.a < 0.05) discard;
        FragColor = vec4(VC.rgb * brightness, VC.a);
    } else if(renderMode == 2){
        vec3 L    = normalize(vec3(0.55, 1.0, 0.45));
        float d   = max(dot(N, L), 0.0) * 0.72 + 0.28;
        FragColor = vec4(vec3(0.70, 0.72, 0.76) * d * brightness, 1.0);
    } else if(renderMode == 3){
        FragColor = vec4(N * 0.5 + 0.5, 1.0);
    } else if(renderMode == 4){
        float dist = distance(fragWorldPos, eyePos);
        float v    = clamp(1.0 - dist / depthMax, 0.0, 1.0);
        v = v * v;
        FragColor  = vec4(vec3(v), 1.0);
    } else if(renderMode == 5){
        vec2 ch = floor(TC * 8.0);
        float c = mod(ch.x + ch.y, 2.0) < 1.0 ? 0.82 : 0.18;
        FragColor = vec4(vec3(c), 1.0);
    } else if(renderMode == 6){
        vec4 tex = texture(t, TC);
        if(tex.a < 0.1) discard;
        FragColor = vec4(tex.rgb * brightness, tex.a);
    } else {
        vec4 tex = texture(t, TC);
        if(tex.a < 0.1) discard;
        vec3 col = tex.rgb;
        if(useVertexColors) col *= VC.rgb;
        vec3 L = normalize(vec3(0.55, 1.0, 0.45));
        float d = max(dot(N, L), 0.0) * 0.45 + 0.55;
        FragColor = vec4(col * d * brightness, tex.a);
    }
}
)";

GLSLBackend::GLSLBackend() {}

GLSLBackend::~GLSLBackend() {
    Shutdown();
}

bool GLSLBackend::Init(void* /*windowHandle*/) {
    CompileShaders();
    return m_Program != 0;
}

void GLSLBackend::Shutdown() {
    if (m_Program) {
        glDeleteProgram(m_Program);
        m_Program = 0;
    }
}

void GLSLBackend::CompileShaders() {
    auto compile = [](GLenum stage, const char* src, const char* name) -> GLuint {
        GLuint sh = glCreateShader(stage);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = GL_FALSE;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            std::cerr << "[GLSLBackend] " << name << " compile error:\n" << log << std::endl;
        }
        return sh;
    };

    GLuint vs = compile(GL_VERTEX_SHADER, s_VS, "vertex");
    GLuint fs = compile(GL_FRAGMENT_SHADER, s_FS, "fragment");
    m_Program = glCreateProgram();
    glAttachShader(m_Program, vs);
    glAttachShader(m_Program, fs);
    glLinkProgram(m_Program);

    GLint ok = GL_FALSE;
    glGetProgramiv(m_Program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(m_Program, sizeof(log), nullptr, log);
        std::cerr << "[GLSLBackend] program link error:\n" << log << std::endl;
    }

    glDetachShader(m_Program, vs); glDeleteShader(vs);
    glDetachShader(m_Program, fs); glDeleteShader(fs);

    m_LocMVP             = glGetUniformLocation(m_Program, "m");
    m_LocModel           = glGetUniformLocation(m_Program, "model");
    m_LocFlipU           = glGetUniformLocation(m_Program, "flipU");
    m_LocFlipV           = glGetUniformLocation(m_Program, "flipV");
    m_LocUVOffset        = glGetUniformLocation(m_Program, "uvOffset");
    m_LocUVScale         = glGetUniformLocation(m_Program, "uvScale");
    m_LocUseVertexColors = glGetUniformLocation(m_Program, "useVertexColors");
    m_LocBrightness      = glGetUniformLocation(m_Program, "brightness");
    m_LocRenderMode      = glGetUniformLocation(m_Program, "renderMode");
    m_LocEyePos          = glGetUniformLocation(m_Program, "eyePos");
    m_LocDepthMax        = glGetUniformLocation(m_Program, "depthMax");
    m_LocTexture         = glGetUniformLocation(m_Program, "t");
}

void GLSLBackend::BeginFrame(int width, int height, float clearR, float clearG, float clearB) {
    glViewport(0, 0, width, height);
    glClearColor(clearR, clearG, clearB, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(m_Program);
}

void GLSLBackend::EndFrame() {
    glUseProgram(0);
}

void GLSLBackend::SetUniforms(const RenderUniforms& u) {
    glUniformMatrix4fv(m_LocMVP, 1, GL_FALSE, &u.mvp[0][0]);
    glUniformMatrix4fv(m_LocModel, 1, GL_FALSE, &u.model[0][0]);
    glUniform1i(m_LocFlipU, u.flipU ? 1 : 0);
    glUniform1i(m_LocFlipV, u.flipV ? 1 : 0);
    glUniform2f(m_LocUVOffset, u.uvOffset.x, u.uvOffset.y);
    glUniform2f(m_LocUVScale, u.uvScale.x, u.uvScale.y);
    glUniform1i(m_LocUseVertexColors, u.useVertexColors ? 1 : 0);
    glUniform1f(m_LocBrightness, u.brightness);
    glUniform1i(m_LocRenderMode, u.renderMode);
    glUniform3f(m_LocEyePos, u.eyePos.x, u.eyePos.y, u.eyePos.z);
    glUniform1f(m_LocDepthMax, u.depthMax);
    glUniform1i(m_LocTexture, 0);
}

void GLSLBackend::BindTexture(uint32_t textureID) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textureID);
}

void GLSLBackend::DrawMesh(uint32_t vao, uint32_t vertexCount) {
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, vertexCount);
    glBindVertexArray(0);
}

void GLSLBackend::DrawLines(uint32_t vao, uint32_t vertexCount) {
    glBindVertexArray(vao);
    glDrawArrays(GL_LINES, 0, vertexCount);
    glBindVertexArray(0);
}
