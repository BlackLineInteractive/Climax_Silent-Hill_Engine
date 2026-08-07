#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClimaxEngine/Render/PlayerModel.h  —  Travis, kept alive across level loads
//
// The loader owns one container at a time: LoadLevelData clears the scene
// registrar and deletes every texture before it reads the next file. That is
// fine for a viewer and useless for a game, where the player has to outlive the
// room he walks out of.
//
// Until the loader can hold two containers at once, this takes the cheap route:
// load the player container, lift its meshes and textures out of the globals
// before the next load can free them, then put the level back. The GPU buffers
// are never touched, so the copies stay drawable.
// ─────────────────────────────────────────────────────────────────────────────

#include <GL/glew.h>
#include <map>
#include <string>
#include <vector>

#include "ClimaxEngine/Core/Types.h"

struct PlayerModel {
    std::vector<MeshChunk>        meshes;
    std::map<std::string, GLuint> textures;  // owned here, not by g_TextureMap
    bool      loaded = false;
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);

    // Distance from the model's feet to the top of its head, in level units.
    float Height() const { return boundsMax.y - boundsMin.y; }
};

extern PlayerModel g_Player;

// Loads `entryName` out of the mounted archive and keeps it, then restores
// whatever level was loaded before. Returns false when the archive has no such
// entry or the container yields no geometry.
//
// Costs a full level reload, so it is meant to be called once.
bool LoadPlayerModel(const std::string &entryName);

// Frees the textures the model owns. The mesh buffers live in the GPU registry
// and go with it.
void ReleasePlayerModel();
