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

#include "ClimaxEngine/Core/Arc.h"
#include "ClimaxEngine/Core/Common.h"
#include "ClimaxEngine/Loader/Export.h"
#include "ClimaxEngine/Loader/Loader.h"
#include "ClimaxEngine/Rendering/CPURasterizer.h"
#include "ClimaxEngine/UI/UI.h"
#include "ClimaxEngine/Core/AudioParser.h"

// ---------------------------------------------------------------------------
// Audio playback
//
// One SDL device at a time, reopened whenever the next clip has a different
// rate or channel count -- the level banks alone span 6 kHz mono to 32 kHz,
// and the cutscene streams are 48 kHz stereo.
// ---------------------------------------------------------------------------
static AudioClip         g_NowPlaying;
static SDL_AudioDeviceID g_AudioDevice = 0;
static size_t            g_AudioPos    = 0;
static int               g_DeviceRate  = 0;
static int               g_DeviceChans = 0;

ArcArchive g_IgcArc;   // cutscene archive, when one is found beside SH.ARC

const AudioClip& CurrentAudioClip() { return g_NowPlaying; }

static void AudioCallback(void*, Uint8* stream, int len) {
    int16_t* out = (int16_t*)stream;
    const int need = len / (int)sizeof(int16_t);
    const size_t total = g_NowPlaying.pcm.size();

    if (!state.isAudioPlaying || total == 0) {
        std::memset(stream, 0, (size_t)len);
        return;
    }

    const float vol = state.audioVolume;
    for (int i = 0; i < need; ++i) {
        if (g_AudioPos >= total) {
            if (state.audioLoop) {
                g_AudioPos = 0;
            } else {
                std::memset(out + i, 0, (size_t)(need - i) * sizeof(int16_t));
                state.isAudioPlaying = false;
                break;
            }
        }
        const int32_t s = (int32_t)((float)g_NowPlaying.pcm[g_AudioPos++] * vol);
        out[i] = (int16_t)(s < -32768 ? -32768 : (s > 32767 ? 32767 : s));
    }
    state.audioProgress = (float)g_AudioPos / (float)total;
}

// Reopens the device only when the format actually changed; SDL resamples for
// us, but it cannot switch rate on a live device.
static bool OpenAudioDeviceFor(const AudioClip& clip) {
    if (g_AudioDevice && g_DeviceRate == clip.sampleRate &&
        g_DeviceChans == clip.channels)
        return true;

    if (g_AudioDevice) {
        SDL_CloseAudioDevice(g_AudioDevice);
        g_AudioDevice = 0;
    }

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq     = clip.sampleRate;
    want.format   = AUDIO_S16SYS;
    want.channels = (Uint8)clip.channels;
    want.samples  = 2048;
    want.callback = AudioCallback;

    g_AudioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (!g_AudioDevice) {
        std::cerr << "[audio] SDL_OpenAudioDevice failed: " << SDL_GetError()
                  << "\n";
        return false;
    }
    g_DeviceRate  = clip.sampleRate;
    g_DeviceChans = clip.channels;
    return true;
}

