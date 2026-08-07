#pragma once

#include <string>
#include <vector>

#include "ClimaxEngine/Core/Common.h"

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
