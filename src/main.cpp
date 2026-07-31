#include <iostream>
#include <vector>
#include <string>
#include <cmath>

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"

#include "Common.h"
#include "Loader.h"
#include "UI.h"

// Build a view matrix from orbit parameters and return camera world position
static glm::mat4 BuildView(glm::vec3& outEye) {
    float yRad = glm::radians(state.camYaw);
    float pRad = glm::radians(glm::clamp(state.camPitch, -89.0f, 89.0f));
    float dist = std::max(state.camDist, 0.1f);

    glm::vec3 target(state.camTargetX, state.camTargetY, state.camTargetZ);
    glm::vec3 offset(
         dist * cosf(pRad) * sinf(yRad),
         dist * sinf(pRad),
         dist * cosf(pRad) * cosf(yRad)
    );
    outEye = target + offset;
    return glm::lookAt(outEye, target, glm::vec3(0, 1, 0));
}

// Draw a 3-ring orientation sphere into the given draw list.
// 'view' upper-left 3x3 acts as the world-to-camera rotation.
// Rings: XZ (equatorial/green), XY (blue), YZ (red).
static void DrawOrbitSphere(ImDrawList* dl, ImVec2 ctr, float R, const glm::mat4& view) {
    dl->AddCircleFilled(ctr, R + 3.0f, IM_COL32(14, 14, 18, 220));
    dl->AddCircle(ctr, R + 3.0f, IM_COL32(50, 50, 65, 200), 64, 1.0f);

    glm::mat3 rot = glm::mat3(view); // world -> camera rotation

    struct Ring { glm::vec3 u, v; ImU32 front, back; };
    Ring rings[3] = {
        { {1,0,0}, {0,0,1}, IM_COL32(65,188,75,225),  IM_COL32(28,76,32,70) },  // XZ
        { {1,0,0}, {0,1,0}, IM_COL32(65,125,228,225), IM_COL32(28,52,98,70) },  // XY
        { {0,1,0}, {0,0,1}, IM_COL32(208,72,62,225),  IM_COL32(86,28,26,70) },  // YZ
    };
    const int N = 80;
    for (auto& ring : rings) {
        std::vector<ImVec2> fpts, bpts;
        fpts.reserve(N + 1); bpts.reserve(N + 1);
        for (int i = 0; i <= N; i++) {
            float a = (float)i * 6.28318f / N;
            glm::vec3 w = cosf(a) * ring.u + sinf(a) * ring.v;
            glm::vec3 c = rot * w;
            ImVec2 s(ctr.x + c.x * R, ctr.y - c.y * R);
            (c.z <= 0.0f ? fpts : bpts).push_back(s);
        }
        if (!bpts.empty()) dl->AddPolyline(bpts.data(), (int)bpts.size(), ring.back,  0, 1.2f);
        if (!fpts.empty()) dl->AddPolyline(fpts.data(), (int)fpts.size(), ring.front, 0, 1.8f);
    }
    // Axis dot labels (front-facing only)
    struct Ax { glm::vec3 d; ImU32 col; const char* lbl; };
    Ax axes[3] = {
        { {1,0,0}, IM_COL32(218,72,52,255),  "X" },
        { {0,1,0}, IM_COL32(55,192,55,255),  "Y" },
        { {0,0,1}, IM_COL32(62,122,222,255), "Z" },
    };
    for (auto& ax : axes) {
        glm::vec3 c = rot * ax.d;
        if (c.z > 0.0f) continue;
        ImVec2 s(ctr.x + c.x * R * 0.86f, ctr.y - c.y * R * 0.86f);
        dl->AddCircleFilled(s, 3.5f, ax.col);
        dl->AddText(ImVec2(s.x + 5.0f, s.y - 7.0f), ax.col, ax.lbl);
    }
}

