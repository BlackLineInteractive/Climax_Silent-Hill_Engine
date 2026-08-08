#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClimaxEngine/Core/Types.h  —  pure data types, NO GL, NO SDL, NO ImGui
//
// Every translation unit in climax-core and climax-game includes this header.
// It must never pull in <GL/glew.h>, <SDL*.h>, or any imgui header.
// GPU handles (GLuint) live in Render/GPUMesh.h.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "ClimaxEngine/Platform/PS2/AudioParser.h"

namespace fs = std::filesystem;

// ── Portability ───────────────────────────────────────────────────────────────
#ifdef _MSC_VER
#  include <string.h>
#  define sho_stricmp  _stricmp
#  define sho_strnicmp _strnicmp
#else
#  include <strings.h>
#  define sho_stricmp  strcasecmp
#  define sho_strnicmp strncasecmp
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Geometry / mesh data  (CPU-side only)
// ─────────────────────────────────────────────────────────────────────────────

struct Vertex {
    glm::vec3 pos = glm::vec3(0.0f);
    glm::vec2 uv = glm::vec2(0.0f);
    glm::vec4 color = glm::vec4(1.0f);
    uint8_t boneIds[4] = {0, 0, 0, 0};
    float boneWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct Bone {
  std::string name;
  int parent = -1;
  glm::mat4 restLocal = glm::mat4(1.0f);
  glm::mat4 invBind = glm::mat4(1.0f);
  // From the frame's HAnim PLG (0x011E). `boneId` is the identity the animation
  // data refers to; `trackIndex` is this bone's position in the hierarchy table,
  // which is the order the clip's keyframe tracks come in.
  int boneId = -1;
  int trackIndex = -1;
  // Second field of the HAnim table entry: where this bone sits in the skin's
  // own arrays, which is what the per-vertex slots index.
  int skinIndex = -1;
};

struct AnimTrack {
  std::vector<float> times;
  std::vector<glm::quat> rot;
  std::vector<glm::vec3> pos;
};

struct AnimClip {
  std::string name;
  float duration = 0.0f;
  float fps = 30.0f;
  std::vector<AnimTrack> tracks;
};

struct Skeleton {
  std::vector<Bone> bones;
};

// CPU-side mesh chunk.  GPU handles (vao/vbo) live in Render/GPUMesh.h.
struct MeshChunk {
  std::vector<Vertex> vertices;
  // Opaque handle to this mesh's GPU mirror, -1 until the renderer uploads it.
  // No GLuint and no GL meaning -- just an index the render layer hands out. It
  // lives in the struct rather than in a table keyed by address because a chunk
  // is uploaded and then moved into the object that owns it.
  int gpuMesh = -1;
  int materialIndex = 0; // kept for backward compat, use texName instead
  std::string
      texName; // resolved texture name (directly from per-object MaterialList)

