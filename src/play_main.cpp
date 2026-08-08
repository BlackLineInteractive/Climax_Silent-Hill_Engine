// ─────────────────────────────────────────────────────────────────────────────
// climax-play — the game, without the toolkit.
//
// This exists to make the boundary real. It links climax-core and climax-game
// and nothing else of ours; if anyone puts an ImGui include into either, this
// target stops linking, and the rule stops being a comment in a header.
//
// It is small on purpose. A window, a 2D pass, the boot sequence, and the main
// menu read out of the retail archive: the four XML screens, their button
// textures, the string table and the font. What it draws is not a mock-up of
// the menu; it is the menu, at the coordinates the game ships.
// ─────────────────────────────────────────────────────────────────────────────
#include <GL/glew.h>
#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ClimaxEngine/Core/RWS/FileSystem/CArchive.h"
#include "ClimaxEngine/Core/UI/Font.h"
#include "ClimaxEngine/Core/UI/ScreenDef.h"
#include "ClimaxEngine/Core/UI/StringTable.h"
#include "ClimaxEngine/Game/FrontEnd.h"
#include "ClimaxEngine/Platform/PS2/PS2Texture.h"

using namespace ClimaxEngine;

// The game authors its screens at 1280x720 for widescreen; every xpos/ypos in
// the XML is in that space, and the 4:3 pair is for 640x480.
static const float kAuthorW = 1280.0f;
static const float kAuthorH = 720.0f;

// ── a 2D pass, which is all a menu needs ─────────────────────────────────────

namespace {

const char *kVert = R"(#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
uniform vec2 uScreen;
out vec2 vUV;
void main() {
    vec2 ndc = vec2(aPos.x / uScreen.x * 2.0 - 1.0,
                    1.0 - aPos.y / uScreen.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    vUV = aUV;
})";

const char *kFrag = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform vec4 uColour;
uniform int uTextured;
out vec4 o;
void main() {
    o = uTextured != 0 ? texture(uTex, vUV) * uColour : uColour;
})";

GLuint Compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[play] shader: %s\n", log);
    }
    return s;
}

class Painter {
public:
    bool Init() {
        m_prog = glCreateProgram();
        glAttachShader(m_prog, Compile(GL_VERTEX_SHADER, kVert));
        glAttachShader(m_prog, Compile(GL_FRAGMENT_SHADER, kFrag));
        glLinkProgram(m_prog);
        m_uScreen = glGetUniformLocation(m_prog, "uScreen");
        m_uColour = glGetUniformLocation(m_prog, "uColour");
        m_uTextured = glGetUniformLocation(m_prog, "uTextured");

        glGenVertexArrays(1, &m_vao);
        glGenBuffers(1, &m_vbo);
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
        glEnableVertexAttribArray(1);
        glBindVertexArray(0);
        return m_prog != 0;
    }

    void Begin(int w, int h) {
        glUseProgram(m_prog);
        glUniform2f(m_uScreen, (float)w, (float)h);
        glBindVertexArray(m_vao);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
    }

    void Quad(float x, float y, float w, float h, GLuint tex, float r, float g,
              float b, float a, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
        const float v[24] = {
            x,     y,     u0, v0,  x + w, y,     u1, v0,  x + w, y + h, u1, v1,
            x,     y,     u0, v0,  x + w, y + h, u1, v1,  x,     y + h, u0, v1,
        };
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
        glUniform4f(m_uColour, r, g, b, a);
        glUniform1i(m_uTextured, tex ? 1 : 0);
        if (tex) glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

private:
    GLuint m_prog = 0, m_vao = 0, m_vbo = 0;
    GLint m_uScreen = -1, m_uColour = -1, m_uTextured = -1;
};

// Reads an entry out of the archive.
bool ReadEntry(RWS::FileSystem::CArchive &arc, const char *name,
               std::vector<uint8_t> &out) {
    const int i = arc.Find(name);
    return i >= 0 && arc.Read((size_t)i, out) && !out.empty();
}

// Walks a container's sections looking for one of a given RenderWare type.
const uint8_t *FindSection(const std::vector<uint8_t> &buf, const char *type,
                           size_t &sizeOut) {
    const uint32_t RW = 0x1C020065;
    size_t off = 0;
    while (off + 12 <= buf.size()) {
        uint32_t t, s, v;
        std::memcpy(&t, &buf[off], 4);
        std::memcpy(&s, &buf[off + 4], 4);
        std::memcpy(&v, &buf[off + 8], 4);
        if (v != RW || s == 0 || off + 12 + s > buf.size()) break;
        const size_t inner = off + 12;
        uint32_t hdr, tagLen;
        std::memcpy(&hdr, &buf[inner], 4);
        std::memcpy(&tagLen, &buf[inner + 4], 4);
        const size_t g = inner + 8 + tagLen;
        if (g + 20 < buf.size() &&
            std::strncmp((const char *)&buf[g + 20], type, std::strlen(type)) == 0) {
            const size_t data = inner + 4 + hdr;
            sizeOut = buf.size() - data;
            return &buf[data];
        }
        off += 12 + s;
    }
    sizeOut = 0;
    return nullptr;
}

} // namespace

