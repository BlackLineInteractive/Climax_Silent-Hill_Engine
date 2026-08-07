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
// load the player container, take its scene objects and textures out of the
// globals before the next load frees them, then put the level back.
//
// The scene objects are kept whole rather than copied mesh by mesh, and that is
// the difference between a statue and a character: a CClumpObject carries its
// skeleton, its clip and its posing code, so setting its transform each frame
// is all that animation needs. The registrar holds shared_ptr, so retaining one
// costs nothing and survives Clear().
// ─────────────────────────────────────────────────────────────────────────────

#include <GL/glew.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ClimaxEngine/Core/Types.h"
#include "ClimaxEngine/SG/SceneObject.h"

struct PlayerModel {
    std::vector<std::shared_ptr<ClimaxEngine::SG::CSceneObject>> objects;
    std::map<std::string, GLuint> textures; // owned here, not by g_TextureMap
    std::vector<AnimClip> clips;            // the container's own animations

    bool loaded = false;
    // Bounds of the body alone, with the oversized effect sheets left out --
    // see PlayerModel.cpp for why they have to be.
    glm::vec3 boundsMin = glm::vec3(0.0f);
    glm::vec3 boundsMax = glm::vec3(0.0f);

    float Height() const { return boundsMax.y - boundsMin.y; }

    // Drives every clump's animTime. Clips loop on their own duration.
    void Advance(float dt);

    // Binds `name` (case-insensitive substring) to every clump whose skeleton
    // it fits. Returns the number of clumps that took it.
    int PlayClip(const std::string &name);
};

extern PlayerModel g_Player;

// True for a mesh that is one of the container's full-size particle blanks
// rather than part of the body. The game scales these at runtime; drawn as
// authored they are floor-wide quads, which is the sheet under Travis's feet.
bool IsPlayerEffectMesh(const MeshChunk &m);

// Loads `entryName` out of the mounted archive and keeps it, then restores
// whatever level was loaded before. Costs a full level reload, so call once.
bool LoadPlayerModel(const std::string &entryName);

// Frees the textures the model owns and drops its objects.
void ReleasePlayerModel();
