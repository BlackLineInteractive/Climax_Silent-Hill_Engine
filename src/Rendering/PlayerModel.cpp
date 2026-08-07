#include "ClimaxEngine/Render/PlayerModel.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>

#include "ClimaxEngine/Core/RWS/FileSystem/CArchiveManager.h"
#include "ClimaxEngine/Loader/Loader.h"
#include "ClimaxEngine/Render/ViewerState.h"

PlayerModel g_Player;

bool IsPlayerEffectMesh(const MeshChunk &m) {
    // Character containers ship effect sheets -- muzzle flashes, blood, the
    // shadow blob -- as full-size quads that the game scales down when it
    // spawns them. Drawn as authored they are metres across, which is the
    // white-and-red sheet lying under Travis's feet.
    //
    // Told apart by shape rather than by name: a body mesh is a shell of many
    // triangles, a blank is a single large quad. Six vertices and a span wider
    // than a person is not something a character is made of.
    if (m.vertices.size() > 12)
        return false;

    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const Vertex &v : m.vertices) {
        lo = glm::min(lo, v.pos);
        hi = glm::max(hi, v.pos);
    }
    const glm::vec3 span = hi - lo;
    return std::max(span.x, std::max(span.y, span.z)) > 1.0f;
}

void PlayerModel::Advance(float dt) {
    for (auto &obj : objects)
        if (auto *clump =
                dynamic_cast<ClimaxEngine::SG::CClumpObject *>(obj.get()))
            clump->animTime += dt;
}

int PlayerModel::PlayClip(const std::string &name) {
    std::string want = name;
    std::transform(want.begin(), want.end(), want.begin(), ::tolower);

    const AnimClip *pick = nullptr;
    for (const AnimClip &c : clips) {
        std::string n = c.name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (n.find(want) != std::string::npos) {
            pick = &c;
            break;
        }
    }
    if (!pick)
        return 0;

    int bound = 0;
    for (auto &obj : objects) {
        auto *clump = dynamic_cast<ClimaxEngine::SG::CClumpObject *>(obj.get());
        if (!clump)
            continue;
        // A clip belongs to the skeleton it was authored for. The container
        // ships clips for every skeleton it carries, and driving a 53-track
        // clip through a 9-bone arm stretches whatever spans the two into
        // spikes -- the same test SetMatrixAndDraw makes before using one.
        int tracks = 0;
        for (const Bone &b : clump->skeleton.bones)
            if (b.trackIndex >= 0)
                tracks++;
        if ((size_t)tracks != pick->tracks.size())
            continue;
        clump->animClip = *pick;
        clump->animTime = 0.0f;
        bound++;
    }
    return bound;
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
        for (auto *m : obj->GetMeshes()) {
            auto it = g_TextureMap.find(m->texName);
            if (it != g_TextureMap.end())
                g_Player.textures[m->texName] = it->second;
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
            if (IsPlayerEffectMesh(*m)) {
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

    // Standing still is still animation: without a clip the skeleton stays in
    // its authored rest pose, which for these models is arms straight out.
    for (const char *name : {"idle", "stand", "wait", "breath"}) {
        const int bound = g_Player.PlayClip(name);
        if (bound > 0) {
            std::cerr << "[player] idle clip '" << name << "' on " << bound
                      << " clump(s)\n";
            break;
        }
    }

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
    g_Player.loaded = false;
}