int main(int argc, char **argv) {
    const char *arcPath = "game-iso/SHO/SH.ARC";
    // --check loads everything and reports, without opening a window. The data
    // half is worth testing on its own: it is the half that can be wrong
    // quietly, and it is the half a build machine can run.
    bool checkOnly = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--check") == 0) checkOnly = true;
        else arcPath = argv[i];
    }

    RWS::FileSystem::CArchive arc;
    if (!arc.Open(arcPath)) {
        std::fprintf(stderr, "[play] cannot open %s\n", arcPath);
        return 1;
    }

    if (!checkOnly && SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "[play] SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *win = nullptr;
    SDL_GLContext ctx = nullptr;
    Painter painter;
    if (!checkOnly) {
        win = SDL_CreateWindow("Silent Hill Origins", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, 1280, 720,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI |
                                   SDL_WINDOW_RESIZABLE);
        ctx = SDL_GL_CreateContext(win);
        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK) {
            std::fprintf(stderr, "[play] no GL\n");
            return 1;
        }
        if (!painter.Init()) return 1;
    }

    // ── textures: the decoder hands pixels here, this sink uploads them ──────
    std::map<std::string, GLuint> textures;
    SetTextureExists([&](const std::string &n) { return textures.count(n) != 0; });
    SetTextureSink([&](RawTexture &raw, const std::vector<uint8_t> &rgba, int w,
                       int h) {
        if (checkOnly) { textures[raw.name] = 1; return; }
        GLuint id = 0;
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        textures[raw.name] = id;
        raw.glID = id;
    });

    // ── the data the menu is made of ────────────────────────────────────────
    std::vector<uint8_t> buf;

    UI::StringTable strings;
    if (ReadEntry(arc, "Strings.Eng", buf))
        strings.Load(buf.data(), buf.size());

    UI::Font font;
    std::vector<uint8_t> fontBuf;
    if (ReadEntry(arc, "FontEUR", fontBuf)) {
        size_t n = 0;
        if (const uint8_t *k = FindSection(fontBuf, "rwID_KFONT", n))
            font.Load(k, n);
        ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(fontBuf, {}, true);
    }

    // The per-language button art, then the screens themselves.
    if (ReadEntry(arc, "LocaleUIEng", buf))
        ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(buf, {}, true);

    std::vector<std::unique_ptr<UI::Element>> owned;
    std::vector<std::pair<std::string, const UI::Element *>> screens;
    for (const char *name : {"mainmenu", "newgame", "gameoptions", "pausemenu"}) {
        if (!ReadEntry(arc, (std::string(name) + ".xml").c_str(), buf))
            continue;
        auto root = std::make_unique<UI::Element>();
        std::string err;
        if (!UI::ParseXml((const char *)buf.data(), buf.size(), *root, &err)) {
            std::fprintf(stderr, "[play] %s.xml: %s\n", name, err.c_str());
            continue;
        }
        if (const UI::Element *scr = root->Find("SCREEN"))
            screens.emplace_back(name, scr);
        owned.push_back(std::move(root));
    }
    std::fprintf(stderr, "[play] %zu screens, %zu strings, %zu glyphs, %zu textures\n",
                 screens.size(), strings.Count(), font.Glyphs().size(), textures.size());

    Game::FrontEnd front;
    front.Menu().SetScreens(screens);

    if (checkOnly) {
        // Walk the boot sequence with a synthetic keypress each frame, so the
        // stage order and the menu wiring are exercised rather than assumed.
        Game::MenuInput press;
        press.anyKey = press.accept = true;
        Game::BootStage last = front.Stage();
        std::fprintf(stderr, "[check] stage %s\n", Game::BootStageName(last));
        for (int i = 0; i < 600 && front.Stage() != Game::BootStage::MainMenu; ++i) {
            front.Update(0.05f, i % 20 == 19 ? press : Game::MenuInput{});
            if (front.Stage() != last) {
                last = front.Stage();
                std::fprintf(stderr, "[check] stage %s\n", Game::BootStageName(last));
            }
        }
        std::fprintf(stderr, "[check] screen '%s', active '%s'\n",
                     front.Menu().ScreenId().c_str(), front.Menu().ActiveId().c_str());
        Game::MenuInput down; down.down = true;
        for (int i = 0; i < 4; ++i) {
            front.Update(0.016f, down);
            std::fprintf(stderr, "[check]   down -> '%s'\n",
                         front.Menu().ActiveId().c_str());
        }
        Game::MenuInput ok; ok.accept = true;
        const std::string cmd = front.Update(0.016f, ok);
        std::fprintf(stderr, "[check] accept -> screen '%s'%s\n",
                     front.Menu().ScreenId().c_str(),
                     cmd.empty() ? "" : (" command '" + cmd + "'").c_str());
        return 0;
    }

    // ── loop ────────────────────────────────────────────────────────────────
    bool run = true;
    uint64_t last = SDL_GetPerformanceCounter();
    while (run) {
        Game::MenuInput in;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) run = false;
            if (e.type != SDL_KEYDOWN) continue;
            in.anyKey = true;
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE:  in.cancel = true; break;
            case SDLK_UP:      in.up = true; break;
            case SDLK_DOWN:    in.down = true; break;
            case SDLK_LEFT:    in.left = true; break;
            case SDLK_RIGHT:   in.right = true; break;
            case SDLK_RETURN:
            case SDLK_SPACE:   in.accept = true; break;
            default: break;
            }
        }

        const uint64_t now = SDL_GetPerformanceCounter();
        const float dt = (float)((now - last) / (double)SDL_GetPerformanceFrequency());
        last = now;

        const std::string cmd = front.Update(dt, in);
        if (!cmd.empty())
            std::fprintf(stderr, "[play] command: %s\n", cmd.c_str());

        int w, h;
        SDL_GL_GetDrawableSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        painter.Begin(w, h);

        // Author-space to window-space. The game ships both a widescreen and a
        // 4:3 layout, and picks by display mode; this picks the same way.
        const bool wide = (float)w / (float)h > 1.5f;
        const float sx = (float)w / (wide ? kAuthorW : 640.0f);
        const float sy = (float)h / (wide ? kAuthorH : 480.0f);

        if (front.Stage() == Game::BootStage::MainMenu) {
            const UI::Element *scr = front.Menu().Screen();
            if (scr) {
                for (const UI::Element &b : scr->children) {
                    const float bx = (wide ? b.Float("xpos") : b.Float("xpos4x3")) * sx;
                    const float by = (wide ? b.Float("ypos") : b.Float("ypos4x3")) * sy;
                    const float bw = b.Float("width") * sx;
                    const float bh = b.Float("height") * sy;

                    // The button art is named with a .png that the archive
                    // stores without the extension.
                    std::string tex = b.Attr("bgtexture");
                    if (tex.size() > 4 && tex.compare(tex.size() - 4, 4, ".png") == 0)
                        tex.resize(tex.size() - 4);
                    auto it = textures.find(tex);
                    const GLuint id = it == textures.end() ? 0 : it->second;

                    const bool active = b.Attr("id") == front.Menu().ActiveId();
                    const float k = active ? 1.0f : 0.45f;
                    painter.Quad(bx, by, bw, bh, id, k, k, k, 1.0f,
                                 b.Float("textureu", 0.0f), b.Float("texturev", 0.0f),
                                 b.Float("texturew", 1.0f), b.Float("textureh", 1.0f));
                }
            }
        } else {
            // The timed stages have no screen of their own yet; a plate keeps
            // the sequence visible while the order is what is being tested.
            const float t = front.Stage() == Game::BootStage::Logo ? 0.35f : 0.12f;
            painter.Quad(0, 0, (float)w, (float)h, 0, t, t, t, 1.0f);
        }

        SDL_GL_SwapWindow(win);
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
