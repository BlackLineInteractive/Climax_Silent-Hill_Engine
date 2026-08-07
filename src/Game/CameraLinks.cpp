#include "ClimaxEngine/Game/CameraLinks.h"

#include "ClimaxEngine/Game/CharacterController.h"

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

glm::vec3 CameraAim(const LevelCamera &cam, const glm::vec3 &eye,
                    const glm::vec3 &subject) {
  glm::vec3 v = subject - eye;
  if (glm::length(v) < 1e-3f)
    return cam.forward;
  v = glm::normalize(v);

  // A static camera keeps the direction it was given -- but only while that
  // direction still holds the subject. On 350 of 471 instances it does, and
  // there the authored shot is the game's own framing, which is the whole
  // point of this camera system. The other 121 sit in levels where every
  // camera carries the same default rotation -- 29 of the 201 levels with more
  // than one camera are like that, HO_1_Hallway1 among them -- and honouring a
  // default aims the shot at a wall. Falling through to tracking there is the
  // difference between the game's framing and no framing at all.
  if (cam.className == "CStaticCamera") {
    const float halfFov = glm::radians(glm::min(cam.fovDeg * 0.5f, 85.0f));
    if (glm::dot(v, cam.forward) >= cosf(halfFov))
      return cam.forward;
  }

  // Walking directly beneath a camera would otherwise point it at the floor;
  // the game never tilts that far. A stand-in for the real constraint, which
  // lives in properties this does not read yet.
  const float kMaxTilt = glm::radians(55.0f);
  const float minY = -sinf(kMaxTilt);
  if (v.y < minY) {
    glm::vec2 h(v.x, v.z);
    h = (glm::length(h) > 1e-4f)
            ? glm::normalize(h) * cosf(kMaxTilt)
            : glm::normalize(glm::vec2(cam.forward.x, cam.forward.z)) *
                  cosf(kMaxTilt);
    v = glm::vec3(h.x, minY, h.y);
  }
  return v;
}

void ResolveCameraView(const CollisionMesh &world, const LevelCamera &cam,
                       const glm::vec3 &subject, glm::vec3 &eyeOut,
                       glm::vec3 &lookOut) {
  eyeOut = cam.position;

  const glm::vec3 v = subject - eyeOut;
  const float dist = glm::length(v);
  if (dist > 1e-3f && !world.indices.empty()) {
    const float t = LastBlockerAlong(world, eyeOut, subject);
    // Clear the surface just crossed, and never step so far that the camera
    // lands on top of the player -- a shot needs room to be a shot.
    const float kClear = 0.25f;
    const float kMinShot = 1.5f;
    if (t > 0.0f && t + kClear < dist - kMinShot)
      eyeOut += (v / dist) * (t + kClear);
  }

  lookOut = CameraAim(cam, eyeOut, subject);
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
