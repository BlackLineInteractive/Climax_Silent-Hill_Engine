#include "ClimaxEngine/Game/CharacterController.h"

#include <algorithm>
#include <cmath>

namespace ClimaxEngine {
namespace Game {

glm::vec3 ClosestPointOnTriangle(const glm::vec3 &p, const glm::vec3 &a,
                                 const glm::vec3 &b, const glm::vec3 &c) {
    // Ericson's barycentric region test: check the three vertex regions, then
    // the three edge regions, and fall through to the face. Cheaper and far
    // more robust near edges than projecting onto the plane and clamping.
    const glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        return a + ab * (d1 / (d1 - d3));

    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        return a + ac * (d2 / (d2 - d6));

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));

    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

float RayDown(const CollisionMesh &world, const glm::vec3 &origin,
              float maxDist, glm::vec3 *normalOut) {
    float best = -1.0f;
    for (size_t i = 0; i + 2 < world.indices.size(); i += 3) {
        const glm::vec3 &a = world.verts[world.indices[i]];
        const glm::vec3 &b = world.verts[world.indices[i + 1]];
        const glm::vec3 &c = world.verts[world.indices[i + 2]];

        glm::vec3 n = glm::cross(b - a, c - a);
        const float len = glm::length(n);
        if (len < 1e-8f) continue;   // degenerate face, ignore
        n /= len;

        // Only floors can be stood on, and a downward ray cannot meaningfully
        // hit a face pointing away from it.
        if (std::abs(n.y) < 1e-4f) continue;

        const float t = (glm::dot(n, a) - glm::dot(n, origin)) / -n.y;
        if (t < 0.0f || t > maxDist) continue;

        const glm::vec3 hit(origin.x, origin.y - t, origin.z);
        // Inside test by barycentric containment, done as a closest-point
        // check so edge cases behave rather than flickering.
        if (glm::length(ClosestPointOnTriangle(hit, a, b, c) - hit) > 1e-3f)
            continue;

        if (best < 0.0f || t < best) {
            best = t;
            if (normalOut) *normalOut = n.y < 0.0f ? -n : n;
        }
    }
    return best;
}

// Pushes the body out of anything it overlaps, and reports the surface that
// resisted most so the caller can tell floor from wall.
static void Depenetrate(const CollisionMesh &world, glm::vec3 &pos, float radius,
                        glm::vec3 &bestFloor, bool &touchedFloor, float maxSlope) {
    // Several passes because resolving one contact can push the body into
    // another; a handful is plenty for geometry this coarse and it keeps the
    // step bounded rather than looping until convergence.
    for (int pass = 0; pass < 4; ++pass) {
        bool moved = false;
        for (size_t i = 0; i + 2 < world.indices.size(); i += 3) {
            const glm::vec3 &a = world.verts[world.indices[i]];
            const glm::vec3 &b = world.verts[world.indices[i + 1]];
            const glm::vec3 &c = world.verts[world.indices[i + 2]];

            const glm::vec3 closest = ClosestPointOnTriangle(pos, a, b, c);
            glm::vec3 away = pos - closest;
            const float dist = glm::length(away);
            if (dist >= radius || dist < 1e-6f) continue;

            away /= dist;
            pos += away * (radius - dist);
            moved = true;

            if (away.y > maxSlope) {
                touchedFloor = true;
                if (away.y > bestFloor.y) bestFloor = away;
            }
        }
        if (!moved) break;
    }
}

void CharacterController::Step(const CollisionMesh &world, const glm::vec3 &move,
                               float dt) {
    if (world.indices.empty()) {
        position += move;
        return;
    }

    velocity.y += gravity * dt;
    // Falling speed is capped so a long drop cannot tunnel through a floor in
    // one step; with discrete depenetration that is the only failure mode that
    // loses the player entirely.
    velocity.y = std::max(velocity.y, -40.0f);

    // Horizontal first, then vertical. Separating them is what makes a wall
    // stop movement without also killing the fall, and a floor stop the fall
    // without also gluing the body to the wall it is brushing.
    position += glm::vec3(move.x, 0.0f, move.z);

    glm::vec3 floorNormal(0.0f);
    bool hitFloor = false;
    Depenetrate(world, position, radius, floorNormal, hitFloor, maxSlope);

    position.y += velocity.y * dt;

    floorNormal = glm::vec3(0.0f);
    hitFloor = false;
    Depenetrate(world, position, radius, floorNormal, hitFloor, maxSlope);

    grounded = hitFloor;
    if (grounded) {
        groundNormal = glm::normalize(floorNormal);
        if (velocity.y < 0.0f) velocity.y = 0.0f;
    }
}

bool CharacterController::SnapToGround(const CollisionMesh &world, float maxDrop) {
    if (world.indices.empty()) return false;
    // Start the probe above the body: a spawn point often sits a little inside
    // the floor, and a ray cast from inside a surface finds nothing.
    const glm::vec3 from = position + glm::vec3(0.0f, radius * 2.0f, 0.0f);
    glm::vec3 n;
    const float t = RayDown(world, from, maxDrop + radius * 2.0f, &n);
    if (t < 0.0f) return false;
    position = glm::vec3(from.x, from.y - t + radius, from.z);
    groundNormal = n;
    grounded = true;
    velocity = glm::vec3(0.0f);
    return true;
}

bool HasLineOfSight(const CollisionMesh &world, const glm::vec3 &from,
                    const glm::vec3 &to) {
    const glm::vec3 dir = to - from;
    const float dist = glm::length(dir);
    if (dist < 1e-4f) return true;
    const glm::vec3 d = dir / dist;

    // Moller-Trumbore, two-sided: collision faces have no reliable winding for
    // this purpose and a wall blocks sight from either face.
    for (size_t i = 0; i + 2 < world.indices.size(); i += 3) {
        const glm::vec3 &a = world.verts[world.indices[i]];
        const glm::vec3 &b = world.verts[world.indices[i + 1]];
        const glm::vec3 &c = world.verts[world.indices[i + 2]];

        const glm::vec3 e1 = b - a, e2 = c - a;
        const glm::vec3 h = glm::cross(d, e2);
        const float det = glm::dot(e1, h);
        if (std::abs(det) < 1e-7f) continue;

        const float inv = 1.0f / det;
        const glm::vec3 s = from - a;
        const float u = glm::dot(s, h) * inv;
        if (u < 0.0f || u > 1.0f) continue;

        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(d, q) * inv;
        if (v < 0.0f || u + v > 1.0f) continue;

        const float t = glm::dot(e2, q) * inv;
        // Small bias at both ends so a camera resting against a surface, or a
        // body standing on the floor, does not block its own line.
        if (t > 0.05f && t < dist - 0.05f) return false;
    }
    return true;
}

bool FindPlayerSpawn(const std::vector<GameObject> &objects, glm::vec3 &out) {
    for (const GameObject &go : objects) {
        if (go.className != "CPlayerSpawner" || go.atOrigin) continue;
        out = go.position;
        return true;
    }
    return false;
}

} // namespace Game
} // namespace ClimaxEngine
