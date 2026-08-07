#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <fstream>

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#include "ImGuizmo.h"
#include "im_anim.h"
#include "ClimaxEngine/Game/CameraLinks.h"
#include "ClimaxEngine/Game/CharacterController.h"
#include "ClimaxEngine/Game/ZoneLinks.h"
#include "ClimaxEngine/Render/PlayerModel.h"

#include "ClimaxEngine/Core/RWS/FileSystem/CArchiveManager.h"
#include "ClimaxEngine/Core/Common.h"
#include "ClimaxEngine/Render/GPUMesh.h"
#include "ClimaxEngine/SG/SceneObject.h"
#include "ClimaxEngine/Loader/Export.h"
#include "ClimaxEngine/Loader/Loader.h"
#include "ClimaxEngine/Rendering/CPURasterizer.h"
#include "ClimaxEngine/UI/UI.h"
#include "ClimaxEngine/Platform/PS2/AudioParser.h"

// ---------------------------------------------------------------------------
// Audio playback
//
// One SDL device at a time, reopened whenever the next clip has a different
// rate or channel count -- the level banks alone span 6 kHz mono to 32 kHz,
// and the cutscene streams are 48 kHz stereo.
// ---------------------------------------------------------------------------
#include "ClimaxEngine/Platform/PS2/RwsAudio.h"

ClimaxEngine::RWS::FileSystem::CArchive g_IgcArc;
const AudioClip& CurrentAudioClip() { return ClimaxEngine::Audio::CAudioRelay::GetInstance().CurrentAudioClip(); }
void PlayAudioClip(const AudioClip& clip) {
    ClimaxEngine::Audio::CAudioRelay::GetInstance().PlayAudioClip(clip);
    state.showAudioPlayer = true;
}

void PlayLibraryEntry(int index) {
    if (index < 0 || index >= (int)g_AudioLibrary.size()) return;
    const AudioSourceRef& ref = g_AudioLibrary[(size_t)index];
    AudioClip clip;
    if (ref.arcIndex >= 0 && g_IgcArc.IsOpen()) {
        std::vector<uint8_t> blob;
        if (!g_IgcArc.Read((size_t)ref.arcIndex, blob) || blob.empty()) return;
        clip.name = ref.name;
        if (!Audio::LoadBuffer(blob.data(), blob.size(), clip)) return;
        clip.name = ref.name;
    } else if (!Audio::LoadFile(ref.path, clip)) {
        return;
    }
    clip.name = ref.name;
    PlayAudioClip(clip);
}

void ToggleAudioPlayback() { ClimaxEngine::Audio::CAudioRelay::GetInstance().ToggleAudioPlayback(); }
void StopAudio() { ClimaxEngine::Audio::CAudioRelay::GetInstance().StopAudio(); }
void SetAudioProgress(float progress) { ClimaxEngine::Audio::CAudioRelay::GetInstance().SetAudioProgress(progress); }
void AudioHealth(int& calls, int& late, double& worstMs, double& bufferMs) { calls = late = 0; worstMs = bufferMs = 0.0; }

