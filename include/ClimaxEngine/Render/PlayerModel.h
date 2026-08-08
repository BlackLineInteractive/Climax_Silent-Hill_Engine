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

    // Travis is not one clump. The container ships seven skeletons, and the
    // face is its own object -- which is why it hung in the air: every object
    // was placed at the player's feet, so the head sat where the hips are and
    // never moved, while the body walked away underneath it.
    //
    // Index into `objects` of the clump with the most geometry: the body.
    int bodyObject = -1;
    // For every other object, the body bone it should ride, chosen at load by
    // nearest rest position. -1 leaves it at the player transform.
    std::vector<int> attachBone;
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

    // Indices into `clips` that fit at least one clump's skeleton. The
    // container ships clips for all seven of its skeletons, and one authored
    // for a nine-bone arm drives a full body into a knot, so the rest are not
    // offered at all.
    std::vector<int> usableClips;
    int currentClip = -1;   // index into usableClips, -1 for the rest pose

    // The clips this body moves with, resolved once at load. The container
    // names them Clip_N with no hint of what they hold, so these came from
    // watching all 118 that fit: 27 is the idle sway, 29 the walk, 28 the run.
    // Identified for CPlayerBehaviour.Travis; another character will number
    // them differently, which is why they are looked up rather than assumed.
    int idleClip = -1;
    int walkClip = -1;
    int runClip  = -1;
    int tiredClip = -1;   // the exhausted idle, unused until stamina exists

    // Index into usableClips of the clip named `name`, or -1.
    int FindClip(const std::string &name) const;

    // Binds usableClips[i]. Pass -1 for the rest pose. Returns the clip's name.
    std::string PlayClipAt(int i);
    // Steps through usableClips by `delta`, wrapping. Returns the new name.
    std::string CycleClip(int delta);
};

extern PlayerModel g_Player;

// True for a mesh that is one of the container's full-size particle blanks
// rather than part of the body. The game scales these at runtime; drawn as
// authored they are floor-wide quads, which is the sheet under Travis's feet.
bool IsPlayerEffectMesh(const MeshChunk &m);

// True for a piece that belongs on screen as part of the character.
//
// Three things in the container are not Travis: the effect sheets above, any
// material whose texture cannot be resolved (a body piece always has one --
// an unresolvable piece renders as flat grey, which is the silhouette that
// shows through him from inside), and the FX_ sheets the game spawns as
// particles rather than wearing.
bool IsPlayerBodyMesh(const MeshChunk &m,
                      const std::map<std::string, GLuint> &textures);

// Texture id for `name`, trying the spelling as given and its upper- and
// lower-case aliases, the way the level renderer does.
GLuint PlayerTextureId(const std::map<std::string, GLuint> &from,
                       const std::string &name);

// Loads `entryName` out of the mounted archive and keeps it, then restores
// whatever level was loaded before. Costs a full level reload, so call once.
bool LoadPlayerModel(const std::string &entryName);

// Frees the textures the model owns and drops its objects.
void ReleasePlayerModel();