// Compile + link a program, reporting the driver's log instead of silently
// handing back a broken (black-screen) program object.
static GLuint MakeProgram(const char* name, const char* vsSrc, const char* fsSrc) {
    auto compile = [&](GLenum stage, const char* src, const char* stageName) -> GLuint {
        GLuint sh = glCreateShader(stage);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = GL_FALSE;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            std::cerr << "[shader] " << name << " / " << stageName
                      << " failed to compile:\n" << log << std::endl;
        }
        return sh;
    };

    GLuint vs = compile(GL_VERTEX_SHADER,   vsSrc, "vertex");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fsSrc, "fragment");
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "[shader] " << name << " failed to link:\n" << log << std::endl;
    }
    glDetachShader(prog, vs); glDeleteShader(vs);
    glDetachShader(prog, fs); glDeleteShader(fs);
    return prog;
}

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return 1;
    }

    // The shaders are `#version 330 core`, so ask for a matching context instead
    // of taking whatever the driver defaults to (a compatibility/2.1 context on
    // macOS, where every shader would then fail to compile).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* win = SDL_CreateWindow("SHO Viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(win); SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    glewExperimental = GL_TRUE;   // required for a core profile context
    GLenum glewErr = glewInit();
    if (glewErr != GLEW_OK) {
        std::cerr << "glewInit failed: " << glewGetErrorString(glewErr) << std::endl;
        return 1;
    }
    glGetError();   // swallow the spurious INVALID_ENUM glewExperimental produces

    // Load only once a GL context exists — LoadLevel() uploads buffers and
    // textures, and it used to run before SDL was even initialised.
    // The TXD list is optional: textures may be embedded in the container.
    if (argc >= 2) {
        std::vector<std::string> txds;
        for (int i = 2; i < argc; i++) txds.push_back(argv[i]);
        LoadLevel(argv[1], txds);
    }

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't write imgui.ini

    // ---- Dark theme (Unity-like) ----
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.WindowPadding     = ImVec2(10, 10);
    style.FramePadding      = ImVec2(6, 4);
    style.ItemSpacing       = ImVec2(8, 5);
    style.IndentSpacing     = 16.0f;
    style.ScrollbarSize     = 12.0f;
    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]          = ImVec4(0.09f, 0.09f, 0.10f, 0.97f);
    c[ImGuiCol_ChildBg]           = ImVec4(0.07f, 0.07f, 0.08f, 0.90f);
    c[ImGuiCol_PopupBg]           = ImVec4(0.09f, 0.09f, 0.11f, 0.97f);
    c[ImGuiCol_Border]            = ImVec4(0.28f, 0.28f, 0.32f, 0.55f);
    c[ImGuiCol_FrameBg]           = ImVec4(0.14f, 0.14f, 0.16f, 1.00f);
    c[ImGuiCol_FrameBgHovered]    = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_FrameBgActive]     = ImVec4(0.26f, 0.26f, 0.30f, 1.00f);
    c[ImGuiCol_TitleBg]           = ImVec4(0.07f, 0.07f, 0.08f, 1.00f);
    c[ImGuiCol_TitleBgActive]     = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_Header]            = ImVec4(0.16f, 0.16f, 0.20f, 0.80f);
    c[ImGuiCol_HeaderHovered]     = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_HeaderActive]      = ImVec4(0.28f, 0.28f, 0.34f, 1.00f);
    c[ImGuiCol_Button]            = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_ButtonHovered]     = ImVec4(0.26f, 0.26f, 0.30f, 1.00f);
    c[ImGuiCol_ButtonActive]      = ImVec4(0.34f, 0.34f, 0.40f, 1.00f);
    c[ImGuiCol_SliderGrab]        = ImVec4(0.42f, 0.42f, 0.50f, 1.00f);
    c[ImGuiCol_SliderGrabActive]  = ImVec4(0.54f, 0.54f, 0.62f, 1.00f);
    c[ImGuiCol_CheckMark]         = ImVec4(0.80f, 0.80f, 0.86f, 1.00f);
    c[ImGuiCol_Tab]               = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TabHovered]        = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_TabActive]         = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_Separator]         = ImVec4(0.28f, 0.28f, 0.34f, 0.80f);
    c[ImGuiCol_ScrollbarBg]       = ImVec4(0.06f, 0.06f, 0.07f, 0.80f);
    c[ImGuiCol_ScrollbarGrab]     = ImVec4(0.24f, 0.24f, 0.28f, 1.00f);

    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

    const char* vS = R"(