  // A RenderWare material may legitimately carry no texture at all: its Struct
  // has textured = 0 and there is no Texture chunk, only a flat colour. The
  // game shades those with material colour x vertex colour. Binding texture 0
  // instead made every such surface come out solid black — most of the ground
  // and walls in the second half of IntroRoad.
  // Effect sheets (save points, TV glow, candles, blood) are opaque black
  // with the effect painted on top: the game draws them additively, so black
  // contributes nothing. Their palettes are fully opaque, so no alpha test can
  // hide the background.
  // The "FX_" name prefix used to be the only marker, on the belief that the
  // blend mode was absent from the asset. That was wrong: it is the 0x0A01
  // material extension, and the name prefix misses 43 textures that declare a
  // non-standard mode without carrying the prefix — Blood_Pool_SUB among them.
  // Blend mode from the material's own 0x0A01 extension: 0 standard alpha,
  // 1 additive, 2 subtractive. This is the field the engine reads through
  // ClimaxT1MaterialGetFrameBlendMode, and it replaces the guesswork the
  // `additive` flag used to carry.
  uint32_t blendMode = 0;
  // Name of the UV animation this material's 0x0135 extension points at, and
  // the second texture layer its 0x011F UserData names (`n1`). Empty when the
  // material has neither.
  std::string uvAnimName;
  std::string layer2Tex;
  // Which frame of the owning clump this piece hangs off. PS2 characters are
  // segmented, not vertex-skinned -- every atomic is bound rigidly to one
  // frame -- so animating means moving whole pieces by their frame's matrix.
  // The rest-pose matrix is already baked into the vertices, so the renderer
  // applies the difference between the animated matrix and the rest one.
  int frameIndex = -1;
  // True when the native data carried four bone weights per vertex. Those
  // pieces go through the shader's skinning branch; the rest are rigid and
  // follow their frame.
  bool hasWeights = false;
  bool additive = false;
  bool unlitGeometry = false; // vertex colours are all zero: no baked light
  bool untextured = false;
  bool alphaPass = false; // GreyAlpha_* mask, not a colour map
  // Shattered Memories only. `altTexName` is the frozen-state texture the
  // 0x0129 material extension names; `iceEffect` marks the surfaces the game
  // shades as ice. The second one is a judgement call on the texture name --
  // the GX TEV setup that produces the real look is set by game code and is
  // not in the container, exactly like the PS2 blend mode.
  std::string altTexName;
  bool iceEffect = false;
  glm::vec4 matColor = glm::vec4(1.0f);
};

// CPU-side texture.  glID lives here only as an opaque handle written by the
// renderer; core code never calls any GL function on it.
struct RawTexture {
  std::string name;
  int width = 0, height = 0, depth = 0;
  std::vector<uint8_t> pixels;
  std::vector<uint8_t> palette;
  uint32_t glID = 0;  // written by the renderer, opaque here
  bool clampU = false;
  bool clampV = false;
  int paletteColors = 256; // 16 for 4-bit rasters, 256 for 8-bit
  // True when a meaningful share of texels are partially transparent. Such a
  // texture fades out on its own and must be alpha-blended; a sheet whose alpha
  // is uniformly opaque with a black surround is an additive effect instead.
  bool hasAlphaGradient = false;
  // No fully transparent texel anywhere: such a sheet cannot be alpha-cut, so a
  // black-surrounded effect has to be drawn additively instead.
  bool hasTransparentTexels = false;
};

// Collision geometry (CPU arrays only; GPU buffers in Render/GPUMesh.h)
struct CollisionMesh {
    std::vector<glm::vec3> verts;
    std::vector<uint32_t>  indices;   // triangle list

    // Opaque handle to this mesh's GPU mirror, -1 until the renderer uploads
    // it. Deliberately a plain int: this header knows that something else may
    // hold a copy of the data, and nothing more -- no GL types, no GL meaning.
    // It travels with the object when it is moved, which is why it lives here
    // rather than in a table keyed by address.
    int gpuMesh = -1;
};

// ─────────────────────────────────────────────────────────────────────────────
// Container / level meta
// ─────────────────────────────────────────────────────────────────────────────

struct ContainerChunkInfo {
  uint32_t offset;
  uint32_t typeId;
  std::string label;
};

struct ShoTypeEntry {
  std::string name; // e.g. "CZone", "CPlayerSpawner"
  uint32_t count;   // number of instances
};

struct ShoSection {
    // A 0x0716 section header carries two strings: the asset's own name, then
    // its RenderWare type. `name` has always held the type -- "rwID_WORLD",
    // "rwID_HANIMANIMATION" -- because that is what the code keys off. The
    // first string was simply never read, which is why every animation had to
    // be numbered Clip_0 upwards instead of being called PC_TG_Walk.anm.
    std::string assetName;
  uint32_t offset = 0;
  uint32_t size = 0;
  std::string name; // e.g. "rwID_WORLD", "rwID_CBSP", "rwID_CLUMP"
  std::string guid; // raw 16 bytes; game objects reference sections by this
  uint32_t dataStart =
      0; // header-size field points here; payload length follows
  uint32_t payloadSize = 0; // declared length of the RenderWare chunk

  // Where in the level this section's geometry belongs.
  //
  // rwID_WORLD geometry is already baked into world space and is drawn once
  // with identity. rwID_RWS / rwID_CLUMP sections are re-usable models: a
  // 0x0704 object references the section by GUID and supplies the placement,
  // and the same model is often instanced several times. Without this the
  // models were all drawn once, untransformed, piled up on the origin.
  struct Instance {
      glm::mat4 transform;
      int gameObjectId;
  };
  std::vector<Instance> instances;
  bool isWorldSpace = false;

