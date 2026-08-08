#pragma once

#include <string>
#include <vector>

#include "ClimaxEngine/Core/Types.h"

// Movement between level containers, recovered from the object graph. Game
// logic, not viewer logic: no ImGui, no GL, no ViewerState.
//
// Silent Hill's levels are separate containers in SH.ARC, and a door is a
// ZoneTrigger box that names the container on the other side. Read from
// HO_1_Hallway1:
//
//   ZoneTrigger  prop 0 = "HO_1_WomensRoom"   where it leads
//                prop 1 = "HO_1_Hallway1"     where it is
//                prop 2 = "goZoneWomensRoom"  the event it sends
//                prop 3 = "MSG_PAD_GRAB"      the button that sends it
//                prop 15 = "DO_UIDWO"         the door's sound set
//
// and the destination container answers with a CPlayerSpawner per neighbour,
// each named after the zone the player is arriving *from*:
//
//   CPlayerSpawner  CBaseBehaviour prop 0 = "HO_1_Lobby"
//
// so walking out of the Lobby door puts Travis on the spawner called
// "HO_1_Lobby", facing the way that spawner faces. Nothing here is guessed:
// the three triggers and three spawners of HO_1_Hallway1 pair up exactly.
namespace ClimaxEngine {
namespace Game {

// One doorway.
struct ZoneLink {
  std::string toZone;    // container to load
  std::string fromZone;  // container this trigger sits in
  std::string eventName; // what it broadcasts when used
  // Not a UI hint, as first assumed: DO_UIDWO is an archive entry named
  // "Door_UIDoorWoodO" holding a CAudioTransitions object and a wave dictionary
  // -- the sound a wooden door makes. Unused until doors make noise.
  std::string sound;
  glm::vec3 position = glm::vec3(0.0f);
  glm::mat4 transform = glm::mat4(1.0f); // volume: rows are half-extent axes
};

// Every ZoneTrigger in the level, in file order.
std::vector<ZoneLink> BuildZoneLinks(const std::vector<GameObject> &objects);

// Index of the link whose volume contains `p`, or -1 when the player is not
// standing in a doorway.
//
// `reach` grows every box by that many units. A door the player has not opened
// is still solid in the collision mesh, so the trigger box behind it can be
// unreachable by a body that is stopped at the door plane -- the reach is what
// lets him use it from where he can actually stand.
int ZoneLinkAt(const std::vector<ZoneLink> &links, const glm::vec3 &p,
               float reach = 0.0f);

// Where the player appears on entering a level from `fromZone`.
//
// Falls back to any spawner when the name does not match, which is what
// happens on the first level opened -- nobody arrived from anywhere. Returns
// false only when the container has no CPlayerSpawner at all, which is true of
// several test and prop containers.
bool FindZoneSpawn(const std::vector<GameObject> &objects,
                   const std::string &fromZone, glm::vec3 &posOut,
                   glm::vec3 &facingOut);

} // namespace Game
} // namespace ClimaxEngine