#version 330 core
layout(location=0) in vec3 P;
layout(location=1) in vec2 T;
layout(location=2) in vec4 C;
out vec2  TC;
out vec4  VC;
out vec3  fragWorldPos;
uniform mat4  m;
uniform bool  flipU;
uniform bool  flipV;
uniform vec2  uvOffset;
uniform vec2  uvScale;
void main(){
    gl_Position  = m * vec4(P, 1.0);
    fragWorldPos = P;
    vec2 coord = T;
    if(flipU) coord.x = 1.0 - coord.x;
    if(flipV) coord.y = 1.0 - coord.y;
    TC = (coord * uvScale) + uvOffset;
    VC = C;
}
)";

    const char* fS = R"(
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
        // Vertex Color
        if(VC.a < 0.05) discard;
        FragColor = vec4(VC.rgb * brightness, VC.a);
    } else if(renderMode == 2){
        // Flat Shaded
        vec3 L    = normalize(vec3(0.55, 1.0, 0.45));
        float d   = max(dot(N, L), 0.0) * 0.72 + 0.28;
        FragColor = vec4(vec3(0.70, 0.72, 0.76) * d * brightness, 1.0);
    } else if(renderMode == 3){
        // Normals
        FragColor = vec4(N * 0.5 + 0.5, 1.0);
    } else if(renderMode == 4){
        // Depth
        float dist = distance(fragWorldPos, eyePos);
        float v    = clamp(1.0 - dist / depthMax, 0.0, 1.0);
        v = v * v;
        FragColor  = vec4(vec3(v), 1.0);
    } else if(renderMode == 5){
        // Checker
        vec2 ch = floor(TC * 8.0);
        float c = mod(ch.x + ch.y, 2.0) < 1.0 ? 0.82 : 0.18;
        FragColor = vec4(vec3(c), 1.0);
    } else if(renderMode == 6){
        // Unlit
        vec4 tex = texture(t, TC);
        if(tex.a < 0.1) discard;
        FragColor = vec4(tex.rgb * brightness, tex.a);
    } else {
        // Textured (default, renderMode == 0)
        vec4 tex = texture(t, TC);
        if(tex.a < 0.1) discard;
        vec4 col = useVertexColors ? tex * VC : tex;
        col.rgb *= brightness;
        FragColor = col;
    }
}
)";

    GLuint p = MakeProgram("scene", vS, fS);

    // ---- Solid-colour shader (collision wireframe + clump markers) ----
    const char* colvS = R"(
#version 330 core
layout(location=0) in vec3 P;
uniform mat4 m;
void main(){ gl_Position = m * vec4(P, 1.0); }
)";
    const char* colfS = R"(
#version 330 core
out vec4 FragColor;
uniform vec4 solidColor;
void main(){ FragColor = solidColor; }
)";
    GLuint collProg = MakeProgram("solid", colvS, colfS);

    // ---- Sky / gradient background shader (fullscreen quad via gl_VertexID) ----
    const char* skyVS = R"(
#version 330 core
out vec2 fragY;
void main(){
    // Two-triangle fullscreen quad from vertex id 0-5
    vec2 pos[6] = vec2[6](
        vec2(-1,-1),vec2(1,-1),vec2(1,1),
        vec2(-1,-1),vec2(1,1), vec2(-1,1));
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
    fragY = pos[gl_VertexID] * 0.5 + 0.5;  // 0=bottom 1=top
}
)";
    const char* skyFS = R"(
