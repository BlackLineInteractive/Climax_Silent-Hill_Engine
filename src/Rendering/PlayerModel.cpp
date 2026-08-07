#include "ClimaxEngine/Render/PlayerModel.h"

#include <iostream>
#include <set>

#include "ClimaxEngine/Core/RWS/FileSystem/CArchiveManager.h"
#include "ClimaxEngine/Loader/Loader.h"
#include "ClimaxEngine/Render/ViewerState.h"
#include "ClimaxEngine/SG/SceneObject.h"

PlayerModel g_Player;

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

    // Remember the level so it can be put back. Its own entry may be gone from
    // the archive if the user opened a loose file, in which case the level is
    // lost and the caller has to reload -- so refuse rather than destroy it.
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

    // ── Lift the meshes out ──────────────────────────────────────────────────
    // Copying the chunk copies its gpuMesh handle, and the render registry only
    // releases buffers at shutdown, so the copy keeps drawing after the
    // registrar that produced it is cleared.
    auto &registrar = ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance();
    for (auto &obj : registrar.GetObjects())
        for (auto *chunk : obj->GetMeshes())
            if (!chunk->vertices.empty())
                g_Player.meshes.push_back(*chunk);

    if (g_Player.meshes.empty()) {
        std::cerr << "[player] '" << entryName << "' yielded no geometry\n";
        if (levelIdx >= 0)
            LoadLevelFromArc(levelIdx);
        return false;
    }

    // ── Take ownership of its textures ───────────────────────────────────────
    for (const MeshChunk &m : g_Player.meshes) {
        auto it = g_TextureMap.find(m.texName);
        if (it != g_TextureMap.end())
            g_Player.textures[m.texName] = it->second;
    }
    // Every key that names one of those textures has to go, not just the ones
    // spelled the way the material spells them: the decoder registers upper and
    // lower case aliases too, and the next load deletes by unique id, so one
    // surviving alias would take the texture down with it.
    {
        std::set<GLuint> keep;
        for (const auto &[name, id] : g_Player.textures)
            keep.insert(id);
        for (auto it = g_TextureMap.begin(); it != g_TextureMap.end();)
            it = keep.count(it->second) ? g_TextureMap.erase(it) : std::next(it);
    }

    // ── Measure it, so the caller can stand it on the floor ──────────────────
    g_Player.boundsMin = glm::vec3(1e9f);
    g_Player.boundsMax = glm::vec3(-1e9f);
    for (const MeshChunk &m : g_Player.meshes)
        for (const Vertex &v : m.vertices) {
            g_Player.boundsMin = glm::min(g_Player.boundsMin, v.pos);
            g_Player.boundsMax = glm::max(g_Player.boundsMax, v.pos);
        }

    g_Player.loaded = true;
    std::cerr << "[player] '" << entryName << "': " << g_Player.meshes.size()
              << " meshes, " << g_Player.textures.size() << " textures, "
              << g_Player.Height() << " units tall\n";

    if (levelIdx >= 0)
        LoadLevelFromArc(levelIdx);
    return true;
}

void ReleasePlayerModel() {
    for (auto &[name, id] : g_Player.textures)
        if (id)
            glDeleteTextures(1, &id);
    g_Player.textures.clear();
    g_Player.meshes.clear();
    g_Player.loaded = false;
}
