#include "ClimaxEngine/Render/PlayerModel.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

#include "ClimaxEngine/Core/RWS/FileSystem/CArchiveManager.h"
#include "ClimaxEngine/Loader/Loader.h"
#include "ClimaxEngine/Render/ViewerState.h"

PlayerModel g_Player;

static std::string s_lastClipName;

bool IsPlayerEffectMesh(const MeshChunk &m) {
    // Character containers ship effect sheets -- muzzle flashes, blood, the
    // shadow blob -- as full-size quads that the game scales down when it
    // spawns them. Drawn as authored they are metres across, which is the
    // white-and-red sheet lying under Travis's feet.
    //
    // Told apart by shape rather than by name: a body mesh is a shell of many
    // triangles, a blank is a single large quad. Six vertices and a span wider
    // than a person is not something a character is made of.
    if (m.vertices.empty())
        return true;

    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const Vertex &v : m.vertices) {
        lo = glm::min(lo, v.pos);
        hi = glm::max(hi, v.pos);
    }
    const glm::vec3 span = hi - lo;
    // Travis is about 0.6 m across the shoulders and 1.8 m tall. Anything
    // spanning two metres sideways, or three in any direction, is one of the
    // sheets -- counting vertices missed them because some are subdivided.
    return std::max(span.x, span.z) > 2.0f ||
           std::max(span.x, std::max(span.y, span.z)) > 3.0f;
}

// Materials and texture dictionaries disagree about capitalisation, so the
// decoder registers each texture under its own spelling plus an upper- and a
// lower-case alias, and the level renderer tries all three. Taking only the
// exact spelling left some of Travis's meshes with no texture of their own --
// the grey body showing through him from inside.
GLuint PlayerTextureId(const std::map<std::string, GLuint> &from,
                       const std::string &name) {
    auto it = from.find(name);
    if (it != from.end())
        return it->second;
    std::string alt = name;
    std::transform(alt.begin(), alt.end(), alt.begin(), ::toupper);
    if ((it = from.find(alt)) != from.end())
        return it->second;
    alt = name;
    std::transform(alt.begin(), alt.end(), alt.begin(), ::tolower);
    if ((it = from.find(alt)) != from.end())
        return it->second;
    return 0;
}

bool IsPlayerBodyMesh(const MeshChunk &m,
                      const std::map<std::string, GLuint> &textures) {
    if (IsPlayerEffectMesh(m))
        return false;
    if (m.texName.size() > 3 &&
        sho_strnicmp(m.texName.c_str(), "FX_", 3) == 0)
        return false;
    // No texture we can bind. On a level that means a flat-coloured surface and
    // is legitimate; on a character it means a proxy -- the low shadow body --
    // and drawing it untextured puts a grey figure inside Travis.
    return PlayerTextureId(textures, m.texName) != 0;
}

void PlayerModel::Advance(float dt) {
    for (auto &obj : objects)
        if (auto *clump =
                dynamic_cast<ClimaxEngine::SG::CClumpObject *>(obj.get()))
            clump->animTime += dt;
}

// True when `clip` was authored for `skel`: the same number of animated bones.
//
// The container carries clips for every skeleton it ships, and SetMatrixAndDraw
// makes the same test before using one -- a 53-track clip driven through a
// nine-bone arm animates a few bones, leaves the rest at rest, and stretches
// whatever spans the two into spikes.
static bool ClipFits(const Skeleton &skel, const AnimClip &clip) {
    int tracks = 0;
    for (const Bone &b : skel.bones)
        if (b.trackIndex >= 0)
            tracks++;
    return tracks > 0 && (size_t)tracks == clip.tracks.size();
}

int PlayerModel::FindClip(const std::string &name) const {
    for (size_t i = 0; i < usableClips.size(); ++i)
        if (clips[(size_t)usableClips[i]].name == name)
            return (int)i;
    return -1;
}