#version 330 core
in  vec2 fragY;
out vec4 FragColor;
uniform vec3 skyTop;
uniform vec3 skyBot;
void main(){
    FragColor = vec4(mix(skyBot, skyTop, fragY.y), 1.0);
}
)";
    GLuint skyProg = MakeProgram("sky", skyVS, skyFS);
    // Empty VAO required by core profile for attributeless draws
    GLuint skyVao;
    glGenVertexArrays(1, &skyVao);

    // Mouse orbit state
    bool  mouseRight = false;
    int   prevMouseX = 0, prevMouseY = 0;

    // Who owns the mouse. Computed at the end of each frame and consumed by the
    // next frame's event loop, because SDL events are polled before NewFrame().
    //
    // io.WantCaptureMouse is deliberately NOT used here: ImGuizmo raises it via
    // SetNextFrameWantCaptureMouse() as soon as the cursor merely *hovers* a gizmo
    // handle, which used to kill wheel-zoom and right-drag orbit across the whole
    // middle of the viewport. Hovering the gizmo must not block the camera —
    // only an active left-button manipulation does.
    bool viewportOwnsMouse = true;

    // Gizmo / orbit-sphere interaction state (persist across frames)
    bool sphereDragging = false;
    bool gizmoUsing     = false;

    bool run = true;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);

            if (e.type == SDL_QUIT) run = false;

            // Mouse wheel zoom — proportional so zooming stays usable at any scale
            if (e.type == SDL_MOUSEWHEEL && viewportOwnsMouse) {
                state.camDist = glm::clamp(
                    state.camDist * powf(0.9f, (float)e.wheel.y), 0.5f, 2000.0f);
            }

            // Right mouse button drag → orbit (yaw / pitch)
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT && viewportOwnsMouse) {
                mouseRight = true;
                prevMouseX = e.button.x;
                prevMouseY = e.button.y;
            }
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) {
                mouseRight = false;
            }
            // Once a drag has started it keeps running even if the cursor leaves the
            // viewport, otherwise the orbit stutters whenever it crosses a panel.
            if (e.type == SDL_MOUSEMOTION && mouseRight) {
                float dx = (float)(e.motion.x - prevMouseX);
                float dy = (float)(e.motion.y - prevMouseY);
                state.camYaw   += dx * 0.4f;
                state.camPitch  = glm::clamp(state.camPitch - dy * 0.4f, -89.0f, 89.0f);
                prevMouseX = e.motion.x;
                prevMouseY = e.motion.y;
            }
        }

        // Logical window size — this is the coordinate space ImGui and ImGuizmo
        // report mouse positions in, so all UI/gizmo rects must use it.
        int winW, winH;
        SDL_GetWindowSize(win, &winW, &winH);
        // Framebuffer size — differs from the logical size on HiDPI/Retina displays.
        // glViewport must use this one; using winW/winH rendered the 3-D scene into
        // a fraction of the framebuffer while the gizmo overlay covered the whole
        // window, so the gizmo did not line up with the geometry at all.
        int fbW, fbH;
        SDL_GL_GetDrawableSize(win, &fbW, &fbH);
        float aspect = fbH > 0 ? (float)fbW / fbH : 1.0f;

        // Build matrices
        glm::vec3 eye;
        glm::mat4 view = BuildView(eye);
        glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 2000.0f);
        glm::mat4 mvp  = proj * view;

        // --- Render 3-D scene ---
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        // Key 1 → reset camera
        if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_1, false)) {
            state.camTargetX = 0; state.camTargetY = 2; state.camTargetZ = 0;
            state.camYaw = 0; state.camPitch = 20; state.camDist = 15;
        }

        glViewport(0, 0, fbW, fbH);
        glClearColor(state.skyColorBot[0], state.skyColorBot[1], state.skyColorBot[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        // Draw gradient sky before any geometry
        if (state.skyGradient) {
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glUseProgram(skyProg);
            glUniform3fv(glGetUniformLocation(skyProg, "skyTop"), 1, state.skyColorTop);
            glUniform3fv(glGetUniformLocation(skyProg, "skyBot"), 1, state.skyColorBot);
            glBindVertexArray(skyVao);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            glBindVertexArray(0);
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
        }
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        if (state.showWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        else                     glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

        GLint filter = state.linearFilter ? GL_LINEAR : GL_NEAREST;
        for (auto const& [name, id] : g_TextureMap) {
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
            if (state.forceRepeat) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            }
        }

        glUseProgram(p);
        glUniformMatrix4fv(glGetUniformLocation(p, "m"),    1, GL_FALSE, glm::value_ptr(mvp));
        glUniform1i(glGetUniformLocation(p, "flipU"),        state.flipU);
        glUniform1i(glGetUniformLocation(p, "flipV"),        state.flipV);
        glUniform2f(glGetUniformLocation(p, "uvOffset"),     state.uvOffsetX, state.uvOffsetY);
        glUniform2f(glGetUniformLocation(p, "uvScale"),      state.uvScaleX,  state.uvScaleY);
        glUniform1i(glGetUniformLocation(p, "useVertexColors"), state.useVertexColors);
        glUniform1f(glGetUniformLocation(p, "brightness"),   state.brightness);
        glUniform1i(glGetUniformLocation(p, "renderMode"),   (int)state.renderMode);
        glUniform3f(glGetUniformLocation(p, "eyePos"),       eye.x, eye.y, eye.z);
        glUniform1f(glGetUniformLocation(p, "depthMax"),     state.camDist * 4.5f);

        for (const auto& chunk : g_Chunks) {
            // Use the directly stored texName (set at load time per-geometry-object)
            const std::string& tName = chunk.texName;
            GLuint tid = 0;
            if (g_TextureMap.count(tName)) tid = g_TextureMap[tName];
            else {
                std::string upper = tName;
                for (auto& ch : upper) ch = (char)toupper((unsigned char)ch);
                if (g_TextureMap.count(upper)) tid = g_TextureMap[upper];
            }
            glBindTexture(GL_TEXTURE_2D, tid);
            glBindVertexArray(chunk.vao);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)chunk.vertices.size());
        }
        glBindVertexArray(0);

        // --- Collision render pass (solid fill + wireframe) ---
        if (state.showCollision && g_Collision.uploaded && !g_Collision.indices.empty()) {
            glUseProgram(collProg);
            glUniformMatrix4fv(glGetUniformLocation(collProg, "m"), 1, GL_FALSE, glm::value_ptr(mvp));
            glDisable(GL_CULL_FACE);
            glBindVertexArray(g_Collision.vao);

            if (state.showCollisionSolid) {
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glUniform4f(glGetUniformLocation(collProg, "solidColor"), 0.10f, 0.80f, 0.20f, 0.28f);
                glDrawElements(GL_TRIANGLES, (GLsizei)g_Collision.indices.size(), GL_UNSIGNED_INT, nullptr);
            }

            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            glLineWidth(1.4f);
            glUniform4f(glGetUniformLocation(collProg, "solidColor"), 0.15f, 0.95f, 0.30f, 0.85f);
            glDrawElements(GL_TRIANGLES, (GLsizei)g_Collision.indices.size(), GL_UNSIGNED_INT, nullptr);
            glBindVertexArray(0);

            if (!state.showWireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            // Restore, don't enable: culling is off for the whole viewer (the PS2
            // strips have inconsistent winding). Unconditionally enabling it here
            // made half the level vanish from the frame after collision was first
            // switched on, and it never came back.
            glDisable(GL_CULL_FACE);
            glLineWidth(1.0f);
        }

        // --- Clump object markers (octahedron wireframe + label) ---
        if (state.showClumps && !g_Clumps.empty()) {
            // Build the octahedron edges for all clumps into one batch
            // Octahedron vertices relative to center: ±R along each axis
            const float R = 0.35f;
            // 12 edges × 2 verts = 24 line vertices per clump
            // Edges: top/bottom ↔ 4 equatorial, plus equatorial ring
            const glm::vec3 O[6] = {
                { R, 0, 0}, {-R, 0, 0},
                {0,  R, 0}, {0, -R, 0},
                {0, 0,  R}, {0, 0, -R},
            };
            // 12 edges by index pair
            const int EDGES[12][2] = {
                {2,0},{2,4},{2,1},{2,5}, // top to equatorial
                {3,0},{3,4},{3,1},{3,5}, // bottom to equatorial
                {0,4},{4,1},{1,5},{5,0}, // equatorial ring
            };

            std::vector<glm::vec3> lineVerts;
            lineVerts.reserve(g_Clumps.size() * 24);
            for (const auto& cl : g_Clumps) {
                for (auto& ed : EDGES) {
                    lineVerts.push_back(cl.position + O[ed[0]]);
                    lineVerts.push_back(cl.position + O[ed[1]]);
                }
                // three long axis lines for orientation
                lineVerts.push_back(cl.position);
                lineVerts.push_back(cl.position + glm::vec3(cl.transform[0]) * 0.5f);
                lineVerts.push_back(cl.position);
                lineVerts.push_back(cl.position + glm::vec3(cl.transform[1]) * 0.5f);
                lineVerts.push_back(cl.position);
                lineVerts.push_back(cl.position + glm::vec3(cl.transform[2]) * 0.5f);
            }

            GLuint lvao, lvbo;
            glGenVertexArrays(1, &lvao); glGenBuffers(1, &lvbo);
            glBindVertexArray(lvao);
            glBindBuffer(GL_ARRAY_BUFFER, lvbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizei)(lineVerts.size() * sizeof(glm::vec3)),
                         lineVerts.data(), GL_DYNAMIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr);
            glEnableVertexAttribArray(0);

            glUseProgram(collProg);
            glUniformMatrix4fv(glGetUniformLocation(collProg, "m"), 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform4f(glGetUniformLocation(collProg, "solidColor"), 1.0f, 0.72f, 0.10f, 1.0f);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glLineWidth(2.0f);
            glDrawArrays(GL_LINES, 0, (GLsizei)lineVerts.size());
            glLineWidth(1.0f);
            glBindVertexArray(0);
            glDeleteVertexArrays(1, &lvao);
            glDeleteBuffers(1, &lvbo);

            // Screen-space labels for each clump (projected)
            auto* fdl = ImGui::GetForegroundDrawList();
            for (const auto& cl : g_Clumps) {
                glm::vec4 clip = proj * view * glm::vec4(cl.position, 1.0f);
                if (clip.w <= 0.0f) continue;
                glm::vec3 ndc = glm::vec3(clip) / clip.w;
                if (std::abs(ndc.x) > 1.1f || std::abs(ndc.y) > 1.1f) continue;
                float sx = (ndc.x * 0.5f + 0.5f) * (float)winW;
                float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * (float)winH;
                const char* lbl = cl.label.c_str();
                ImVec2 ts = ImGui::CalcTextSize(lbl);
                fdl->AddRectFilled(
                    ImVec2(sx - ts.x * 0.5f - 3.0f, sy - ts.y - 8.0f),
                    ImVec2(sx + ts.x * 0.5f + 3.0f, sy - 4.0f),
                    IM_COL32(20, 20, 28, 180), 3.0f);
                fdl->AddText(ImVec2(sx - ts.x * 0.5f, sy - ts.y - 7.0f),
                    IM_COL32(255, 192, 40, 255), lbl);
            }
        }

        glUseProgram(p);

        // -- Orbit sphere geometry (hit region only; drawn further down) -----
        // The circle is computed before the gizmo runs so the two widgets can
        // arbitrate over the same left button instead of both grabbing it.
        const float SR = 54.0f;
        const ImVec2 sphereCtr((float)winW - 10.0f - 122.0f * 0.5f, 10.0f + SR + 8.0f);
        const float sdx = io.MousePos.x - sphereCtr.x;
        const float sdy = io.MousePos.y - sphereCtr.y;
        const bool  overSphere = (sdx*sdx + sdy*sdy <= (SR + 3.0f)*(SR + 3.0f));

        // -- ImGuizmo translate pivot ----------------------------------------
        // Runs before every IsOver()/IsUsing() query below: ImGuizmo only refreshes
        // its hover/use state inside Manipulate(), so querying it earlier in the
        // frame returned data from the previous frame.
        gizmoUsing = false;
        if (state.showPivotGizmo) {
            // Enable() only suppresses interaction — Manipulate() still draws the
            // gizmo — so hiding it has to skip the call entirely.
            // Hand the left button to the orbit sphere when the cursor is on it,
            // but never cancel a manipulation that is already in progress.
            ImGuizmo::Enable(!sphereDragging && (ImGuizmo::IsUsing() || !overSphere));

            ImGuizmo::SetOrthographic(false);
            ImGuizmo::AllowAxisFlip(false);          // no confusing flips
            ImGuizmo::SetGizmoSizeClipSpace(0.12f);
            // No SetDrawlist(): ImGuizmo::BeginFrame() already installed its own
            // full-screen NoInputs window. Pointing it at the foreground draw list
            // made the gizmo paint over every panel and broke the hover test that
            // keeps it from reacting to clicks landing on the UI.
            ImGuizmo::SetRect(0.0f, 0.0f, (float)winW, (float)winH);

            float viewArr[16], projArr[16], matArr[16];
            memcpy(viewArr, glm::value_ptr(view), sizeof(viewArr));
            memcpy(projArr, glm::value_ptr(proj), sizeof(projArr));
            glm::mat4 pivotMat = glm::translate(glm::mat4(1.0f),
                glm::vec3(state.camTargetX, state.camTargetY, state.camTargetZ));
            memcpy(matArr, glm::value_ptr(pivotMat), sizeof(matArr));

            const bool snapping = state.pivotSnapOn || io.KeyCtrl;
            const float snapStep = std::max(state.pivotSnap, 0.001f);
            const float snapVec[3] = { snapStep, snapStep, snapStep };

            // Take the manipulated matrix verbatim. The previous code applied only
            // 35 % of (result - current) per frame as a "sensitivity" tweak, but
            // ImGuizmo returns the *absolute* position the cursor projects to, not
            // an incremental delta — so scaling it turned the drag into a per-frame
            // exponential lag: the pivot never reached the cursor, kept creeping for
            // a while after the mouse stopped, and moved at a different speed
            // depending on the frame rate.
            if (ImGuizmo::Manipulate(viewArr, projArr,
                    ImGuizmo::TRANSLATE, ImGuizmo::WORLD, matArr,
                    nullptr, snapping ? snapVec : nullptr)) {
                state.camTargetX = matArr[12];
                state.camTargetY = matArr[13];
                state.camTargetZ = matArr[14];
            }

            gizmoUsing = ImGuizmo::IsUsing();
            // No SDL_CaptureMouse() here: imgui_impl_sdl2 already calls it every
            // frame from the button mask, so the manual calls were dead at best and
            // released a capture the orbit sphere still needed at worst.
        }

        // -- Orbit sphere overlay (top-right, direct circular hit-test) ------
        {
            DrawOrbitSphere(ImGui::GetForegroundDrawList(), sphereCtr, SR, view);

            if (!sphereDragging && overSphere
                    && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                    && !gizmoUsing
                    && !ImGuizmo::IsOver()
                    && !io.WantCaptureMouse) {
                sphereDragging = true;
            }
            if (sphereDragging) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    state.camYaw   += io.MouseDelta.x * 0.32f;
                    state.camPitch  = glm::clamp(
                        state.camPitch - io.MouseDelta.y * 0.32f, -89.0f, 89.0f);
                } else {
                    sphereDragging = false;
                }
            }
        }

        // -- Main control panel (pinned top-left, fixed 256 px, scrollable) --
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(256, (float)winH - 20.0f), ImGuiCond_Always);
        ImGui::Begin("SHO Viewer", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

        if (ImGui::Button("Open Level", ImVec2(-1, 0))) g_FileBrowser.Open(true);

        if (!g_CurrentMeshContainer.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.52f, 0.86f, 0.52f, 1.0f), "%s",
                fs::path(g_CurrentMeshContainer).filename().string().c_str());
            ImGui::TextDisabled("%zu meshes  |  %zu textures",
                g_Chunks.size(), g_TextureMap.size() / 2);
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        // ---- Camera --------------------------------------------------
        ImGui::TextDisabled("Camera");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##dist", &state.camDist, 1.0f, 200.0f, "Dist %.1f");
        if (ImGui::Button("Reset Camera", ImVec2(-1, 0))) {
            state.camTargetX = 0; state.camTargetY = 2; state.camTargetZ = 0;
            state.camYaw = 0; state.camPitch = 20; state.camDist = 15;
        }

        ImGui::Checkbox("Pivot gizmo", &state.showPivotGizmo);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag the arrows/planes to move the orbit pivot.\n"
                              "Hold Ctrl while dragging to snap.");
        if (state.showPivotGizmo) {
            ImGui::SameLine(128);
            ImGui::Checkbox("Snap", &state.pivotSnapOn);
            ImGui::SetNextItemWidth(-44.0f);
            ImGui::DragFloat("##snapstep", &state.pivotSnap, 0.05f, 0.01f, 100.0f, "%.2f");
            ImGui::SameLine(); ImGui::TextDisabled("Step");
            ImGui::TextDisabled("Pivot %.2f  %.2f  %.2f",
                state.camTargetX, state.camTargetY, state.camTargetZ);
        }
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        // ---- Render mode ------------------------------------------------
        ImGui::TextDisabled("Render Mode");
        ImGui::Spacing();

        struct ModeBtn { const char* label; RenderMode mode; const char* tip; };
        const ModeBtn MODES[] = {
            {"Textured",    RenderMode::Textured,    "Texture + vertex colors"},
            {"Vert.Color",  RenderMode::VertexColor, "Vertex colors only"},
            {"Flat",        RenderMode::FlatShaded,  "Per-face shading, no texture"},
            {"Normals",     RenderMode::Normals,     "Face normals as RGB"},
            {"Depth",       RenderMode::Depth,       "Linear depth grey-scale"},
            {"Checker",     RenderMode::Checker,     "UV checkerboard"},
            {"Unlit",       RenderMode::Unlit,       "Texture, no lighting"},
        };
        const float BTN_W = (248.0f - 20.0f - 2.0f * 6.0f) / 4.0f;
        int mi = 0;
        for (auto& mb : MODES) {
            bool active = (state.renderMode == mb.mode);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.32f, 0.32f, 0.42f, 1.0f));
            if (ImGui::Button(mb.label, ImVec2(BTN_W, 22.0f)))
                state.renderMode = mb.mode;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", mb.tip);
            if (active) ImGui::PopStyleColor();
            if (++mi % 4 != 0) ImGui::SameLine(0, 3.0f);
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        // ---- Display options ----------------------------------------
        ImGui::TextDisabled("Display");
        ImGui::Checkbox("Wireframe",   &state.showWireframe); ImGui::SameLine(128);
        ImGui::Checkbox("Linear",      &state.linearFilter);
        ImGui::Checkbox("Vert.Colors", &state.useVertexColors); ImGui::SameLine(128);
        ImGui::Checkbox("Repeat UV",   &state.forceRepeat);
        ImGui::SetNextItemWidth(-44.0f);
        ImGui::SliderFloat("##bright", &state.brightness, 0.5f, 3.0f);
        ImGui::SameLine(); ImGui::TextDisabled("Bright");

        // ---- Overlay objects (shown when loaded) -------------------
        if (g_Collision.uploaded || !g_Clumps.empty()) {
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            ImGui::TextDisabled("Overlay");
            if (g_Collision.uploaded) {
                ImGui::Checkbox("Collision Wire", &state.showCollision);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%zu verts  %zu tris",
                        g_Collision.verts.size(), g_Collision.indices.size() / 3);
                if (state.showCollision) {
                    ImGui::SameLine(128);
                    ImGui::Checkbox("Solid##cs", &state.showCollisionSolid);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Semi-transparent fill");
                }
            }
            if (!g_Clumps.empty()) {
                ImGui::Checkbox("Clumps", &state.showClumps);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%zu clump objects", g_Clumps.size());
            }
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        // ---- Panels & extras ---------------------------------------
        ImGui::TextDisabled("Panels");
        ImGui::Checkbox("Structure", &state.showStructure); ImGui::SameLine(128);
        ImGui::Checkbox("Textures",  &state.showTextures);

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        if (ImGui::CollapsingHeader("UV Overrides")) {
            ImGui::Checkbox("Flip U", &state.flipU); ImGui::SameLine(128);
            ImGui::Checkbox("Flip V", &state.flipV);
            ImGui::SetNextItemWidth(-54.0f); ImGui::SliderFloat("##ux", &state.uvOffsetX, -1.f, 1.f); ImGui::SameLine(); ImGui::TextDisabled("Off X");
            ImGui::SetNextItemWidth(-54.0f); ImGui::SliderFloat("##uy", &state.uvOffsetY, -1.f, 1.f); ImGui::SameLine(); ImGui::TextDisabled("Off Y");
            ImGui::SetNextItemWidth(-54.0f); ImGui::SliderFloat("##sx", &state.uvScaleX,  0.1f, 5.f); ImGui::SameLine(); ImGui::TextDisabled("Sc X");
            ImGui::SetNextItemWidth(-54.0f); ImGui::SliderFloat("##sy", &state.uvScaleY,  0.1f, 5.f); ImGui::SameLine(); ImGui::TextDisabled("Sc Y");
            if (ImGui::Button("Reset UV", ImVec2(-1, 0))) {
                state.flipU = false; state.flipV = false;
                state.uvOffsetX = 0; state.uvOffsetY = 0;
                state.uvScaleX  = 1; state.uvScaleY  = 1;
            }
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        if (ImGui::CollapsingHeader("Background")) {
            ImGui::Checkbox("Gradient sky", &state.skyGradient);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Draw a vertical colour gradient behind the scene");
            ImGui::Spacing();
            ImGui::TextDisabled(state.skyGradient ? "Top colour" : "Clear colour");
            ImGui::SetNextItemWidth(-1);
            ImGui::ColorEdit3("##skyTop", state.skyColorTop,
                ImGuiColorEditFlags_Float | ImGuiColorEditFlags_PickerHueBar);
            if (state.skyGradient) {
                ImGui::TextDisabled("Bottom colour");
                ImGui::SetNextItemWidth(-1);
                ImGui::ColorEdit3("##skyBot", state.skyColorBot,
                    ImGuiColorEditFlags_Float | ImGuiColorEditFlags_PickerHueBar);
            }
            if (ImGui::Button("Reset##bg", ImVec2(-1, 0))) {
                state.skyColorTop[0] = 0.07f; state.skyColorTop[1] = 0.07f; state.skyColorTop[2] = 0.09f;
                state.skyColorBot[0] = 0.11f; state.skyColorBot[1] = 0.11f; state.skyColorBot[2] = 0.14f;
                state.skyGradient = false;
            }
        }

        ImGui::End();

        if (state.showStructure) RenderStructureWindow();
        if (state.showTextures)  RenderTxdWindow();

        // File browser
        g_FileBrowser.Render();

        // Decide who owns the mouse for the *next* frame's event loop. Panels and
        // active widgets win; a hovered (but not dragged) gizmo does not.
        {
            const bool overPanel = ImGui::IsWindowHovered(
                ImGuiHoveredFlags_AnyWindow |
                ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            viewportOwnsMouse = !overPanel && !ImGui::IsAnyItemActive()
                                && !gizmoUsing && !sphereDragging;
        }

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(win);
    }

    glDeleteVertexArrays(1, &skyVao);

    for (auto& chunk : g_Chunks) {
        if (chunk.vao) glDeleteVertexArrays(1, &chunk.vao);
        if (chunk.vbo) glDeleteBuffers(1, &chunk.vbo);
    }
    for (auto& [name, id] : g_TextureMap) glDeleteTextures(1, &id);
    g_Collision.Free();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