void PlayAudioClip(const AudioClip& clip) {
    if (!clip.Valid()) return;

    if (g_AudioDevice) {
        SDL_PauseAudioDevice(g_AudioDevice, 1);
        SDL_LockAudioDevice(g_AudioDevice);
    }
    state.isAudioPlaying = false;
    g_NowPlaying = clip;
    g_AudioPos = 0;
    state.audioProgress = 0.0f;
    if (g_AudioDevice) SDL_UnlockAudioDevice(g_AudioDevice);

    if (!OpenAudioDeviceFor(g_NowPlaying)) return;
    state.isAudioPlaying = true;
    state.showAudioPlayer = true;
    SDL_PauseAudioDevice(g_AudioDevice, 0);
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

void ToggleAudioPlayback() {
    if (!g_AudioDevice || !g_NowPlaying.Valid()) return;
    // Restart rather than resume once the clip has run to its end.
    if (!state.isAudioPlaying && g_AudioPos >= g_NowPlaying.pcm.size())
        g_AudioPos = 0;
    state.isAudioPlaying = !state.isAudioPlaying;
    SDL_PauseAudioDevice(g_AudioDevice, state.isAudioPlaying ? 0 : 1);
}

void StopAudio() {
    if (g_AudioDevice) SDL_PauseAudioDevice(g_AudioDevice, 1);
    state.isAudioPlaying = false;
    g_AudioPos = 0;
    state.audioProgress = 0.0f;
}

void SetAudioProgress(float progress) {
    if (!g_NowPlaying.Valid()) return;
    progress = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
    size_t pos = (size_t)(progress * (float)g_NowPlaying.pcm.size());
    pos -= pos % (size_t)g_NowPlaying.channels;   // never split a frame
    if (g_AudioDevice) SDL_LockAudioDevice(g_AudioDevice);
    g_AudioPos = pos;
    state.audioProgress = progress;
    if (g_AudioDevice) SDL_UnlockAudioDevice(g_AudioDevice);
}

// The music and cutscenes are not inside SH.ARC: MUSIC/ and IGC.ARC sit beside
// it on the disc. Mounting the archive is enough to find them.
void ScanAudioLibrary() {
    g_AudioLibrary.clear();
    if (!g_Arc.IsOpen()) return;

    std::error_code ec;
    const fs::path root = fs::path(g_Arc.Path()).parent_path();

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
              << fs::path(g_Arc.Path()).filename().string() << " (looked in "
              << root.string() << ")\n";
    if (!g_AudioLibrary.empty() && !state.audioAutoOpened) {
        state.audioAutoOpened = true;
        state.showAudioPlayer = true;
    }
}

// ---------------------------------------------------------------------------
// Prefs: persist the last opened .arc path so the next launch auto-mounts it.
// File: <basePath>/SHOViewer.prefs  (one line = arc path)
// ---------------------------------------------------------------------------
static std::string g_PrefsPath;

static void InitPrefsPath() {
    // SDL_GetBasePath() returns the directory containing the executable.
    char* base = SDL_GetBasePath();
    if (base) {
        g_PrefsPath = std::string(base) + "SHOViewer.prefs";
        SDL_free(base);
    } else {
        g_PrefsPath = "SHOViewer.prefs";
    }
}

void SaveArcPref(const std::string& arcPath) {
    if (g_PrefsPath.empty() || arcPath.empty()) return;
    std::ofstream f(g_PrefsPath, std::ios::trunc);
    if (f) f << arcPath << "\n";
}

static std::string LoadArcPref() {
    if (g_PrefsPath.empty()) return {};
    std::ifstream f(g_PrefsPath);
    std::string line;
    if (f && std::getline(f, line) && !line.empty()) return line;
    return {};
}

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

    SDL_Window* win = SDL_CreateWindow("Climax Silent Hill Engine Toolkit 0.2",
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
    //   SHOViewer SH.ARC [LevelName]        — mount the archive, load by name
    //   SHOViewer <container> [txd ...]     — loose files; TXDs are optional
    if (argc >= 2) {
        const std::string first = argv[1];
        const bool looksLikeArc =
            first.size() > 4 && sho_stricmp(first.c_str() + first.size() - 4, ".arc") == 0;

        if (looksLikeArc && g_Arc.Open(first)) {
            ScanAudioLibrary();
            SaveArcPref(first);                         // ← remember for next launch
            std::cerr << "[arc] mounted " << first << " ("
                      << g_Arc.Entries().size() << " files)\n";
            if (argc >= 3 && std::string(argv[2]) != "--export") {
                const int idx = g_Arc.Find(argv[2]);
                if (idx >= 0) LoadLevelFromArc(idx);
                else std::cerr << "[arc] no entry named '" << argv[2] << "'\n";
            }
        } else {
            if (looksLikeArc)
                std::cerr << "[arc] " << g_Arc.Error() << " — treating as a container\n";
            std::vector<std::string> txds;
            for (int i = 2; i < argc; i++) txds.push_back(argv[i]);
            LoadLevel(first, txds);
        }
    } else {
        // No CLI argument: try to auto-mount the last opened archive
        const std::string saved = LoadArcPref();
        if (!saved.empty()) {
            if (g_Arc.Open(saved)) {
                ScanAudioLibrary();
                std::cerr << "[arc] auto-mounted last arc: " << saved
                          << " (" << g_Arc.Entries().size() << " files)\n";
                state.showArc = true;   // open the archive browser automatically
            } else {
                std::cerr << "[arc] prefs arc no longer accessible: " << saved << "\n";
            }
        }
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
uniform vec4  matColor;
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
    } else if(untextured){
        // A material with no texture chunk: flat colour times vertex colour,
        // which is how the game shades it. Sampling the unbound sampler here
        // returned solid black instead.
        vec4 col = matColor;
        if(useVertexColors && !unlitGeometry) col *= VC;
        col.rgb *= brightness;
        if(col.a < 0.05) discard;
        FragColor = col;
    } else {
        // Textured (default, renderMode == 0)
        vec4 tex = texture(t, TC);
        // Discard only what is fully transparent. Cutting at 0.1 threw away the
        // whole soft edge of a gradient and left a hard jagged border where the
        // game fades out smoothly; the rest is handled by alpha blending.
        if(tex.a < 0.02) discard;
        // Additive effect sheets carry their own brightness. Multiplying them by
        // the baked vertex lighting drives them to black in a dark room, which
        // is why they only showed up with vertex colours switched off.
        vec4 col = (useVertexColors && !additive && !unlitGeometry) ? tex * VC : tex;
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
            if (e.type == SDL_DROPFILE) {
                std::string path = e.drop.file;
                SDL_free(e.drop.file);

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

        // Key 1 → reset camera, F1 → hide/show the whole interface
        if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_1, false)) {
            state.camTargetX = 0; state.camTargetY = 2; state.camTargetZ = 0;
            state.camYaw = 0; state.camPitch = 20; state.camDist = 15;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) state.showUI = !state.showUI;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) state.showManual = !state.showManual;
        if (ImGui::IsKeyPressed(ImGuiKey_G,  false) && !io.WantCaptureKeyboard)
            state.showPivotGizmo = !state.showPivotGizmo;

        const bool haveModel = !g_Chunks.empty();

        if (state.renderDevice == RenderDevice::CPU) {
            // --- CPU Software Rasterization Pass ---
            g_CPURasterizer.Init(fbW, fbH);
            g_CPURasterizer.Clear(state.skyColorBot[0], state.skyColorBot[1], state.skyColorBot[2], 1.0f);
            if (haveModel) {
                g_CPURasterizer.RenderScene(g_Chunks, mvp, eye);
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
            if (g_Chunks.size() != s_lastLoadChunkCount) {
                s_lastLoadChunkCount = g_Chunks.size();
                s_debugPrinted = false;
            }
            if (!s_debugPrinted && !g_Chunks.empty()) {
                std::cerr << "[render] first frame: chunks=" << g_Chunks.size()
                          << " sections=" << g_ShoSections.size()
                          << " gameObjects=" << g_GameObjects.size() << "\n";
                std::cerr.flush();
            }
            float dt = ImGui::GetIO().DeltaTime;
            for (auto& go : g_GameObjects) {
                if (go.currentClipIndex >= 0 && go.currentClipIndex < (int)go.clipSectionIndices.size()) {
                    go.animTime += dt;
                } // No auto-start: user picks clip from UI
            }
            if (!s_debugPrinted && !g_Chunks.empty()) {
                std::cerr << "[render] after anim loop OK\n";
                std::cerr.flush();
            }
        const GLint uUntex  = glGetUniformLocation(p, "untextured");
        const GLint uAdd    = glGetUniformLocation(p, "additive");
        const GLint uUnlit  = glGetUniformLocation(p, "unlitGeometry");
        const GLint uMatCol = glGetUniformLocation(p, "matColor");
            const glm::mat4 identity(1.0f);
            // Two passes: opaque first with depth writes on, blended second
            // with them off. A blended surface that writes depth hides whatever
            // stands behind it, which is why one semi-transparent sheet made the
            // next one disappear.
            for (int pass = 0; pass < 2; pass++) {
            for (const auto& chunk : g_Chunks) {
                // "GreyAlpha_<base>" is the mask half of a two-pass transparency
                // setup, white shapes on black. Drawing it as a colour map paints
                // white branches over the tree. The base texture already carries
                // its own alpha, so the mask pass is redundant here.
                if (chunk.alphaPass) continue;
                {
                    // Effect sheets must never occlude each other. Some fire and
                    // ember cards have near-binary alpha (FX_ember_Dahlia is 70%
                    // clear / 4% partial / 24% opaque), so a gradient test alone
                    // left them in the opaque pass where they wrote depth and hid
                    // the softer flames behind them.
                    auto itG0 = g_TexGradient.find(chunk.texName);
                    const bool isFx = chunk.texName.size() > 3 &&
                        sho_strnicmp(chunk.texName.c_str(), "FX_", 3) == 0;
                    const bool blended = isFx || chunk.additive ||
                        (itG0 != g_TexGradient.end() && itG0->second);
                    if ((pass == 0) == blended) continue;
                }
                // Use the directly stored texName (set at load time per-geometry-object)
                const std::string& tName = chunk.texName;
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
                // A material with no texture chunk and a pure white colour is a
                // placeholder sheet the game does not draw — those were the white
                // cards standing in front of the fir trees. Untextured materials
                // Materials with no texture chunk paint solid white cards over
                // characters and props, so they stay hidden until the real rule
                // for them is known.
                if (chunk.untextured) continue;
                // An FX sheet whose alpha is a gradient fades out by itself and
                // must be alpha-blended: forcing it additive ignored the alpha
                // entirely and drew the whole quad, so the fire cards showed
                // their rectangular borders.
                // Blending is NOT recorded in the container: on PS2 it is the
                // GS ALPHA register, set per draw. FX_fire_Dahlia and FX_Flare_01
                // have identical signatures in the asset (flat opaque alpha, no
                // gradient, no clear texel) yet need opposite treatment, so no
                // rule derived from the texture can separate them.
                //
                // Everything therefore blends normally — that is what makes the
                // fire, the smoke and the TV screen come out right — and only the
                // glow sprites listed here are additive. They fade through their
                // COLOUR over a black surround, so alpha blending would draw that
                // surround as a black card.
                static const char* kAdditiveNames[] = {
                    "FX_Flare", "FX_Glow", "FX_Halo", "FX_Corona", "FX_Lens",
                };
                bool addNow = false;
                for (const char* pre : kAdditiveNames)
                    if (tName.size() >= strlen(pre) &&
                        sho_strnicmp(tName.c_str(), pre, strlen(pre)) == 0) {
                        addNow = true;
                        break;
                    }
                glUniform1i(uAdd, addNow ? 1 : 0);
                glUniform1i(uUnlit, chunk.unlitGeometry ? 1 : 0);
                if (addNow) {
                    glBlendFunc(GL_ONE, GL_ONE);
                    glDepthMask(GL_FALSE);
                } else {
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    // Blended geometry must not write depth or it hides whatever
                    // stands behind it.
                    glDepthMask(pass == 0 ? GL_TRUE : GL_FALSE);
                }
                glBindTexture(GL_TEXTURE_2D, tid);
                // A resolved-but-missing texture still renders as a flat material.
                glUniform1i(uUntex, (chunk.untextured || tid == 0) ? 1 : 0);
                glUniform4fv(uMatCol, 1, glm::value_ptr(chunk.matColor));
                glBindVertexArray(chunk.vao);

                // World geometry draws once; a model section draws once per game
                // object that placed it, and not at all when nothing references it.
                const ShoSection* sec = (chunk.sectionIndex >= 0 &&
                                         chunk.sectionIndex < (int)g_ShoSections.size())
                                        ? &g_ShoSections[chunk.sectionIndex] : nullptr;
                if (!sec || sec->isWorldSpace || sec->instances.empty()) {
                    if (sec && !sec->isWorldSpace && !state.showUnplacedModels) continue;
                    glUniformMatrix4fv(uM,     1, GL_FALSE, glm::value_ptr(mvp));
                    glUniformMatrix4fv(uModel, 1, GL_FALSE, glm::value_ptr(identity));
                    glUniform1i(glGetUniformLocation(p, "useSkinning"), 0);
                    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)chunk.vertices.size());
                } else {
                    for (size_t instIdx = 0; instIdx < sec->instances.size(); instIdx++) {
                        const auto& inst = sec->instances[instIdx];
                        glm::mat4 m = mvp * inst.transform;
                        glUniformMatrix4fv(uM,     1, GL_FALSE, glm::value_ptr(m));
                        glUniformMatrix4fv(uModel, 1, GL_FALSE, glm::value_ptr(inst.transform));
                        
                        bool hasSkin = false;
                        if (inst.gameObjectId >= 0 && inst.gameObjectId < (int)g_GameObjects.size() && !sec->skeleton.bones.empty()) {
                            GameObject& go = g_GameObjects[inst.gameObjectId];
                            if (go.currentClipIndex >= 0 && go.currentClipIndex < (int)go.clipSectionIndices.size()) {
                                int clipSecIdx = go.clipSectionIndices[go.currentClipIndex];
                                if (clipSecIdx >= 0 && clipSecIdx < (int)g_ShoSections.size()) {
                                    const AnimClip& clip = g_ShoSections[clipSecIdx].animClip;
                                    if (clip.duration > 0.0f) {
                                        // Evaluate tracks at go.animTime
                                        float t = fmod(go.animTime, clip.duration);
                                        const size_t numBones = std::min(sec->skeleton.bones.size(), (size_t)128);
                                        std::vector<glm::mat4> boneMats(numBones, glm::mat4(1.0f));
                                        
                                        for (size_t b = 0; b < numBones; ++b) {
                                            glm::mat4 local = sec->skeleton.bones[b].restLocal;
                                            if (b < clip.tracks.size() && !clip.tracks[b].times.empty()) {
                                                const auto& times = clip.tracks[b].times;
                                                const auto& poss  = clip.tracks[b].pos;
                                                const auto& rots  = clip.tracks[b].rot;
                                                
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
                                            
                                            int parent = sec->skeleton.bones[b].parent;
                                            // Guard: parent must be a valid, already-computed bone index
                                            if (parent >= 0 && parent < (int)b) {
                                                boneMats[b] = boneMats[parent] * local;
                                            } else {
                                                boneMats[b] = local;
                                            }
                                        }
                                        
                                        // Compute skinning matrices and upload to GPU
                                        std::vector<glm::mat4> shaderTransforms(numBones);
                                        for (size_t b = 0; b < numBones; ++b)
                                            shaderTransforms[b] = boneMats[b] * sec->skeleton.bones[b].invBind;
                                        
                                        if (!shaderTransforms.empty()) {
                                            glUniformMatrix4fv(glGetUniformLocation(p, "boneTransforms"),
                                                (GLsizei)shaderTransforms.size(), GL_FALSE,
                                                glm::value_ptr(shaderTransforms[0]));
                                            hasSkin = true;
                                        }
                                    }
                                }
                            }
                        }
                        
                        glUniform1i(glGetUniformLocation(p, "useSkinning"), hasSkin ? 1 : 0);
                        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)chunk.vertices.size());
                    }
                }
            }
            } // opaque pass, then blended pass
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
        ImGui::Begin("SHO Viewer", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);

        if (ImGui::Button("Open SH.ARC", ImVec2(-1, 0))) g_FileBrowser.Open(FileBrowserMode::Arc);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Load the game archive and browse levels by their real names");
        if (g_Arc.IsOpen()) {
            ImGui::TextDisabled("%s", fs::path(g_Arc.Path()).filename().string().c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%zu files)", g_Arc.Entries().size());
        }
        if (ImGui::Button("Open Loose File", ImVec2(-1, 0))) g_FileBrowser.Open(FileBrowserMode::Mesh);

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

        // ---- Level cameras ------------------------------------------
        if (!g_Cameras.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Level cameras (%zu)", g_Cameras.size());
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
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("(%.2f, %.2f, %.2f)",
                                          c.position.x, c.position.y, c.position.z);
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            ImGui::Spacing();
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

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        // ---- Panels & extras ---------------------------------------
        ImGui::TextDisabled("Panels");
        ImGui::Checkbox("Structure", &state.showStructure); ImGui::SameLine(128);
        ImGui::Checkbox("Textures",  &state.showTextures);
        ImGui::Checkbox("Archive",   &state.showArc); ImGui::SameLine(128);
        ImGui::Checkbox("Manual",    &state.showManual);
        ImGui::Checkbox("Audio",     &state.showAudioPlayer);

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
        
        RenderAudioPlayer();

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
    g_Arc.Close();

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