std::string PlayerModel::PlayClipAt(int i) {
    currentClip = -1;
    for (auto &obj : objects)
        if (auto *c = dynamic_cast<ClimaxEngine::SG::CClumpObject *>(obj.get()))
            c->animClip = AnimClip{};   // back to the rest pose

    if (i < 0 || i >= (int)usableClips.size())
        return "rest pose";

    const AnimClip &clip = clips[(size_t)usableClips[(size_t)i]];
    for (auto &obj : objects) {
        auto *c = dynamic_cast<ClimaxEngine::SG::CClumpObject *>(obj.get());
        if (c && ClipFits(c->skeleton, clip)) {
            c->animClip = clip;
            c->animTime = 0.0f;
        }
    }
    currentClip = i;
    return clip.name.empty() ? "unnamed" : clip.name;
}

std::string PlayerModel::CycleClip(int delta) {
    if (usableClips.empty())
        return "no clips fit this skeleton";
    const int n = (int)usableClips.size();
    return PlayClipAt(((currentClip + delta) % n + n) % n);
}


bool LoadPlayerModel(const std::string &entryName) {
    auto *arc = ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance()
                    .GetFirstArchive();
    if (!arc)
        return false;

    const int playerIdx = arc->Find(entryName);
    if (playerIdx < 0) {
        std::cerr << "[player] no archive entry '" << entryName << "'\n";
        return false;
    }

    // Remember the level so it can be put back. If it came from a loose file it
    // has no archive entry, and reloading would destroy it with nothing to
    // restore -- so refuse instead.
    const std::string levelName = g_CurrentMeshContainer;
    const int levelIdx = levelName.empty() ? -1 : arc->Find(levelName);
    if (!levelName.empty() && levelIdx < 0) {
        std::cerr << "[player] current container '" << levelName
                  << "' is not in the archive; not reloading over it\n";
        return false;
    }

    ReleasePlayerModel();
    if (!LoadLevelFromArc(playerIdx))
        return false;

    // ── Retain the scene objects ─────────────────────────────────────────────
    // shared_ptr, so holding one keeps it alive past registrar.Clear(). Their
    // GPU buffers live in the render registry, which only releases at shutdown.
    auto &registrar = ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance();
    for (auto &obj : registrar.GetObjects())
        if (!obj->GetMeshes().empty())
            g_Player.objects.push_back(obj);

    if (g_Player.objects.empty()) {
        std::cerr << "[player] '" << entryName << "' yielded no geometry\n";
        if (levelIdx >= 0)
            LoadLevelFromArc(levelIdx);
        return false;
    }

    g_Player.clips = g_AnimClips;

    // ── Take ownership of its textures ───────────────────────────────────────
    for (auto &obj : g_Player.objects)
        for (auto *m : obj->GetMeshes())
            for (const std::string &n : {m->texName, m->altTexName}) {
                if (n.empty())
                    continue;
                if (const GLuint id = PlayerTextureId(g_TextureMap, n))
                    g_Player.textures[n] = id;
            }
    // Every key naming one of those textures has to go, not only the ones
    // spelled the way the material spells them: the decoder registers upper
    // and lower case aliases too, and the next load deletes by unique id, so a
    // single surviving alias would take the texture down with it.
    {
        std::set<GLuint> keep;
        for (const auto &[name, id] : g_Player.textures)
            keep.insert(id);
        for (auto it = g_TextureMap.begin(); it != g_TextureMap.end();)
            it = keep.count(it->second) ? g_TextureMap.erase(it) : std::next(it);
    }

    // ── Measure the body ─────────────────────────────────────────────────────
    // Effect sheets excluded, or the height comes out as the width of the
    // biggest blank instead of Travis.
    g_Player.boundsMin = glm::vec3(1e9f);
    g_Player.boundsMax = glm::vec3(-1e9f);
    size_t effects = 0;
    for (auto &obj : g_Player.objects)
        for (auto *m : obj->GetMeshes()) {
            if (!IsPlayerBodyMesh(*m, g_Player.textures)) {
                effects++;
                continue;
            }
            for (const Vertex &v : m->vertices) {
                g_Player.boundsMin = glm::min(g_Player.boundsMin, v.pos);
                g_Player.boundsMax = glm::max(g_Player.boundsMax, v.pos);
            }
        }

    g_Player.loaded = true;

    size_t meshes = 0;
    for (auto &obj : g_Player.objects)
        meshes += obj->GetMeshes().size();
    std::cerr << "[player] '" << entryName << "': " << g_Player.objects.size()
              << " objects, " << meshes << " meshes (" << effects
              << " effect sheets skipped), " << g_Player.textures.size()
              << " textures, " << g_Player.clips.size() << " clips, "
              << g_Player.Height() << " units tall\n";

    // Which clips this body can actually take. Naming them is hopeless -- the
    // container calls them Clip_1 upwards -- so they are selected by fit, and
    // the caller cycles through what is left to find the idle by eye.
    for (size_t ci = 0; ci < g_Player.clips.size(); ++ci)
        for (auto &obj : g_Player.objects)
            if (auto *c = dynamic_cast<ClimaxEngine::SG::CClumpObject *>(obj.get()))
                if (ClipFits(c->skeleton, g_Player.clips[ci])) {
                    g_Player.usableClips.push_back((int)ci);
                    break;
                }

    std::cerr << "[player] " << g_Player.usableClips.size() << " of "
              << g_Player.clips.size() << " clips fit this body\n";
    // 35 is the calm breathing stand; 27 is the exhausted idle, kept for when
    // there is a stamina state to drive it.
    std::cerr << "[player] pieces (name / frame / weights / verts):\n";
    for (auto &obj : g_Player.objects)
        for (auto *m : obj->GetMeshes())
            std::cerr << "[player]   " << (m->texName.empty() ? "-" : m->texName)
                      << "  frame=" << m->frameIndex
                      << "  weights=" << (m->hasWeights ? "yes" : "no")
                      << "  verts=" << m->vertices.size()
                      << "  tex="
                      << (PlayerTextureId(g_Player.textures, m->texName) ? "yes" : "NO")
                      << (IsPlayerBodyMesh(*m, g_Player.textures) ? "" : "   [skipped]")
                      << "\n";

    // By their authored names now that the section header gives them up. The
    // numbered fallbacks stay for a container whose names do not parse -- and
    // are what the numbers in ANIMATION_SPEC.md refer to.
    struct Want { int *slot; const char *real; const char *fallback; };
    const Want wants[] = {
        {&g_Player.idleClip,  "PC_TG_Idle.anm",        "Clip_35"},
        {&g_Player.walkClip,  "PC_TG_Walk.anm",        "Clip_29"},
        {&g_Player.runClip,   "PC_TG_Run.anm",         "Clip_28"},
        {&g_Player.tiredClip, "PC_TG_Pained_Idle.anm", "Clip_27"},
    };
    for (const Want &w : wants) {
        *w.slot = g_Player.FindClip(w.real);
        if (*w.slot < 0)
            *w.slot = g_Player.FindClip(w.fallback);
    }
    std::cerr << "[player] idle " << g_Player.idleClip << "  walk "
              << g_Player.walkClip << "  run " << g_Player.runClip << "\n";

    if (g_Player.idleClip >= 0)
        s_lastClipName = g_Player.PlayClipAt(g_Player.idleClip);
    else if (!g_Player.usableClips.empty())
        s_lastClipName = g_Player.PlayClipAt(0);
    std::cerr << "[player] playing '" << s_lastClipName << "'\n";

    if (levelIdx >= 0)
        LoadLevelFromArc(levelIdx);
    return true;
}

void ReleasePlayerModel() {
    for (auto &[name, id] : g_Player.textures)
        if (id)
            glDeleteTextures(1, &id);
    g_Player.textures.clear();
    g_Player.objects.clear();
    g_Player.clips.clear();
    g_Player.usableClips.clear();
    g_Player.currentClip = -1;
    g_Player.loaded = false;
}