  // Animation data
  Skeleton skeleton; // if rwID_CLUMP and has HAnim
  AnimClip animClip; // if rwID_HANIMANIMATION
};

// ─────────────────────────────────────────────────────────────────────────────
// Scene objects
// ─────────────────────────────────────────────────────────────────────────────

struct ClumpObject {
  std::string sectionName; // e.g. "rwID_CLUMP"
  std::string label;       // short display label (e.g. "CLUMP 0")
  glm::vec3 position;      // world-space position from FrameList
  glm::mat4 transform;     // full 3x4 → 4x4 transform
  int meshStart; // first g_Chunks index belonging to this clump (-1 = none)
  int meshCount; // how many g_Chunks indices
};

struct GameObject {
  std::string className; // "CPickupItem", "CStaticCamera", …
  std::string instName;  // instance / base-class name from the 0x80 record
  std::string label;     // what the viewport marker shows
  glm::mat4 transform = glm::mat4(1.0f);
  glm::vec3 position = glm::vec3(0.0f);
  bool atOrigin = true; // identity transform → not spatially placed
  uint32_t offset = 0;  // chunk offset, for the structure panel
  std::vector<std::string> guidRefs;
  // A second 64-byte property some components carry (CZone property 3). It is
  // the object's own volume, not where it stands, and keeping the two apart is
  // what removed the need for a hand-maintained list of "volume classes".
  glm::mat4 volume = glm::mat4(1.0f);
  bool haveVolume = false; // raw 16-byte GUIDs of referenced sections

  // CColorLight payload. Component 1 property 0 is an RGBA colour; component 2
  // carries [type][cone angle in degrees][range][enabled].
  bool isLight = false;
  glm::vec3 lightColor = glm::vec3(1.0f);
  float lightRange = 10.0f;
  float lightAngle = 45.0f; // >180 means omnidirectional
  int lightType = 0;

  // Field of view in degrees, from CBaseCamera property 2 -- the property the
  // engine feeds to Camera::CBaseCamera::SetFOV. Shared by every camera class,
  // so constraint and cutscene cameras carry it too. -1 means not present.
  float fovDeg = -1.0f;

  // RenderWare Studio wires level logic by *name*, not by pointer or GUID:
  // both CEventHandler::RegisterMsg and ::LinkMsg take a const char*. An object
  // publishes the name it is known by, and other objects name it back.
  //
  //   PlaneTrigger   prop 3 = "camLanding01"   prop 4 = "camLanding02"
  //   CStaticCamera  prop 0 = "camStairway01"
  //
  // `objName` is the first such string the object declares -- the name it is
  // found by. `linkNames` are the rest, in property order: for a PlaneTrigger
  // those are the cameras to switch between, one per crossing direction.
  std::string objName;
  std::vector<std::string> linkNames;

  // Animation playback state
  int currentClipIndex = -1;
  float animTime = 0.0f;
  std::vector<int>
      clipSectionIndices; // indices into g_ShoSections for available clips
  bool animPlaying = false;
  bool animLoop = true;
};

struct LevelCamera {
    std::string name;      // designer name, e.g. "camStairway01"
    // A camera publishes two names -- CBaseCamera property 0 and property 1,
    // the second being the first with an "S" suffix, which is the event name a
    // trigger sends. DH_1_Hallway's planes name the plain form, HO_1_Hallway1's
    // name the S form, so a camera has to be findable by both or half the
    // level logic goes unresolved.
    std::string altName;
    std::string className; // CStaticCamera, CConstraintCamera, ...
    glm::mat4   transform = glm::mat4(1.0f);
    glm::vec3   position  = glm::vec3(0.0f);
    glm::vec3   forward   = glm::vec3(0, 0, 1);
    glm::vec3   up        = glm::vec3(0, 1, 0);
    float       fovDeg    = 60.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// UV animation
// ─────────────────────────────────────────────────────────────────────────────

struct UVAnimKey {
  float time = 0.0f;
  float uScale = 1.0f, vScale = 1.0f;
  float uOff = 0.0f, vOff = 0.0f;
};

struct UVAnimClip {
  float duration = 0.0f;
  std::vector<std::vector<UVAnimKey>> layers; // one keyframe chain per layer
};

// ─────────────────────────────────────────────────────────────────────────────
// Audio (PCM data only; SDL playback in climax-platform)
// ─────────────────────────────────────────────────────────────────────────────

struct AudioSourceRef {
    std::string name;
    std::string group;   // "Music" or "Cutscenes"
    std::string path;    // file on disk; empty when the entry is in an archive
    int arcIndex = -1;   // entry index inside IGC.ARC
};
