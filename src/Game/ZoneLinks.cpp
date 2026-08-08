#include "ClimaxEngine/Game/ZoneLinks.h"

#include <cmath>

namespace ClimaxEngine {
namespace Game {

namespace {

// True when `p` is inside the oriented box the placement matrix describes.
//
// The rows are axis vectors whose lengths are the box's half-extents -- the
// same reading CameraSwitcher already uses for PlaneTrigger. The archive backs
// it: the HO_1_Lobby doorway box reaches z = -2.895 and the spawner the player
// arrives on sits at z = -2.177, just 0.7 m clear of it. Halving the extents
// instead would put the box a metre inside the wall, where nobody could stand.
bool InsideVolume(const glm::mat4 &m, const glm::vec3 &p, float reach) {
  const glm::vec3 rel = p - glm::vec3(m[3]);
  for (int k = 0; k < 3; ++k) {
    const glm::vec3 axis(m[k]);
    const float len = glm::length(axis);
    if (len < 1e-5f)
      continue; // degenerate axis: unconstrained in that direction
    if (std::abs(glm::dot(rel, axis / len)) > len + reach)
      return false;
  }
  return true;
}

} // namespace

std::vector<ZoneLink> BuildZoneLinks(const std::vector<GameObject> &objects) {
  std::vector<ZoneLink> out;
  for (const GameObject &go : objects) {
    // Spelled without the leading C in the archive, like PlaneTrigger.
    if (go.className != "ZoneTrigger")
      continue;
    // A trigger naming only itself leads nowhere; skip rather than produce a
    // door that loads an empty string.
    if (go.objName.empty() || go.linkNames.empty())
      continue;

    ZoneLink z;
    z.toZone = go.objName;
    z.fromZone = go.linkNames.size() > 0 ? go.linkNames[0] : std::string();
    z.eventName = go.linkNames.size() > 1 ? go.linkNames[1] : std::string();
    // linkNames[2] is the button ("MSG_PAD_GRAB"); the prompt id follows it.
    z.prompt = go.linkNames.size() > 3 ? go.linkNames[3] : std::string();
    z.position = go.position;
    z.transform = go.transform;
    out.push_back(std::move(z));
  }
  return out;
}

int ZoneLinkAt(const std::vector<ZoneLink> &links, const glm::vec3 &p,
               float reach) {
  // Nearest wins, so standing between two doorways offers the one in front.
  int best = -1;
  float bestD = 1e9f;
  for (size_t i = 0; i < links.size(); ++i) {
    if (!InsideVolume(links[i].transform, p, reach))
      continue;
    const float d = glm::length(p - glm::vec3(links[i].transform[3]));
    if (d < bestD) {
      bestD = d;
      best = (int)i;
    }
  }
  return best;
}

bool FindZoneSpawn(const std::vector<GameObject> &objects,
                   const std::string &fromZone, glm::vec3 &posOut,
                   glm::vec3 &facingOut) {
  const GameObject *fallback = nullptr;
  for (const GameObject &go : objects) {
    if (go.className != "CPlayerSpawner" || go.atOrigin)
      continue;
    if (!fallback)
      fallback = &go;
    if (!fromZone.empty() && go.objName == fromZone) {
      fallback = &go;
      break;
    }
  }
  if (!fallback)
    return false;

  posOut = fallback->position;
  // Row 2 is the facing direction, the same convention the cameras use.
  const glm::vec3 f(fallback->transform[2]);
  facingOut = glm::length(f) > 1e-4f ? glm::normalize(f) : glm::vec3(0, 0, 1);
  return true;
}

} // namespace Game
} // namespace ClimaxEngine
