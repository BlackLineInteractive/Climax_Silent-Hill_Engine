#include "ClimaxEngine/Game/CameraLinks.h"

namespace ClimaxEngine {
namespace Game {

int FindCameraByName(const std::vector<LevelCamera> &cameras,
                     const std::string &name) {
  if (name.empty())
    return -1;
  for (size_t i = 0; i < cameras.size(); ++i)
    if (cameras[i].name == name || cameras[i].altName == name)
      return (int)i;
  return -1;
}

std::vector<CameraSwitch> BuildCameraSwitches(
    const std::vector<GameObject> &objects,
    const std::vector<LevelCamera> &cameras) {
  std::vector<CameraSwitch> out;
  for (const GameObject &go : objects) {
    // The class is spelled without a leading C in the archive, unlike most
    // others -- "PlaneTrigger", not "CPlaneTrigger".
    if (go.className != "PlaneTrigger")
      continue;
    // A trigger that names no camera is one of the other PlaneTrigger uses
    // (Ghost Rider has FillInMessage overloads for several), not a camera cut.
    if (go.objName.empty() || go.linkNames.empty())
      continue;

    CameraSwitch sw;
    sw.position = go.position;
    sw.transform = go.transform;
    sw.nameA = go.objName;
    sw.nameB = go.linkNames.front();
    sw.cameraA = FindCameraByName(cameras, sw.nameA);
    sw.cameraB = FindCameraByName(cameras, sw.nameB);
    out.push_back(std::move(sw));
  }
  return out;
}

} // namespace Game
} // namespace ClimaxEngine
