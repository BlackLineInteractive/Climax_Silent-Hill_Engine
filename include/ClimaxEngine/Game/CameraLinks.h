#pragma once

#include <string>
#include <vector>

#include "ClimaxEngine/Core/Types.h"

// Level logic recovered from the object graph, with no dependency on the
// viewer: no ImGui, no AppState, no render types. This is the part meant to
// survive into a native port, so it must stay usable without the tool around
// it.
//
// RenderWare Studio wires objects together by name. A PlaneTrigger names two
// cameras -- one per crossing direction -- and each camera publishes the name
// it answers to. That is the mechanism behind Silent Hill's hard camera cuts
// on stairs and in corridors.
namespace ClimaxEngine {
namespace Game {

// One plane that swaps the active camera when the player crosses it.
struct CameraSwitch {
  glm::vec3 position = glm::vec3(0.0f); // the trigger's own placement
  glm::mat4 transform = glm::mat4(1.0f);
  std::string nameA; // camera named first
  std::string nameB; // camera named second
  int cameraA = -1;  // index into g_Cameras, -1 if the name resolves to nothing
  int cameraB = -1;
};

// Resolves every PlaneTrigger in the level against the cameras it names.
// Triggers whose names match nothing are still returned, with -1 indices, so a
// caller can report them rather than silently dropping level logic.
std::vector<CameraSwitch> BuildCameraSwitches(
    const std::vector<GameObject> &objects,
    const std::vector<LevelCamera> &cameras);

// Index of the camera with this exact name, or -1.
int FindCameraByName(const std::vector<LevelCamera> &cameras,
                     const std::string &name);

// Where a camera points with the player standing at `subject`.
//
// The two fixed-camera classes behave differently and the archive says so.
// CStaticCamera really is fixed, and the direction in its placement matrix is
// usually the right one: measured across all 473 instances against each level's
// own walkable markers, the median error is 4.5 degrees of yaw and 3.6 of
// pitch, and on 350 of 471 the authored aim holds the play space in frame. The
// remaining quarter is not a decoding failure but real data -- 29 levels give
// every camera the same default rotation -- so the authored direction is used
// only while it still holds the subject.
//
// CConstraintCamera is not fixed at all. 400 of its 417 instances carry no
// pitch, while the aim needed to see the floor from where they hang averages 11
// degrees down -- so its vertical aim is computed while the game runs, which is
// what "constraint camera" means. Its 21-property table holds the real tracking
// limits and none of them are decoded yet, so here it simply follows the
// player, stopped only from tipping further than a camera plausibly would.
glm::vec3 CameraAim(const LevelCamera &cam, const glm::vec3 &eye,
                    const glm::vec3 &subject);

// The eye position and look direction to render a camera with, given where the
// player is and what the level is built of.
//
// A fixed camera is routinely placed behind the room it films -- camEntrance in
// HO_1_Hallway1 stands at z = 3.20 while the floor stops at z = 2.00. From
// there a part of the frustum falls outside the level's geometry entirely and
// shows the void past the floor and the wall. So the eye is stepped through the
// last surface between it and the player, putting it in the same room he is in.
// How far it moves comes from the collision mesh, so a thick wall moves it
// further than a thin one without anything being tuned.
void ResolveCameraView(const CollisionMesh &world, const LevelCamera &cam,
                       const glm::vec3 &subject, glm::vec3 &eyeOut,
                       glm::vec3 &lookOut);

// Tracks which side of each switch plane the player is on, and reports the
// camera to cut to when one is crossed.
//
// This is the mechanism behind Silent Hill's hard camera changes: a plane in
// the level names one camera for each side, and crossing it hands over. Kept
// here rather than in the viewer because it is game behaviour, not a tool
// feature.
class CameraSwitcher {
public:
    // Rebuilt whenever the level changes.
    void Reset(std::vector<CameraSwitch> switches);

    // Returns the camera index to become active, or -1 when nothing changed.
    // The first call after Reset only records which side the player starts on,
    // so entering a level does not count as a crossing.
    int Update(const glm::vec3 &playerPos);

    const std::vector<CameraSwitch> &Switches() const { return m_switches; }

private:
    std::vector<CameraSwitch> m_switches;
    std::vector<float> m_side;   // last signed distance, one per switch
    bool m_primed = false;
};

} // namespace Game
} // namespace ClimaxEngine