// The music and cutscenes are not inside SH.ARC: MUSIC/ and IGC.ARC sit beside
// it on the disc. Mounting the archive is enough to find them.
void ScanAudioLibrary() {
    g_AudioLibrary.clear();
    if (!ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()) return;

    std::error_code ec;
    const fs::path root = fs::path(ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Path()).parent_path();

    for (const char* dirName : {"MUSIC", "Music", "music"}) {
        const fs::path dir = root / dirName;
        if (!fs::is_directory(dir, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (ext != ".rws" && ext != ".vag" && ext != ".ads") continue;
            AudioSourceRef ref;
            ref.name  = it->path().stem().string();
            ref.group = "Music";
            ref.path  = it->path().string();
            g_AudioLibrary.push_back(std::move(ref));
        }
        break;
    }

    for (const char* arcName : {"IGC.ARC", "igc.arc", "Igc.arc"}) {
        const fs::path p = root / arcName;
        if (!fs::is_regular_file(p, ec)) continue;
        if (!g_IgcArc.Open(p.string())) break;
        const auto& entries = g_IgcArc.Entries();
        for (size_t i = 0; i < entries.size(); ++i) {
            AudioSourceRef ref;
            ref.name = entries[i].name;
            const size_t dot = ref.name.find_last_of('.');
            if (dot != std::string::npos) ref.name.resize(dot);
            ref.group    = "Cutscenes";
            ref.arcIndex = (int)i;
            g_AudioLibrary.push_back(std::move(ref));
        }
        break;
    }

    std::cout << "[audio] library: " << g_AudioLibrary.size() << " tracks beside "
              << fs::path(ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Path()).filename().string() << " (looked in "
              << root.string() << ")\n";
    if (!g_AudioLibrary.empty() && !state.audioAutoOpened) {
        state.audioAutoOpened = true;
        state.showAudioPlayer = true;
    }
}

// ---------------------------------------------------------------------------
// Prefs: persist the last opened .arc path so the next launch auto-mounts it.
// File: <basePath>/ClimaxGameEngineToolkit.prefs  (one line = arc path)
// ---------------------------------------------------------------------------
static std::string g_PrefsPath;

static void InitPrefsPath() {
    // SDL_GetBasePath() returns the directory containing the executable.
    char* base = SDL_GetBasePath();
    if (base) {
        g_PrefsPath = std::string(base) + "ClimaxGameEngineToolkit.prefs";
        SDL_free(base);
    } else {
        g_PrefsPath = "ClimaxGameEngineToolkit.prefs";
    }
}

void SaveArcPref(const std::string& arcPath) {
    if (g_PrefsPath.empty() || arcPath.empty()) return;
    std::ofstream f(g_PrefsPath, std::ios::trunc);
    if (f) f << arcPath << "\n";
}

// Not static: the file browser uses it to work out where to open.
std::string LoadArcPref() {
    if (g_PrefsPath.empty()) return {};
    std::ifstream f(g_PrefsPath);
    std::string line;
    if (f && std::getline(f, line) && !line.empty()) return line;
    return {};
}

// ── Doorway state, shared between the step and the overlay ───────────────────
// Which ZoneLink the player is standing in, and the container it leads to. The
// overlay reads them to draw the prompt; the step writes them.
static int         s_zoneLinkHere = -1;
static std::string s_zonePrompt;
// Set by the key handler, consumed and cleared by the step, so a single press
// travels once instead of once per frame it is held.
static bool        s_useDoorPressed = false;

// Where to stand Travis and which way to turn him. Written by the walk step,
// read by the draw pass.
static glm::vec3 s_playerFeet = glm::vec3(0.0f);
static float     s_playerYaw  = 0.0f;

// Build a view matrix from orbit parameters and return camera world position
static glm::mat4 BuildView(glm::vec3& outEye) {
    float yRad = glm::radians(state.camYaw);
    float pRad = glm::radians(glm::clamp(state.camPitch, -89.0f, 89.0f));

    if (state.useWASD) {
        outEye = glm::vec3(state.camPosX, state.camPosY, state.camPosZ);
        glm::vec3 fwd(
             -cosf(pRad) * sinf(yRad),
             -sinf(pRad),
             -cosf(pRad) * cosf(yRad)
        );
        return glm::lookAt(outEye, outEye + fwd, glm::vec3(0, 1, 0));
    }

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

// Append one octahedron-wireframe marker plus three orientation axes.
static void AppendMarker(std::vector<glm::vec3>& out, const glm::vec3& pos,
                         const glm::mat4& xform, float R, float axisLen) {
    const glm::vec3 O[6] = {
        { R, 0, 0}, {-R, 0, 0},
        {0,  R, 0}, {0, -R, 0},
        {0, 0,  R}, {0, 0, -R},
    };
    static const int EDGES[12][2] = {
        {2,0},{2,4},{2,1},{2,5}, // top to equatorial
        {3,0},{3,4},{3,1},{3,5}, // bottom to equatorial
        {0,4},{4,1},{1,5},{5,0}, // equatorial ring
    };
    for (auto& ed : EDGES) {
        out.push_back(pos + O[ed[0]]);
        out.push_back(pos + O[ed[1]]);
    }
    if (axisLen > 0.0f) {
        for (int a = 0; a < 3; a++) {
            out.push_back(pos);
            out.push_back(pos + glm::vec3(xform[a]) * axisLen);
        }
    }
}

// Project a world position to screen space and draw a boxed label.
static void DrawWorldLabel(ImDrawList* dl, const glm::mat4& viewProj,
                           const glm::vec3& pos, int winW, int winH,
                           const char* text, ImU32 col) {
    glm::vec4 clip = viewProj * glm::vec4(pos, 1.0f);
    if (clip.w <= 0.0f) return;
    glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (std::abs(ndc.x) > 1.1f || std::abs(ndc.y) > 1.1f) return;
    float sx = (ndc.x * 0.5f + 0.5f) * (float)winW;
    float sy = (1.0f - (ndc.y * 0.5f + 0.5f)) * (float)winH;
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddRectFilled(
        ImVec2(sx - ts.x * 0.5f - 3.0f, sy - ts.y - 8.0f),
        ImVec2(sx + ts.x * 0.5f + 3.0f, sy - 4.0f),
        IM_COL32(20, 20, 28, 180), 3.0f);
    dl->AddText(ImVec2(sx - ts.x * 0.5f, sy - ts.y - 7.0f), col, text);
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

// Samples a UV animation layer, returning (uScale, vScale, uOffset, vOffset).
// Linear between keyframes, which is the blend `ClimaxT1KeyFrameBlend` does.
static glm::vec4 EvalUVAnim(const UVAnimClip& clip, size_t layer, float t) {
    if (layer >= clip.layers.size()) return glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    const auto& k = clip.layers[layer];
    if (k.empty()) return glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
    if (k.size() == 1) return glm::vec4(k[0].uScale, k[0].vScale, k[0].uOff, k[0].vOff);

    const float dur = clip.duration > 0.0f ? clip.duration : k.back().time;
    const float tt = dur > 0.0f ? std::fmod(t, dur) : 0.0f;
    size_t i = 0;
    while (i + 1 < k.size() && k[i + 1].time <= tt) i++;
    const size_t j = std::min(i + 1, k.size() - 1);
    const float span = k[j].time - k[i].time;
    const float a = span > 1e-6f ? (tt - k[i].time) / span : 0.0f;
    return glm::vec4(glm::mix(k[i].uScale, k[j].uScale, a),
                     glm::mix(k[i].vScale, k[j].vScale, a),
                     glm::mix(k[i].uOff,   k[j].uOff,   a),
                     glm::mix(k[i].vOff,   k[j].vOff,   a));
}

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
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

    SDL_Window* win = SDL_CreateWindow("Climax Silent Hill Engine Toolkit 0.5",
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

    // Initialise prefs now that SDL_GetBasePath() is available
    InitPrefsPath();

    // Load only once a GL context exists — LoadLevel() uploads buffers and
    // textures, and it used to run before SDL was even initialised.
    //
    //   ClimaxGameEngineToolkit SH.ARC [LevelName]        — mount the archive, load by name
    //   ClimaxGameEngineToolkit <container> [txd ...]     — loose files; TXDs are optional
    if (argc >= 2) {
        const std::string first = argv[1];
        const bool looksLikeArc =
            first.size() > 4 && sho_stricmp(first.c_str() + first.size() - 4, ".arc") == 0;

        if (looksLikeArc && ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().Mount(first)) {
            ScanAudioLibrary();
            SaveArcPref(first);                         // ← remember for next launch
            std::cerr << "[arc] mounted " << first << " ("
                      << ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Entries().size() << " files)\n";
            if (argc >= 3 && std::string(argv[2]) != "--export") {
                const int idx = ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Find(argv[2]);
                if (idx >= 0) LoadLevelFromArc(idx);
                else std::cerr << "[arc] no entry named '" << argv[2] << "'\n";
            }
        } else {
            if (looksLikeArc)
                std::cerr << "[arc] " << ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Error() << " — treating as a container\n";
            std::vector<std::string> txds;
            for (int i = 2; i < argc; i++) txds.push_back(argv[i]);
            LoadLevel(first, txds);
        }
    } else {
        // No CLI argument: try to auto-mount the last opened archive
        const std::string saved = LoadArcPref();
        if (!saved.empty()) {
            if (ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().Mount(saved)) {
                ScanAudioLibrary();
                std::cerr << "[arc] auto-mounted last arc: " << saved
                          << " (" << ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Entries().size() << " files)\n";
                state.showArc = true;   // open the archive browser automatically
            } else {
                std::cerr << "[arc] prefs arc no longer accessible: " << saved << "\n";
            }
        }
    }

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // don't write imgui.ini

    // ---- Theme ----
    //
    // Everything used to be the same grey, so nothing told you what was on,
    // what was interactive and what was just a label. One warm accent now
    // carries state -- checkmarks, slider grabs, the active tab, selection --
    // against a cool neutral chrome, which is enough hierarchy without turning
    // the tool into a paintbox.
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 7.0f;
    style.ChildRounding     = 5.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 5.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 5.0f;
    style.WindowPadding     = ImVec2(12, 11);
    style.FramePadding      = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(8, 7);
    style.ItemInnerSpacing  = ImVec2(7, 5);
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 11.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.SeparatorTextBorderSize = 2.0f;
    style.SeparatorTextPadding    = ImVec2(18, 4);

    // C++ blue.
    const ImVec4 accent      = ImVec4(0.00f, 0.41f, 0.71f, 1.00f);
    const ImVec4 accentHi    = ImVec4(0.16f, 0.58f, 0.89f, 1.00f);
    const ImVec4 accentDim   = ImVec4(0.04f, 0.22f, 0.39f, 1.00f);

    ImVec4* c = style.Colors;
    c[ImGuiCol_WindowBg]          = ImVec4(0.075f, 0.075f, 0.085f, 0.98f);
    c[ImGuiCol_ChildBg]           = ImVec4(0.055f, 0.055f, 0.065f, 0.92f);
    c[ImGuiCol_PopupBg]           = ImVec4(0.085f, 0.085f, 0.098f, 0.99f);
    c[ImGuiCol_Border]            = ImVec4(0.24f, 0.24f, 0.28f, 0.60f);
    c[ImGuiCol_FrameBg]           = ImVec4(0.135f, 0.135f, 0.155f, 1.00f);
    c[ImGuiCol_FrameBgHovered]    = ImVec4(0.195f, 0.195f, 0.225f, 1.00f);
    c[ImGuiCol_FrameBgActive]     = ImVec4(0.245f, 0.245f, 0.285f, 1.00f);
    c[ImGuiCol_TitleBg]           = ImVec4(0.065f, 0.065f, 0.075f, 1.00f);
    c[ImGuiCol_TitleBgActive]     = ImVec4(0.105f, 0.105f, 0.125f, 1.00f);
    c[ImGuiCol_MenuBarBg]         = ImVec4(0.095f, 0.095f, 0.110f, 1.00f);
    c[ImGuiCol_Header]            = ImVec4(accentDim.x, accentDim.y, accentDim.z, 0.55f);
    c[ImGuiCol_HeaderHovered]     = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_HeaderActive]      = ImVec4(accent.x, accent.y, accent.z, 0.62f);
    c[ImGuiCol_Button]            = ImVec4(0.165f, 0.165f, 0.195f, 1.00f);
    c[ImGuiCol_ButtonHovered]     = ImVec4(0.245f, 0.245f, 0.285f, 1.00f);
    c[ImGuiCol_ButtonActive]      = ImVec4(accent.x, accent.y, accent.z, 0.75f);
    c[ImGuiCol_SliderGrab]        = accent;
    c[ImGuiCol_SliderGrabActive]  = accentHi;
    c[ImGuiCol_CheckMark]         = accentHi;
    c[ImGuiCol_Tab]               = ImVec4(0.105f, 0.105f, 0.125f, 1.00f);
    c[ImGuiCol_TabHovered]        = ImVec4(accent.x, accent.y, accent.z, 0.50f);
    c[ImGuiCol_TabActive]         = ImVec4(0.135f, 0.165f, 0.205f, 1.00f);
    c[ImGuiCol_Separator]         = ImVec4(0.24f, 0.24f, 0.28f, 0.70f);
    c[ImGuiCol_SeparatorHovered]  = accent;
    c[ImGuiCol_ScrollbarBg]       = ImVec4(0.050f, 0.050f, 0.058f, 0.85f);
    c[ImGuiCol_ScrollbarGrab]     = ImVec4(0.215f, 0.215f, 0.250f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.290f, 0.290f, 0.330f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = accent;
    c[ImGuiCol_ResizeGrip]        = ImVec4(0.24f, 0.24f, 0.28f, 0.50f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.60f);
    c[ImGuiCol_ResizeGripActive]  = accent;
    c[ImGuiCol_Text]              = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
    c[ImGuiCol_TextDisabled]      = ImVec4(0.48f, 0.48f, 0.53f, 1.00f);
    c[ImGuiCol_TextSelectedBg]    = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_NavHighlight]      = accent;
    c[ImGuiCol_TableHeaderBg]     = ImVec4(0.125f, 0.125f, 0.145f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.24f, 0.24f, 0.28f, 0.80f);
    c[ImGuiCol_TableBorderLight]  = ImVec4(0.18f, 0.18f, 0.21f, 0.60f);
    c[ImGuiCol_TableRowBgAlt]     = ImVec4(1.00f, 1.00f, 1.00f, 0.020f);

    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

    const char* vS = R"(
#version 330 core
layout(location=0) in vec3 P;
layout(location=1) in vec2 T;
layout(location=2) in vec4 C;
layout(location=3) in vec4 W;
layout(location=4) in vec4 B;

out vec2  TC;
out vec4  VC;
out vec3  fragWorldPos;
uniform mat4  m;
uniform mat4  model;     // instance placement, identity for world geometry
uniform bool  flipU;
uniform bool  flipV;
uniform vec2  uvOffset;
uniform vec2  uvScale;
uniform bool  useSkinning;
uniform mat4  boneTransforms[128]; // Max 128 bones

void main(){
    vec4 localPos = vec4(P, 1.0);
    if(useSkinning) {
        float wSum = W.x + W.y + W.z + W.w;
        if(wSum > 0.001) {
            mat4 skinMat = boneTransforms[int(B.x)] * W.x +
                           boneTransforms[int(B.y)] * W.y +
                           boneTransforms[int(B.z)] * W.z +
                           boneTransforms[int(B.w)] * W.w;
            localPos = skinMat * localPos;
        }
    }

    gl_Position  = m * localPos;
    fragWorldPos = vec3(model * localPos);
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
uniform bool  untextured;
uniform bool  additive;
uniform bool  unlitGeometry;
uniform bool  alphaOff;
uniform vec4  matColor;
uniform float brightness;
uniform int   renderMode;
// 0=Textured 1=VertexColor 2=FlatShaded 3=Normals 4=Depth 5=Checker 6=Unlit
uniform vec3  eyePos;
uniform float depthMax;
uniform bool  iceEffect;

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
    } else if(untextured){
        // A material with no texture chunk: flat colour times vertex colour,
        // which is how the game shades it. Sampling the unbound sampler here
        // returned solid black instead.
        vec4 col = matColor;
        if(useVertexColors && !unlitGeometry) col *= VC;
        col.rgb *= brightness;
        if(col.a < 0.05) discard;
        FragColor = col;
    } else if(iceEffect){
        // Ice and water.
        //
        // The real look comes from the Wii's TEV stages, which the container
        // does not carry, so this is an approximation and not a decode: a
        // Fresnel rim plus a sharp specular highlight over the colour map,
        // with the surface normal taken from the screen-space derivatives the
        // flat-shading path already uses.
        vec4 tex = texture(t, TC);
        // A material whose blend op is NONE has no coverage channel: the
        // engine writes it with SRCBLEND ONE / DESTBLEND ZERO and never looks
        // at alpha. Character heads and bodies are all declared that way, and
        // their textures are barely opaque anywhere -- nurse_head has 1% opaque
        // texels and a third of it below this threshold -- so testing alpha on
        // them discarded the face and left the head bare.
        if(!alphaOff && tex.a < 0.02) discard;
        vec3 V = normalize(eyePos - fragWorldPos);
        vec3 L = normalize(vec3(0.45, 1.0, 0.35));
        vec3 Nf = faceforward(N, -V, N);
        float fres = pow(1.0 - clamp(dot(Nf, V), 0.0, 1.0), 3.0);
        float spec = pow(max(dot(reflect(-L, Nf), V), 0.0), 48.0);
        vec4 col = (useVertexColors && !unlitGeometry) ? tex * VC : tex;
        col.rgb  = mix(col.rgb, vec3(0.62, 0.78, 0.92), 0.35 * fres);
        col.rgb += vec3(0.55, 0.68, 0.80) * spec * 0.9;
        col.rgb += vec3(0.10, 0.16, 0.22) * fres;
        col.rgb *= brightness;
        FragColor = vec4(col.rgb, col.a);
    } else {
        // Textured (default, renderMode == 0)
        vec4 tex = texture(t, TC);
        // Discard only what is fully transparent. Cutting at 0.1 threw away the
        // whole soft edge of a gradient and left a hard jagged border where the
        // game fades out smoothly; the rest is handled by alpha blending.
        // A material whose blend op is NONE has no coverage channel: the
        // engine writes it with SRCBLEND ONE / DESTBLEND ZERO and never looks
        // at alpha. Character heads and bodies are all declared that way, and
        // their textures are barely opaque anywhere -- nurse_head has 1% opaque
        // texels and a third of it below this threshold -- so testing alpha on
        // them discarded the face and left the head bare.
        if(!alphaOff && tex.a < 0.02) discard;
        // Additive effect sheets carry their own brightness. Multiplying their
        // RGB by the baked vertex lighting drives them to black in a dark room,
        // which is why they only showed up with vertex colours switched off.
        //
        // Their vertex *alpha* is a different thing entirely: on the flame
        // sheets it runs the full 0..1 across the mesh, and it is the artist's
        // fade — the thing that stops a flame from ending in a hard polygon
        // edge. Dropping the whole vertex colour threw that away too, so the
        // fire came out as flat slabs with visible borders. Take the alpha and
        // leave the RGB alone: with SRC_ALPHA/ONE it scales the additive
        // contribution to nothing at the edges, without darkening the sheet.
        vec4 col;
        if(useVertexColors && !unlitGeometry)
            col = additive ? vec4(tex.rgb, tex.a * VC.a) : tex * VC;
        else
            col = tex;
        col.rgb *= brightness;
        if(alphaOff) col.a = 1.0;
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

    // Persistent line buffer for clump / game-object markers
    GLuint markerVao, markerVbo;
    glGenVertexArrays(1, &markerVao);
    glGenBuffers(1, &markerVbo);
    glBindVertexArray(markerVao);
    glBindBuffer(GL_ARRAY_BUFFER, markerVbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    // Batch export: `--export out.glb` writes the loaded scene and exits.
    for (int i = 1; i + 1 < argc; i++) {
        if (std::string(argv[i]) != "--export") continue;
        std::string err;
        GlbExportOptions eo;
        const bool ok = ExportGLB(argv[i + 1], eo, err);
        std::cerr << "[export] " << (ok ? "wrote " + std::string(argv[i + 1])
                                        : "failed: " + err) << "\n";
        ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
        return ok ? 0 : 1;
    }

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

    bool mouse_captured = false;
    Uint32 last_tick = SDL_GetTicks();

    bool run = true;
    while (run) {
        Uint32 now = SDL_GetTicks();
        float dt = (now - last_tick) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        last_tick = now;

        if (state.animSpeed > 0.0f) {
            for (auto& obj : ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects()) {
                if (auto clump = std::dynamic_pointer_cast<ClimaxEngine::SG::CClumpObject>(obj)) {
                    if (clump->animClip.duration > 0.0f) {
                        clump->animTime += dt * state.animSpeed;
                        if (clump->animTime > clump->animClip.duration) {
                            clump->animTime = fmod(clump->animTime, clump->animClip.duration);
                        }
                    }
                }
            }
        }
        if (state.animRestPose) {
            for (auto& obj : ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects()) {
                if (auto clump = std::dynamic_pointer_cast<ClimaxEngine::SG::CClumpObject>(obj)) {
                    clump->animTime = 0.0f; // Force rest pose evaluation (time 0 usually maps to bind pose? Or wait, if we want rest pose, we can just clear the clip. Actually, animClip.duration = 0 in SceneObject.cpp forces rest pose).
                    // We'll let UI.cpp or SceneObject handle rest pose explicitly.
                }
            }
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);

            if (e.type == SDL_QUIT) run = false;
            
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_m && state.useWASD) {
                    mouse_captured = !mouse_captured;
                    SDL_SetRelativeMouseMode(mouse_captured ? SDL_TRUE : SDL_FALSE);
                }
                // The action button. The trigger asks for MSG_PAD_GRAB, which
                // is the pad's grab/use input; E is its keyboard stand-in.
                if (e.key.keysym.sym == SDLK_e && state.playMode)
                    s_useDoorPressed = true;
            }

            // Mouse wheel zoom — proportional so zooming stays usable at any scale
            if (e.type == SDL_MOUSEWHEEL && viewportOwnsMouse) {
                state.camDist = glm::clamp(
                    state.camDist * powf(0.9f, (float)e.wheel.y), 0.5f, 2000.0f);
            }

            // Right mouse button drag → orbit (yaw / pitch)
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT && viewportOwnsMouse && !mouse_captured) {
                mouseRight = true;
                prevMouseX = e.button.x;
                prevMouseY = e.button.y;
            }
            if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) {
                mouseRight = false;
            }
            if (e.type == SDL_DROPFILE) {
                std::string path = e.drop.file;
                SDL_free(e.drop.file);

                // Update the file browser path to remember the directory
                g_FileBrowser.currentPath = fs::path(path).parent_path().string();

                // Anything the audio parser recognises is played; the sniff is
                // on the contents, so the extension only decides whether it is
                // worth reading the file at all.
                std::string ext;
                const size_t dot = path.find_last_of('.');
                if (dot != std::string::npos) {
                    ext = path.substr(dot);
                    for (auto& c : ext) c = (char)tolower((unsigned char)c);
                }
                static const char* kAudioExt[] = {
                    ".igc", ".igcstream", ".abc", ".ads", ".rws", ".vag", ".wav",
                };
                bool isAudio = false;
                for (const char* e2 : kAudioExt)
                    if (ext == e2) { isAudio = true; break; }

                if (isAudio) {
                    AudioClip clip;
                    if (Audio::LoadFile(path, clip)) {
                        PlayAudioClip(clip);
                    } else {
                        std::cerr << "[audio] cannot decode " << path << "\n";
                    }
                }
            }
            // Once a drag has started it keeps running even if the cursor leaves the
            // viewport, otherwise the orbit stutters whenever it crosses a panel.
            if (e.type == SDL_MOUSEMOTION && mouseRight && !mouse_captured) {
                float dx = (float)(e.motion.x - prevMouseX);
                float dy = (float)(e.motion.y - prevMouseY);
                state.camYaw   -= dx * 0.4f;
                state.camPitch  = glm::clamp(state.camPitch + dy * 0.4f, -89.0f, 89.0f);
                prevMouseX = e.motion.x;
                prevMouseY = e.motion.y;
            }
            if (e.type == SDL_MOUSEMOTION && mouse_captured && state.useWASD) {
                state.camYaw   -= e.motion.xrel * state.wasdSensitivity;
                state.camPitch  = glm::clamp(state.camPitch + e.motion.yrel * state.wasdSensitivity, -89.0f, 89.0f);
            }
        }

        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        if (state.useWASD) {
            float yRad = glm::radians(state.camYaw);
            float pRad = glm::radians(state.camPitch);
            glm::vec3 fwd(
                 -cosf(pRad) * sinf(yRad),
                 -sinf(pRad),
                 -cosf(pRad) * cosf(yRad)
            );
            glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0, 1, 0)));
            glm::vec3 flat_fwd = glm::normalize(glm::cross(glm::vec3(0, 1, 0), right));

            float speed = state.wasdSpeed * dt;
            if (keys[SDL_SCANCODE_LSHIFT]) speed *= 3.0f;

            if (state.playMode) {
                // Walk mode: the same keys drive a body through the collision
                // mesh instead of teleporting the camera. Speed is a rate here,
                // not a per-frame step, because the controller integrates.
                // Travis is fetched once, the first time a level is walked.
                // It costs a level reload, so it must not sit in the frame path.
                static bool triedPlayer = false;
                if (!triedPlayer && !g_CurrentMeshContainer.empty()) {
                    triedPlayer = true;
                    LoadPlayerModel("CPlayerBehaviour.Travis");
                }

                static ClimaxEngine::Game::CharacterController body;
                // Keyed by the container's name, not by &g_Collision: that is a
                // global whose address never changes, so the old test never
                // fired and neither the spawn nor the camera links were ever
                // rebuilt when a level was swapped.
                static std::string placedFor;
                static std::string arrivedFrom;   // zone the player came out of
                static std::vector<ClimaxEngine::Game::ZoneLink> zoneLinks;

                if (placedFor != g_CurrentMeshContainer) {
                    zoneLinks = ClimaxEngine::Game::BuildZoneLinks(g_GameObjects);
                    glm::vec3 spawn, facing;
                    if (ClimaxEngine::Game::FindZoneSpawn(g_GameObjects, arrivedFrom,
                                                          spawn, facing)) {
                        body.position = spawn;
                        // Face the way the spawner faces -- walking out of a
                        // door should not drop the player looking at it.
                        state.camYaw = glm::degrees(atan2f(-facing.x, -facing.z));
                    } else {
                        body.position = glm::vec3(state.camPosX, state.camPosY,
                                                  state.camPosZ);
                    }
                    body.velocity = glm::vec3(0.0f);
                    body.SnapToGround(g_Collision);
                    placedFor = g_CurrentMeshContainer;
                }

                glm::vec3 wish(0.0f);
                if (keys[SDL_SCANCODE_W]) wish += flat_fwd;
                if (keys[SDL_SCANCODE_S]) wish -= flat_fwd;
                if (keys[SDL_SCANCODE_A]) wish -= right;
                if (keys[SDL_SCANCODE_D]) wish += right;
                if (glm::length(wish) > 1e-4f) wish = glm::normalize(wish);

                float walk = state.walkSpeed;
                if (keys[SDL_SCANCODE_LSHIFT]) walk *= 2.0f;
                body.Step(g_Collision, wish * walk * dt, dt);

                // SnapToGround puts the sphere's centre one radius above the
                // floor, so the model's feet are that much below it.
                s_playerFeet = glm::vec3(body.position.x,
                                         body.position.y - body.radius,
                                         body.position.z);
                if (glm::length(wish) > 1e-3f) {
                    // Turn towards the way he is walking, by the short way
                    // round, so crossing due south does not spin him.
                    const float want = glm::degrees(atan2f(wish.x, wish.z));
                    float d = want - s_playerYaw;
                    while (d >  180.0f) d -= 360.0f;
                    while (d < -180.0f) d += 360.0f;
                    s_playerYaw += d * glm::min(1.0f, dt * 12.0f);
                }
                g_Player.Advance(dt);

                // Camera planes: crossing one hands the view to the camera
                // that side names. Rebuilt when the level changes, which is
                // what the collision-mesh pointer tracks.
                static ClimaxEngine::Game::CameraSwitcher switcher;
                static std::string switchesFor;
                if (switchesFor != g_CurrentMeshContainer) {
                    switcher.Reset(ClimaxEngine::Game::BuildCameraSwitches(
                        g_GameObjects, g_Cameras));
                    switchesFor = g_CurrentMeshContainer;
                    state.activeCamera = -1;
                }

                // ── Doorways ────────────────────────────────────────────────
                // Standing in a ZoneTrigger box and pressing the action key
                // loads the container it names and puts Travis on that level's
                // spawner for the zone he just left. MSG_PAD_GRAB is the button
                // the trigger asks for; E stands in for it here.
                s_zoneLinkHere = ClimaxEngine::Game::ZoneLinkAt(zoneLinks,
                                                                body.position);
                if (s_zoneLinkHere >= 0) {
                    s_zonePrompt = zoneLinks[(size_t)s_zoneLinkHere].toZone;
                    if (s_useDoorPressed) {
                        const auto &link = zoneLinks[(size_t)s_zoneLinkHere];
                        auto *arc = ClimaxEngine::RWS::FileSystem::
                            CArchiveManager::GetInstance().GetFirstArchive();
                        const int idx = arc ? arc->Find(link.toZone) : -1;
                        if (idx >= 0) {
                            arrivedFrom = link.fromZone;
                            std::cout << "[zone] " << link.fromZone << " -> "
                                      << link.toZone << "  (" << link.eventName
                                      << ")\n";
                            LoadLevelFromArc(idx);
                        } else {
                            std::cerr << "[zone] no container named "
                                      << link.toZone << " in the archive\n";
                        }
                    }
                } else {
                    s_zonePrompt.clear();
                }
                s_useDoorPressed = false;
                if (state.autoCameras) {
                    const int cut = switcher.Update(body.position);
                    if (cut >= 0 && cut < (int)g_Cameras.size())
                        state.activeCamera = cut;

                    // Which of a plane's two names belongs to which side was a
                    // guess, and a wrong guess puts the view behind a wall.
                    // Rather than guess again, keep the choice honest: if the
                    // active camera cannot see the player, take the nearest one
                    // that can. Cameras sit inside walls looking in, so the wall
                    // between them and the player is exactly what this finds.
                    const glm::vec3 head(body.position.x,
                                         body.position.y + state.eyeHeight * 0.6f,
                                         body.position.z);
                    // A camera is usable when the player is actually in its
                    // shot: inside the cone its own field of view cuts, and not
                    // behind a wall. Both halves come from the level rather than
                    // from tuning -- the cone is the camera's aim and FOV, the
                    // wall is the collision mesh.
                    auto usable = [&](int idx) {
                        if (idx < 0 || idx >= (int)g_Cameras.size()) return false;
                        const LevelCamera &c = g_Cameras[(size_t)idx];
                        const glm::vec3 v = head - c.position;
                        const float dist = glm::length(v);
                        if (dist < 1e-3f) return false;
                        // Slightly wider than the frustum so the player is not
                        // dropped the instant he touches the frame edge.
                        const float halfFov =
                            glm::radians(glm::min(c.fovDeg * 0.6f, 85.0f));
                        if (glm::dot(v / dist,
                                     ClimaxEngine::Game::CameraAim(
                                         c, c.position, head)) < cosf(halfFov))
                            return false;
                        return ClimaxEngine::Game::HasLineOfSight(g_Collision,
                                                                  c.position, head);
                    };

                    if (!usable(state.activeCamera)) {
                        // Nearest camera that still has the player in shot.
                        // Falling back to any that merely sees him keeps a view
                        // on screen while walking through a gap no camera
                        // covers, instead of freezing on a wall.
                        int best = -1, anySight = -1;
                        float bestDist = 1e9f, anyDist = 1e9f;
                        for (size_t k = 0; k < g_Cameras.size(); ++k) {
                            const glm::vec3 &cp = g_Cameras[k].position;
                            if (!ClimaxEngine::Game::HasLineOfSight(g_Collision, cp, head))
                                continue;
                            const float dd = glm::length(cp - head);
                            if (dd < anyDist) { anyDist = dd; anySight = (int)k; }
                            if (usable((int)k) && dd < bestDist) {
                                bestDist = dd; best = (int)k;
                            }
                        }
                        if (best >= 0) state.activeCamera = best;
                        else if (anySight >= 0) state.activeCamera = anySight;
                    }
                }

                if (state.autoCameras && state.activeCamera >= 0 &&
                    state.activeCamera < (int)g_Cameras.size()) {
                    // A fixed camera does not follow the player: it sits where
                    // the designer put it and looks where they aimed it, which
                    // is the whole point of the framing in this game.
                    const LevelCamera& lc = g_Cameras[(size_t)state.activeCamera];
                    state.camFovDeg = lc.fovDeg;

                    // The aim of a static camera is authored, and it is in the
                    // placement matrix after all: row 2 is the look direction,
                    // RenderWare's "at".
                    //
                    // An earlier reading called those matrices orientation-free
                    // because all four cameras in HO_1_Hallway1 share one
                    // rotation. Measured over the whole archive that is a
                    // coincidence of one small level: 989 cameras carry 358
                    // distinct rotations, 175 of 229 levels hold more than one,
                    // and 307 are pitched -- up to 83 degrees down a stairwell.
                    // Against every walkable marker in the level, +row2 aims
                    // nearer the playable space than -row2 on 654 cameras to
                    // 314, mean cosine 0.97, which settles the sign too.
                    //
                    // Which cameras honour that, which track the player, and how
                    // the eye gets out from behind the wall it was placed in is
                    // the game layer's business, not the viewer's.
                    const glm::vec3 subject(body.position.x,
                                            body.position.y + state.eyeHeight * 0.6f,
                                            body.position.z);
                    glm::vec3 eye, look;
                    ClimaxEngine::Game::ResolveCameraView(g_Collision, lc, subject,
                                                          eye, look);
                    state.camPosX = eye.x;
                    state.camPosY = eye.y;
                    state.camPosZ = eye.z;

                    // The subject, kept current so the near-plane clip below
                    // measures against the player and not a stale orbit pivot.
                    state.camTargetX = subject.x;
                    state.camTargetY = subject.y;
                    state.camTargetZ = subject.z;
                    const glm::vec3 back = -look;
                    state.camPitch = glm::degrees(asinf(glm::clamp(back.y, -1.0f, 1.0f)));
                    state.camYaw   = glm::degrees(atan2f(back.x, back.z));
                } else {
                    state.camPosX = body.position.x;
                    state.camPosY = body.position.y + state.eyeHeight;
                    state.camPosZ = body.position.z;
                }
            } else {
                if (keys[SDL_SCANCODE_W]) { state.camPosX += flat_fwd.x * speed; state.camPosZ += flat_fwd.z * speed; }
                if (keys[SDL_SCANCODE_S]) { state.camPosX -= flat_fwd.x * speed; state.camPosZ -= flat_fwd.z * speed; }
                if (keys[SDL_SCANCODE_A]) { state.camPosX -= right.x * speed; state.camPosZ -= right.z * speed; }
                if (keys[SDL_SCANCODE_D]) { state.camPosX += right.x * speed; state.camPosZ += right.z * speed; }
                if (keys[SDL_SCANCODE_Q]) { state.camPosY -= speed; }
                if (keys[SDL_SCANCODE_E]) { state.camPosY += speed; }
            }
        } else if (mouse_captured) {
            mouse_captured = false;
            SDL_SetRelativeMouseMode(SDL_FALSE);
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
        // Fixed cameras are placed behind the walls of the room they film, so
        // the view used to open on that wall. This was answered by pushing the
        // near plane out in proportion to the distance to the subject, which
        // cleared the wall but also sliced the front off the room and let the
        // frame show the emptiness past the floor.
        //
        // Game::ResolveCameraView now steps the eye through the wall instead,
        // so it renders from inside the room and the near plane can go back to
        // being a near plane. A small proportional clip is kept for the case it
        // declines to move -- when the wall sits so close to the player that
        // stepping through would land the camera on top of him.
        float nearPlane = 0.1f;
        if (state.playMode && state.autoCameras && state.activeCamera >= 0 &&
            state.activeCamera < (int)g_Cameras.size()) {
            const glm::vec3 d = eye - glm::vec3(state.camTargetX, state.camTargetY,
                                                state.camTargetZ);
            nearPlane = glm::clamp(glm::length(d) * 0.05f, 0.1f, 0.6f);
        }
        glm::mat4 proj = glm::perspective(glm::radians(state.camFovDeg), aspect, nearPlane, 2000.0f);
        glm::mat4 mvp  = proj * view;

        // --- Render 3-D scene ---
        // Sync audio state
        state.isAudioPlaying = ClimaxEngine::Audio::CAudioRelay::GetInstance().IsAudioPlaying();
        state.audioProgress = ClimaxEngine::Audio::CAudioRelay::GetInstance().GetAudioProgress();
        ClimaxEngine::Audio::CAudioRelay::GetInstance().SetVolume(state.audioVolume);
        ClimaxEngine::Audio::CAudioRelay::GetInstance().SetLoop(state.audioLoop);

        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        iam_update_begin_frame();
        iam_clip_update(io.DeltaTime);

        // Key 1 → reset camera, F1 → hide/show the whole interface
        if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_1, false)) {
            state.camTargetX = 0; state.camTargetY = 2; state.camTargetZ = 0;
            state.camYaw = 0; state.camPitch = 20; state.camDist = 15; state.camFovDeg = 60.0f;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) state.showUI = !state.showUI;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) state.showManual = !state.showManual;
        if (ImGui::IsKeyPressed(ImGuiKey_G,  false) && !io.WantCaptureKeyboard)
            state.showPivotGizmo = !state.showPivotGizmo;

        size_t totalMeshes = 0;
        for (auto& obj : ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects()) {
            totalMeshes += obj->GetMeshes().size();
        }
        const bool haveModel = totalMeshes > 0;

        if (state.renderDevice == RenderDevice::CPU) {
            // --- CPU Software Rasterization Pass ---
            g_CPURasterizer.Init(fbW, fbH);
            g_CPURasterizer.Clear(state.skyColorBot[0], state.skyColorBot[1], state.skyColorBot[2], 1.0f);
            if (haveModel) {
                g_CPURasterizer.RenderScene(ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects(), mvp, eye);
            }
            g_CPURasterizer.PresentOnScreen();
        } else {
            // --- GPU Hardware Acceleration Pass (OpenGL 3.3 / Metal) ---
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

            const GLint uM     = glGetUniformLocation(p, "m");
            const GLint uModel  = glGetUniformLocation(p, "model");

            // Advance animation time for all objects playing a clip
            static size_t s_lastLoadChunkCount = 0;
            static bool s_debugPrinted = false;
            if (totalMeshes != s_lastLoadChunkCount) {
                s_lastLoadChunkCount = totalMeshes;
                s_debugPrinted = false;
            }
            if (!s_debugPrinted && totalMeshes > 0) {
                std::cerr << "[render] first frame: meshes=" << totalMeshes
                          << " sections=" << g_ShoSections.size()
                          << " gameObjects=" << g_GameObjects.size() << "\n";
                std::cerr.flush();
            }
            float dt = ImGui::GetIO().DeltaTime;
            if (state.uvAnimRun) state.uvAnimTime += dt * state.uvAnimSpeed;
            // Every clump shares the transport in the Playback panel.
            for (auto& o : ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects())
                if (auto cl = std::dynamic_pointer_cast<ClimaxEngine::SG::CClumpObject>(o))
                    cl->animTime += dt * state.animSpeed;
            for (auto& go : g_GameObjects) {
                if (go.currentClipIndex >= 0 && go.currentClipIndex < (int)go.clipSectionIndices.size()) {
                    go.animTime += dt;
                } // No auto-start: user picks clip from UI
            }
            if (!s_debugPrinted && totalMeshes > 0) {
                std::cerr << "[render] after anim loop OK\n";
                std::cerr.flush();
            }
        const GLint uUntex  = glGetUniformLocation(p, "untextured");
        const GLint uAdd    = glGetUniformLocation(p, "additive");
        const GLint uUnlit  = glGetUniformLocation(p, "unlitGeometry");
        const GLint uAlphaOff = glGetUniformLocation(p, "alphaOff");
        const GLint uUvOff  = glGetUniformLocation(p, "uvOffset");
        const GLint uUvScl  = glGetUniformLocation(p, "uvScale");
        const GLint uMatCol = glGetUniformLocation(p, "matColor");
        const GLint uIce    = glGetUniformLocation(p, "iceEffect");
            const glm::mat4 identity(1.0f);
            // Two passes: opaque first with depth writes on, blended second
            // with them off. A blended surface that writes depth hides whatever
            // stands behind it, which is why one semi-transparent sheet made the
            // next one disappear.
            
            ClimaxEngine::SG::RenderContext ctx;
            ctx.viewProj = mvp;
            ctx.eye = eye;
            ctx.uM = uM;
            ctx.uModel = uModel;
            ctx.uUntex = uUntex;
            ctx.uAdd = uAdd;
            ctx.uUnlit = uUnlit;
            ctx.uMatCol = uMatCol;
            ctx.uIce = uIce;
            ctx.uUseSkinning = glGetUniformLocation(p, "useSkinning");
            ctx.textureMap = &g_TextureMap;
            
            for (int pass = 0; pass < 2; pass++) {
                ctx.pass = pass;
                
                auto& registrar = ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance();
                for (auto& obj : registrar.GetObjects()) {
                    auto meshes = obj->GetMeshes();
                    for (auto* chunkPtr : meshes) {
                        const auto& chunk = *chunkPtr;
                        
                        if (chunk.alphaPass) continue;
                        
                        // Effect sheets must never occlude each other.
                        //
                        // Which pass a mesh belongs in is decided by the blend
                        // mode the material itself declares, not by how its
                        // texture happens to be named. An additive or
                        // subtractive sheet is authored so that black drops
                        // out, so it carries no transparent texels at all --
                        // FX_fire_Dahlia is 99% opaque. Both the FX_ prefix and
                        // the measured alpha gradient therefore miss it, and it
                        // lands in the opaque pass and draws as a solid
                        // rectangle. Blood_Pool_SUB and 42 other textures fail
                        // the name test the same way.
                        //
                        // The name and gradient tests stay as a fallback for
                        // materials that declare mode 0 but still need sorting.
                        auto itG0 = g_TexGradient.find(chunk.texName);
                        const bool isFx = chunk.texName.size() > 3 &&
                            sho_strnicmp(chunk.texName.c_str(), "FX_", 3) == 0;
                        // The engine masks this to 16 bits too -- the world
                        // pipe setup does `andi $s3, $v0, 0xffff` right after
                        // reading the field.
                        const uint32_t declared = chunk.blendMode & 0xFFFF;
                        // Mode 3 is the engine's NONE op: SRCBLEND ONE,
                        // DESTBLEND ZERO. The surface is written straight out
                        // and its alpha is not a coverage channel at all, so it
                        // belongs in the opaque pass whatever its texture looks
                        // like.
                        const bool alphaOff = (declared == 3);
                        const bool blended = !alphaOff &&
                            (declared == 1 || declared == 2 ||
                             isFx || chunk.additive ||
                             (itG0 != g_TexGradient.end() && itG0->second));
                        if ((pass == 0) == blended) continue;
                        
                        const std::string& tName =
                            (state.frozenVariant && !chunk.altTexName.empty())
                                ? chunk.altTexName : chunk.texName;
                        GLuint tid = 0;
                        if (g_TextureMap.count(tName)) tid = g_TextureMap[tName];
                        if (!tid) {
                            std::string alt = tName;
                            for (auto& ch : alt) ch = (char)toupper((unsigned char)ch);
                            if (g_TextureMap.count(alt)) tid = g_TextureMap[alt];
                        }
                        if (!tid) {
                            std::string alt = tName;
                            for (auto& ch : alt) ch = (char)tolower((unsigned char)ch);
                            if (g_TextureMap.count(alt)) tid = g_TextureMap[alt];
                        }
                        
                        if (chunk.untextured) continue;
                        
                        // Blend mode comes from the material itself -- the
                        // 0x0A01 extension field the engine reads through
                        // ClimaxT1MaterialGetFrameBlendMode. 0 is standard
                        // alpha, 1 additive, 2 subtractive.
                        //
                        // This replaces a hand-maintained list of FX_ name
                        // prefixes. The list could never be right: FX_TV and
                        // FX_save_point1 are standard alpha while FX_Flare_01
                        // is additive, and nothing in their names says so. The
                        // field also carries a third mode the list had no way
                        // to express -- subtractive, used by the blood decals,
                        // two of which spell SUB in the texture name.
                        const uint32_t blend = chunk.blendMode & 0xFFFF;
                        const bool addNow = (blend == 1);
                        const bool subNow = (blend == 2);

                        glUniform1i(uAdd, addNow ? 1 : 0);
                        glUniform1i(uUnlit, chunk.unlitGeometry ? 1 : 0);
                        glUniform1i(uIce, (chunk.iceEffect && state.iceShading) ? 1 : 0);

                        if (addNow) {
                            // ClimaxT1AtomicSetAlphaOpADD sets SRCBLEND to
                            // rwBLENDSRCALPHA (5) and DESTBLEND to rwBLENDONE
                            // (2) -- the source is scaled by its own alpha
                            // before being added. GL_ONE, GL_ONE adds the full
                            // colour instead, which is what made the headlight
                            // beams read as flat white.
                            glBlendEquation(GL_FUNC_ADD);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                            glDepthMask(GL_FALSE);
                        } else if (subNow) {
                            // SUB sets the same two blend factors as ADD and
                            // changes the equation instead.
                            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                            glDepthMask(GL_FALSE);
                        } else if (alphaOff) {
                            glBlendEquation(GL_FUNC_ADD);
                            glBlendFunc(GL_ONE, GL_ZERO);
                            glDepthMask(GL_TRUE);
                        } else {
                            glBlendEquation(GL_FUNC_ADD);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            glDepthMask(pass == 0 ? GL_TRUE : GL_FALSE);
                        }
                        glUniform1i(uAlphaOff, alphaOff ? 1 : 0);

                        // UV animation. The clip's offsets run to whole texture
                        // units over its duration (-28.0 after 28 s), so with
                        // WRAP addressing the end of the loop samples exactly
                        // what the start does and the cycle is seamless.
                        {
                            glm::vec4 uv(state.uvScaleX, state.uvScaleY,
                                         state.uvOffsetX, state.uvOffsetY);
                            if (!chunk.uvAnimName.empty()) {
                                auto itA = g_UVAnims.find(chunk.uvAnimName);
                                if (itA != g_UVAnims.end())
                                    uv = EvalUVAnim(itA->second, 0, state.uvAnimTime);
                            }
                            glUniform2f(uUvScl, uv.x, uv.y);
                            glUniform2f(uUvOff, uv.z, uv.w);
                        }

                        glBindTexture(GL_TEXTURE_2D, tid);
                        glUniform1i(uUntex, (chunk.untextured || tid == 0) ? 1 : 0);
                        glUniform4fv(uMatCol, 1, glm::value_ptr(chunk.matColor));
                        
                        // Delegate drawing to the object which handles transformations
                        obj->SetMatrixAndDraw(ctx, chunkPtr);
                    }
                }
            } // opaque pass, then blended pass

            // ── Travis ──────────────────────────────────────────────────────
            // Drawn after the level and outside the two passes: he is opaque,
            // and his textures are not in g_TextureMap -- the model owns them
            // so that loading the next room cannot delete them.
            //
            // Drawn through the objects' own SetMatrixAndDraw, so the skeleton,
            // the clip and the skinning all run on the existing path. Placing
            // him is just setting the transform they already multiply by.
            if (state.playMode && state.autoCameras && g_Player.loaded) {
                glm::mat4 model = glm::translate(glm::mat4(1.0f), s_playerFeet);
                model = glm::rotate(model, glm::radians(s_playerYaw),
                                    glm::vec3(0, 1, 0));

                glUniform1i(uAlphaOff, 0);
                glUniform1i(uAdd, 0);
                glUniform1i(uIce, 0);
                glUniform2f(uUvScl, 1.0f, 1.0f);
                glUniform2f(uUvOff, 0.0f, 0.0f);
                glBlendEquation(GL_FUNC_ADD);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_TRUE);

                for (auto& obj : g_Player.objects) {
                    obj->SetTransform(model);
                    for (auto* chunkPtr : obj->GetMeshes()) {
                        const MeshChunk& m = *chunkPtr;
                        // The container's particle blanks are authored at full
                        // size and scaled when the game spawns them; drawn as
                        // they are, they carpet the floor.
                        if (IsPlayerEffectMesh(m)) continue;

                        auto itT = g_Player.textures.find(m.texName);
                        const GLuint tid = itT == g_Player.textures.end() ? 0 : itT->second;
                        glBindTexture(GL_TEXTURE_2D, tid);
                        glUniform1i(uUntex, (m.untextured || tid == 0) ? 1 : 0);
                        glUniform1i(uUnlit, m.unlitGeometry ? 1 : 0);
                        glUniform4fv(uMatCol, 1, glm::value_ptr(m.matColor));
                        obj->SetMatrixAndDraw(ctx, chunkPtr);
                    }
                }
            }

            glUniformMatrix4fv(uM, 1, GL_FALSE, glm::value_ptr(mvp));
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_TRUE);
            glBindVertexArray(0);
            if (!s_debugPrinted) {
                std::cerr << "[render] chunk loop finished OK\n";
                std::cerr.flush();
                s_debugPrinted = true;
            }
        }

        // --- Collision render pass (solid fill + wireframe) ---
        if (state.showCollision && GpuPeek(g_Collision) && !g_Collision.indices.empty()) {
            glUseProgram(collProg);
            glUniformMatrix4fv(glGetUniformLocation(collProg, "m"), 1, GL_FALSE, glm::value_ptr(mvp));
            glDisable(GL_CULL_FACE);
            glBindVertexArray(GpuFor(g_Collision).vao);

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

        // --- Object markers: CLUMPs and placed 0x0704 game objects ---
        // Both batches share one persistent VBO; this used to allocate and then
        // destroy a VAO + VBO on every single frame.
        {
            auto* fdl = ImGui::GetForegroundDrawList();

            auto drawBatch = [&](const std::vector<glm::vec3>& verts, float r, float g, float b) {
                if (verts.empty()) return;
                glBindVertexArray(markerVao);
                glBindBuffer(GL_ARRAY_BUFFER, markerVbo);
                glBufferData(GL_ARRAY_BUFFER,
                             (GLsizeiptr)(verts.size() * sizeof(glm::vec3)),
                             verts.data(), GL_DYNAMIC_DRAW);
                glUseProgram(collProg);
                glUniformMatrix4fv(glGetUniformLocation(collProg, "m"), 1, GL_FALSE, glm::value_ptr(mvp));
                glUniform4f(glGetUniformLocation(collProg, "solidColor"), r, g, b, 1.0f);
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                glLineWidth(2.0f);
                glDrawArrays(GL_LINES, 0, (GLsizei)verts.size());
                glLineWidth(1.0f);
                glBindVertexArray(0);
            };

            std::vector<glm::vec3> lineVerts;

            if (state.showClumps && !g_Clumps.empty()) {
                lineVerts.clear();
                lineVerts.reserve(g_Clumps.size() * 30);
                for (const auto& cl : g_Clumps)
                    AppendMarker(lineVerts, cl.position, cl.transform, 0.35f, 0.5f);
                drawBatch(lineVerts, 1.0f, 0.72f, 0.10f);

                if (state.showObjectLabels)
                    for (const auto& cl : g_Clumps)
                        DrawWorldLabel(fdl, mvp, cl.position, winW, winH,
                                       cl.label.c_str(), IM_COL32(255, 192, 40, 255));
            }

            if (state.showGameObjects && !g_GameObjects.empty()) {
                lineVerts.clear();
                lineVerts.reserve(g_GameObjects.size() * 30);
                for (const auto& go : g_GameObjects) {
                    // Non-spatial objects (CZone, GameMessage, …) carry identity;
                    // drawing them all stacked on the origin is just noise.
                    if (go.atOrigin && !state.showOriginObjects) continue;
                    AppendMarker(lineVerts, go.position, go.transform, 0.22f, 0.45f);
                }
                drawBatch(lineVerts, 0.35f, 0.85f, 1.0f);

                if (state.showObjectLabels) {
                    for (const auto& go : g_GameObjects) {
                        if (go.atOrigin && !state.showOriginObjects) continue;
                        DrawWorldLabel(fdl, mvp, go.position, winW, winH,
                                       go.label.c_str(), IM_COL32(120, 216, 255, 255));
                    }
                }
            }

            if (state.showBoneOverlay) {
                lineVerts.clear();
                for (const auto& obj : ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects()) {
                    if (auto clump = std::dynamic_pointer_cast<ClimaxEngine::SG::CClumpObject>(obj)) {
                        if (clump->skeleton.bones.empty() || clump->currentBoneMats.empty()) continue;
                        const glm::mat4& clumpMat = clump->GetTransform();
                        for (size_t b = 0; b < clump->skeleton.bones.size(); ++b) {
                            glm::mat4 worldMat = clumpMat * clump->currentBoneMats[b];
                            glm::vec3 pos = glm::vec3(worldMat[3]);
                            AppendMarker(lineVerts, pos, worldMat, 0.02f, 0.05f);
                            
                            int parent = clump->skeleton.bones[b].parent;
                            if (parent >= 0 && parent < (int)b) {
                                glm::vec3 parentPos = glm::vec3(clumpMat * clump->currentBoneMats[parent][3]);
                                lineVerts.push_back(parentPos);
                                lineVerts.push_back(pos);
                            }
                        }
                    }
                }
                if (!lineVerts.empty()) {
                    drawBatch(lineVerts, 1.0f, 0.0f, 1.0f); // Magenta for bones
                }
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
        // No level, no gizmo: the arrows used to float in an empty viewport with
        // nothing to aim at.
        if (state.showPivotGizmo && state.showUI && haveModel) {
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
        if (state.showUI) {
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
                    // Dragging the ball turns the ball, so the camera has to move
                    // the opposite way from a free right-drag orbit. Both axes used
                    // to be inherited from the orbit code and came out reversed.
                    state.camYaw   -= io.MouseDelta.x * 0.32f;
                    state.camPitch  = glm::clamp(
                        state.camPitch + io.MouseDelta.y * 0.32f, -89.0f, 89.0f);
                } else {
                    sphereDragging = false;
                }
            }
        }

        // -- Main control panel (pinned top-left, fixed 256 px, scrollable) --
        if (state.showUI) {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(256, (float)winH - 20.0f), ImGuiCond_Always);
        // The tool covers both games, so the panel is not "SHO" anything.
        ImGui::Begin("Climax Toolkit", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

        if (ImGui::Button("Open SH.ARC", ImVec2(-1, 0))) g_FileBrowser.Open(FileBrowserMode::Arc);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Load the game archive and browse levels by their real names");
        if (ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()) {
            ImGui::TextDisabled("%s", fs::path(ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Path()).filename().string().c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu files)", ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Entries().size());
        }
        if (ImGui::Button("Open Loose File", ImVec2(-1, 0))) g_FileBrowser.Open(FileBrowserMode::Mesh);

        // What is loaded, stated once and clearly. This used to be two dim
        // lines that read like a caption; it is the single most useful thing
        // on the panel, so it gets a framed block of its own.
        if (!g_CurrentMeshContainer.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.085f, 0.115f, 0.150f, 0.85f));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0f);
            ImGui::BeginChild("##loaded", ImVec2(-1, ImGui::GetTextLineHeightWithSpacing() * 2.5f),
                              true, ImGuiWindowFlags_NoScrollbar);
            ImGui::TextColored(ImVec4(0.35f, 0.68f, 0.95f, 1.0f), "%s",
                fs::path(g_CurrentMeshContainer).filename().string().c_str());
            ImGui::TextDisabled("%zu meshes   %zu textures", totalMeshes,
                                g_TextureMap.size() / 2);
            if (!g_AnimClips.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("  %zu clips", g_AnimClips.size());
            }
            if (!g_UVAnims.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("  %zu uv", g_UVAnims.size());
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();

        // ---- Camera --------------------------------------------------
        ImGui::SeparatorText("Camera");
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("##dist", &state.camDist, 1.0f, 200.0f, "Dist %.1f");

        if (ImGui::Checkbox("Free Flight (WASD, M to toggle mouse)", &state.useWASD)) {
            if (state.useWASD) {
                state.showPivotGizmo = false;
                // Sync position
                float yRad = glm::radians(state.camYaw);
                float pRad = glm::radians(glm::clamp(state.camPitch, -89.0f, 89.0f));
                float dist = std::max(state.camDist, 0.1f);
                state.camPosX = state.camTargetX + cosf(pRad) * sinf(yRad) * dist;
                state.camPosY = state.camTargetY + sinf(pRad) * dist;
                state.camPosZ = state.camTargetZ + cosf(pRad) * cosf(yRad) * dist;
            } else {
                // Sync orbit target
                float yRad = glm::radians(state.camYaw);
                float pRad = glm::radians(glm::clamp(state.camPitch, -89.0f, 89.0f));
                state.camTargetX = state.camPosX - cosf(pRad) * sinf(yRad) * state.camDist;
                state.camTargetY = state.camPosY - sinf(pRad) * state.camDist;
                state.camTargetZ = state.camPosZ - cosf(pRad) * cosf(yRad) * state.camDist;
                
                if (mouse_captured) {
                    mouse_captured = false;
                    SDL_SetRelativeMouseMode(SDL_FALSE);
                }
            }
        }
        if (state.useWASD) {
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##wasd_speed", &state.wasdSpeed, 1.0f, 100.0f, "Speed %.1f");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##wasd_sens", &state.wasdSensitivity, 0.01f, 1.0f, "Sens %.2f");
        }

        {
            // Walk mode only means anything once the level has collision to
            // stand on, so it is disabled rather than silently doing nothing.
            const bool canWalk = !g_Collision.indices.empty();
            ImGui::BeginDisabled(!canWalk);
            if (ImGui::Checkbox("Walk (collision)", &state.playMode) && state.playMode)
                state.useWASD = true;   // walk mode drives the same camera
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered() && state.uiTooltips)
                ImGui::SetTooltip(canWalk
                    ? "Move as a body through the level's collision mesh.\n"
                      "Starts at the level's CPlayerSpawner when it has one."
                    : "This level has no collision mesh loaded.");
            if (state.playMode) {
                ImGui::SetNextItemWidth(-70.0f);
                ImGui::SliderFloat("##walkspeed", &state.walkSpeed, 1.0f, 8.0f, "%.1f");
                ImGui::SameLine(); ImGui::TextDisabled("Speed");
                ImGui::SetNextItemWidth(-70.0f);
                ImGui::SliderFloat("##eye", &state.eyeHeight, 0.8f, 2.2f, "%.2f");
                ImGui::SameLine(); ImGui::TextDisabled("Eye");
                ImGui::Checkbox("Fixed cameras", &state.autoCameras);
                if (ImGui::IsItemHovered() && state.uiTooltips)
                    ImGui::SetTooltip("Hand the view to the level's own cameras when\n"
                                      "a PlaneTrigger is crossed, as the game does.\n"
                                      "Off keeps a first-person view on the body.");
                if (state.autoCameras && state.activeCamera >= 0 &&
                    state.activeCamera < (int)g_Cameras.size())
                    ImGui::TextDisabled("camera: %s",
                                        g_Cameras[(size_t)state.activeCamera].name.c_str());
            }
        }

        if (ImGui::Button("Reset Camera", ImVec2(-1, 0))) {
            state.camTargetX = 0; state.camTargetY = 2; state.camTargetZ = 0;
            state.camPosX = 0; state.camPosY = 2; state.camPosZ = 15;
            state.camYaw = 0; state.camPitch = 20; state.camDist = 15; state.camFovDeg = 60.0f;
        }

        // ---- Level cameras ------------------------------------------
        if (!g_Cameras.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Level cameras (%zu)", g_Cameras.size());
            {
                // Level logic, read straight out of the object graph: which
                // plane hands over to which camera.
                static std::vector<ClimaxEngine::Game::CameraSwitch> switches;
                static size_t builtFor = (size_t)-1;
                if (builtFor != g_GameObjects.size()) {
                    switches = ClimaxEngine::Game::BuildCameraSwitches(g_GameObjects, g_Cameras);
                    builtFor = g_GameObjects.size();
                }
                if (!switches.empty()) {
                    int resolved = 0;
                    for (const auto& sw : switches)
                        if (sw.cameraA >= 0 && sw.cameraB >= 0) resolved++;
                    ImGui::TextDisabled("%zu camera switches, %d resolved",
                                        switches.size(), resolved);
                    if (ImGui::IsItemHovered() && state.uiTooltips) {
                        ImGui::BeginTooltip();
                        for (size_t k = 0; k < switches.size() && k < 12; k++)
                            ImGui::Text("%s  ->  %s", switches[k].nameA.c_str(),
                                        switches[k].nameB.c_str());
                        ImGui::EndTooltip();
                    }
                }
            }
            if (state.camFovDeg != 60.0f) {
                ImGui::SameLine();
                ImGui::TextDisabled("FOV %.0f°", state.camFovDeg);
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##camsel", "Jump to camera...")) {
                for (size_t i = 0; i < g_Cameras.size(); i++) {
                    const LevelCamera& c = g_Cameras[i];
                    ImGui::PushID((int)i);
                    if (ImGui::Selectable(c.name.c_str())) {
                        // Orbit around a point in front of the camera so the
                        // existing orbit controls keep working from there.
                        const float d = 3.0f;
                        glm::vec3 target = c.position + c.forward * d;
                        state.camTargetX = target.x;
                        state.camTargetY = target.y;
                        state.camTargetZ = target.z;
                        state.camDist    = d;
                        glm::vec3 back   = -c.forward;
                        state.camPitch   = glm::degrees(asinf(glm::clamp(back.y, -1.0f, 1.0f)));
                        state.camYaw     = glm::degrees(atan2f(back.x, back.z));
                        state.camFovDeg  = c.fovDeg;
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("(%.2f, %.2f, %.2f)   FOV %.0f°",
                                          c.position.x, c.position.y, c.position.z,
                                          c.fovDeg);
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::Spacing();
        }

        if (ImGui::Checkbox("Pivot gizmo", &state.showPivotGizmo)) {
            if (state.showPivotGizmo) {
                state.useWASD = false;
            }
        }
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
        ImGui::TextDisabled("G gizmo  F1 hide UI  F2 manual");
        // ---- Render Device (GPU / CPU) -----------------------------------
        ImGui::TextDisabled("Render Device / Engine");
        if (ImGui::RadioButton("GPU (Hardware)", state.renderDevice == RenderDevice::GPU)) {
            state.renderDevice = RenderDevice::GPU;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hardware Acceleration (OpenGL 3.3 Core / Apple Metal shaders)");
        ImGui::SameLine();
        if (ImGui::RadioButton("CPU (Software)", state.renderDevice == RenderDevice::CPU)) {
            state.renderDevice = RenderDevice::CPU;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Software Rasterizer: renders geometry & shading on CPU thread");

        ImGui::Spacing();

        // ---- Render mode ------------------------------------------------
        ImGui::SeparatorText("Render Mode");
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
        // The selected mode used to pop on and off with a hard colour swap,
        // which in a grid of eight buttons makes it hard to see which one moved.
        // Tweening the fill through ImAnim means the eye follows the change.
        {
            const float dt = ImGui::GetIO().DeltaTime;
            const ImVec4 idle = ImGui::GetStyleColorVec4(ImGuiCol_Button);
            const ImVec4 on   = ImVec4(0.00f, 0.41f, 0.71f, 1.00f);
            for (auto& mb : MODES) {
                const bool active = (state.renderMode == mb.mode);
                const ImGuiID bid = ImGui::GetID(mb.label);
                const ImVec4 fill = iam_tween_color(
                    bid, ImGui::GetID("fill"), active ? on : idle, 0.18f,
                    iam_ease_preset(iam_ease_out_cubic), iam_policy_crossfade,
                    iam_col_oklab, dt);
                ImGui::PushStyleColor(ImGuiCol_Button, fill);
                if (ImGui::Button(mb.label, ImVec2(BTN_W, 22.0f)))
                    state.renderMode = mb.mode;
                if (ImGui::IsItemHovered() && state.uiTooltips)
                    ImGui::SetTooltip("%s", mb.tip);
                ImGui::PopStyleColor();
                if (++mi % 4 != 0) ImGui::SameLine(0, 3.0f);
            }
        }

        ImGui::Spacing();

        // ---- Display options ----------------------------------------
        ImGui::SeparatorText("Display");
        ImGui::Checkbox("Wireframe",   &state.showWireframe); ImGui::SameLine(128);
        ImGui::Checkbox("Linear",      &state.linearFilter);
        ImGui::Checkbox("Vert.Colors", &state.useVertexColors);
        ImGui::SetNextItemWidth(-44.0f);
        ImGui::SliderFloat("##bright", &state.brightness, 0.5f, 3.0f);
        ImGui::SameLine(); ImGui::TextDisabled("Bright");

        // ---- Model sections nothing placed --------------------------
        {
            size_t orphan = 0, placedSecs = 0;
            for (const auto& s : g_ShoSections) {
                if (s.isWorldSpace) continue;
                if (s.instances.empty()) orphan++; else placedSecs++;
            }
            if (orphan || placedSecs) {
                ImGui::Checkbox("Unplaced models", &state.showUnplacedModels);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "%zu model sections are placed by game objects.\n"
                        "%zu are referenced by nothing; showing them piles\n"
                        "their geometry on the origin.", placedSecs, orphan);
            }
        }

        // ---- Overlay objects (shown when loaded) -------------------
        if (GpuPeek(g_Collision) || !g_Clumps.empty()) {
            ImGui::Spacing();
            ImGui::SeparatorText("Overlay");
            if (GpuPeek(g_Collision)) {
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
            if (!g_GameObjects.empty()) {
                size_t placed = 0;
                for (const auto& go : g_GameObjects) if (!go.atOrigin) placed++;
                ImGui::Checkbox("Objects", &state.showGameObjects);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%zu game objects, %zu with a world transform",
                                      g_GameObjects.size(), placed);
                if (state.showGameObjects) {
                    ImGui::SameLine(128);
                    ImGui::Checkbox("Labels", &state.showObjectLabels);
                    ImGui::Checkbox("At origin", &state.showOriginObjects);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Also show the %zu logical objects that carry\n"
                                          "an identity transform (CZone, GameMessage, ...)",
                                          g_GameObjects.size() - placed);
                }
            }
        }

        ImGui::Spacing();

        // ---- Panels & extras ---------------------------------------
        ImGui::SeparatorText("Panels");
        ImGui::Checkbox("Structure", &state.showStructure); ImGui::SameLine(128);
        ImGui::Checkbox("Textures",  &state.showTextures);
        ImGui::Checkbox("Archive",   &state.showArc); ImGui::SameLine(128);
        ImGui::Checkbox("Manual",    &state.showManual);
        ImGui::Checkbox("Playback",  &state.showAudioPlayer);

        // Wii-only options; hidden when the loaded container has neither.
        {
            bool anyIce = false, anyAlt = false;
            for (auto& obj : ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects()) {
                for (auto* cPtr : obj->GetMeshes()) {
                    const auto& c = *cPtr;
                    anyIce |= c.iceEffect;
                    anyAlt |= !c.altTexName.empty() && c.altTexName != c.texName;
                }
            }
            if (anyIce || anyAlt) {
                ImGui::Spacing();
                ImGui::TextDisabled("Wii");
                if (anyIce) {
                    ImGui::Checkbox("Ice shading", &state.iceShading);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Fresnel and specular over the colour map.\n"
                                          "An approximation: the real look comes from\n"
                                          "the GX TEV stages, which the container does\n"
                                          "not store.");
                }
                if (anyAlt) {
                    ImGui::Checkbox("Frozen variant", &state.frozenVariant);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Draw the alternate texture the material\n"
                                          "names in its 0x0129 extension - the same\n"
                                          "surface in its Otherworld state.");
                }
            }
        }

        // ---- Export -------------------------------------------------
        if (haveModel) {
            ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
            if (ImGui::CollapsingHeader("Export glTF")) {
                static GlbExportOptions opt;
                static std::string exportMsg;
                static bool exportOk = false;

                ImGui::Checkbox("Embed textures",  &opt.embedTextures);
                ImGui::Checkbox("Vertex colors",   &opt.includeVertexColors);
                ImGui::Checkbox("Lights",          &opt.includeLights);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Export CColorLight objects as\nKHR_lights_punctual lights");
                ImGui::Checkbox("Bake instances",  &opt.bakeInstances);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Write one copy of a model per placement.\n"
                                      "Off: only the first placement is written.");

                ImGui::TextDisabled("One mesh per texture name");
                if (ImGui::Button("Export .glb", ImVec2(-1, 0))) {
                    std::string name = g_CurrentMeshContainer.empty()
                        ? std::string("scene")
                        : fs::path(g_CurrentMeshContainer).filename().string();
                    const std::string out = name + ".glb";
                    std::string err;
                    exportOk = ExportGLB(out, opt, err);
                    exportMsg = exportOk ? ("Saved " + out) : ("Failed: " + err);
                    std::cerr << "[export] " << exportMsg << "\n";
                }
                if (!exportMsg.empty())
                    ImGui::TextColored(exportOk ? ImVec4(0.52f, 0.86f, 0.52f, 1.0f)
                                                : ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                                       "%s", exportMsg.c_str());
            }
        }

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

        if (ImGui::CollapsingHeader("Settings")) {
            ImGui::SeparatorText("Interface");

            ImGui::SetNextItemWidth(-70.0f);
            if (ImGui::SliderFloat("##uiscale", &state.uiScale, 0.8f, 1.6f, "%.2f"))
                ImGui::GetIO().FontGlobalScale = state.uiScale;
            ImGui::SameLine(); ImGui::TextDisabled("Scale");
            if (ImGui::IsItemHovered() && state.uiTooltips)
                ImGui::SetTooltip("Size of every panel and label. Useful on a\n"
                                  "high-density display where 256 px of panel\n"
                                  "is a very small 256 px.");

            // ImAnim drives the panel's motion; this is its master rate, so 0
            // turns every transition off rather than leaving them half-done.
            ImGui::SetNextItemWidth(-70.0f);
            if (ImGui::SliderFloat("##uianim", &state.uiAnimSpeed, 0.0f, 2.0f, "%.2fx"))
                iam_set_global_time_scale(state.uiAnimSpeed);
            ImGui::SameLine(); ImGui::TextDisabled("Motion");
            if (ImGui::IsItemHovered() && state.uiTooltips)
                ImGui::SetTooltip("Speed of button and list transitions.\n"
                                  "Set to 0 for no animation at all.");

            ImGui::Checkbox("Tooltips", &state.uiTooltips);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Turn these explanations off once you know\n"
                                  "your way around.");

            if (ImGui::Button("Reset interface", ImVec2(-1, 0))) {
                state.uiScale = 1.0f;
                state.uiAnimSpeed = 1.0f;
                state.uiTooltips = true;
                ImGui::GetIO().FontGlobalScale = 1.0f;
                iam_set_global_time_scale(1.0f);
            }

            ImGui::SeparatorText("Paths");
            {
                const std::string arc = LoadArcPref();
                ImGui::TextDisabled("Archive remembered:");
                ImGui::TextWrapped("%s", arc.empty() ? "(none yet)" : arc.c_str());
                if (ImGui::IsItemHovered() && !arc.empty() && state.uiTooltips)
                    ImGui::SetTooltip("%s", arc.c_str());
                if (!arc.empty() && ImGui::Button("Forget", ImVec2(-1, 0)))
                    SaveArcPref("");
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
        
        RenderPlaybackPanel();

        ImGui::End();

        if (state.showStructure) RenderStructureWindow();
        if (state.showTextures)  RenderTxdWindow();
        if (state.showArc)       RenderArcWindow();
        if (state.showManual)    RenderManualWindow();

        // File browser
        g_FileBrowser.Render();
        } else {
            // F1 hides everything; leave one hint so the UI is recoverable.
            auto* dl = ImGui::GetForegroundDrawList();
            dl->AddText(ImVec2(12.0f, (float)winH - 22.0f),
                        IM_COL32(190, 190, 200, 150), "F1 - show interface");
        }

        // Doorway prompt. Drawn on the foreground list so it survives F1, and
        // centred low like the game's own use-prompt.
        if (state.playMode && s_zoneLinkHere >= 0 && !s_zonePrompt.empty()) {
            char buf[192];
            snprintf(buf, sizeof(buf), "E  -  %s", s_zonePrompt.c_str());
            auto* dl = ImGui::GetForegroundDrawList();
            const ImVec2 sz = ImGui::CalcTextSize(buf);
            const ImVec2 at((winW - sz.x) * 0.5f, (float)winH * 0.78f);
            dl->AddRectFilled(ImVec2(at.x - 12.0f, at.y - 6.0f),
                              ImVec2(at.x + sz.x + 12.0f, at.y + sz.y + 6.0f),
                              IM_COL32(0, 0, 0, 150), 4.0f);
            dl->AddText(at, IM_COL32(235, 235, 245, 255), buf);
        }

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
    glDeleteVertexArrays(1, &markerVao);
    glDeleteBuffers(1, &markerVbo);
    ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().UnmountAll();

    for (auto& [name, id] : g_TextureMap) glDeleteTextures(1, &id);
    ReleaseAllGpuMeshes();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

