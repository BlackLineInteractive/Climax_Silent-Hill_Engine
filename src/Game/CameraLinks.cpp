#include "ClimaxEngine/Game/CameraLinks.h"

#include <cmath>

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

void CameraSwitcher::Reset(std::vector<CameraSwitch> switches) {
    m_switches = std::move(switches);
    m_side.assign(m_switches.size(), 0.0f);
    m_primed = false;
}

int CameraSwitcher::Update(const glm::vec3 &playerPos) {
    int result = -1;
    for (size_t i = 0; i < m_switches.size(); ++i) {
        const CameraSwitch &sw = m_switches[i];

        // The trigger's matrix is not an orientation -- its columns are
        // scaled, and their lengths are the box's half extents. A doorway
        // plane is thin in one axis and wide in the other two, so the normal
        // is the *shortest* column:
        //
        //   axisX (-0.41, 0, 0)   axisY (0, 4.30, 0)   axisZ (0, 0, -5.02)
        //
        // Taking column 2 as the normal, as this did at first, aimed the test
        // along the box's longest axis instead. The plane then sat at z = 5.70
        // while the walkable floor ended at z = 2.0, so nothing could ever
        // cross it and the camera never changed.
        glm::vec3 axis[3] = {glm::vec3(sw.transform[0]), glm::vec3(sw.transform[1]),
                             glm::vec3(sw.transform[2])};
        float ext[3] = {glm::length(axis[0]), glm::length(axis[1]),
                        glm::length(axis[2])};
        int thin = 0;
        for (int k = 1; k < 3; ++k)
            if (ext[k] < ext[thin]) thin = k;
        if (ext[thin] < 1e-5f)
            continue;

        const glm::vec3 n = axis[thin] / ext[thin];
        const glm::vec3 rel = playerPos - sw.position;
        const float d = glm::dot(rel, n);

        // The plane is treated as unbounded on purpose.
        //
        // Bounding it by the other two column lengths was tried and rejected:
        // whether those are half extents or full sizes, the box comes out
        // barely missing the floor the player can actually stand on -- in
        // HO_1_Hallway1 the walkable area ends at z = 2.0 while the box sits
        // around z = 5.7 -- so every crossing was discarded and the camera
        // never changed. Which of the two conventions the game uses is not
        // settled, and guessing it wrong is worse than not testing: these are
        // doorways in small rooms, and one plane per room is the normal case.
        (void)ext;

        // Which side the player is on, as a discrete state rather than a
        // comparison of two raw distances.
        //
        // The first attempt required the previous distance above +0.05 and the
        // current below -0.05. At walking pace that distance changes by about
        // 0.05 per frame, so a crossing routinely went +0.03 -> -0.02 and
        // satisfied neither test: the camera never cut. Committing to a side
        // only once the player is clearly past the plane keeps the hysteresis
        // that stops flicker, without a window a normal step can jump over.
        const float kHysteresis = 0.12f;
        int side = m_side[i] > 0.0f ? 1 : (m_side[i] < 0.0f ? -1 : 0);
        if (d > kHysteresis)
            side = 1;
        else if (d < -kHysteresis)
            side = -1;

        if (m_primed && side != 0) {
            const int prev = m_side[i] > 0.0f ? 1 : (m_side[i] < 0.0f ? -1 : 0);
            if (prev == 1 && side == -1)
                result = sw.cameraB;
            else if (prev == -1 && side == 1)
                result = sw.cameraA;
        }
        m_side[i] = (float)side;
    }
    m_primed = true;
    return result;
}

} // namespace Game
} // namespace ClimaxEngine
