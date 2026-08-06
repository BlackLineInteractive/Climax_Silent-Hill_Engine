#pragma once

#include <GL/glew.h>
#include <filesystem>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <map>
#include <string>
#include <vector>

#include "ClimaxEngine/Platform/PS2/AudioParser.h"

namespace fs = std::filesystem;

// strcasecmp is POSIX; MSVC spells it _stricmp.
#ifdef _MSC_VER
#include <string.h>
#define sho_stricmp _stricmp
#define sho_strnicmp _strnicmp
#else
#include <strings.h>
#define sho_stricmp strcasecmp
#define sho_strnicmp strncasecmp
#endif

// ------------------- structures -------------------
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

struct MeshChunk {
  std::vector<Vertex> vertices;
  GLuint vao = 0;
  GLuint vbo = 0;
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
  // Keyed on the "FX_" name prefix, and that is not a shortcut: the blend mode
  // is not stored in the asset at all. Checked and ruled out — the material
  // Extension is an empty UV-anim plugin (0x0A01), byte-identical across all
  // 68 materials of HO_1_Lobby; TEX0 differs only in PSM (pixel format) with
  // TFX=0/TCC=1 everywhere; rasterFormat differs only in the palette-size
  // nibble. On PS2 the blend function lives in the GS ALPHA register, which
  // the engine sets per draw. The naming convention is the only marker the
  // data carries.
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

struct RawTexture {
  std::string name;
  int width = 0, height = 0, depth = 0;
  std::vector<uint8_t> pixels;
  std::vector<uint8_t> palette;
  GLuint glID = 0;
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

// Texture metadata for the TXD preview window
struct TexPreviewInfo {
  GLuint glID = 0;
  int width = 0, height = 0;
  int depth = 0;
};

// One entry in the container structure panel
struct ContainerChunkInfo {
  uint32_t offset;
  uint32_t typeId;
  std::string label;
};

// Entry from the SHO file header type directory
struct ShoTypeEntry {
  std::string name; // e.g. "CZone", "CPlayerSpawner"
  uint32_t count;   // number of instances
};

// A game section inside the container (per 0x716 block)
struct ShoSection {
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

// Collision geometry (from CBSP / second RW_WORLD)
struct CollisionMesh {
  std::vector<glm::vec3> verts;
  std::vector<uint32_t> indices; // triangle list
  GLuint vao = 0;
  GLuint vbo = 0;
  GLuint ebo = 0;
  bool uploaded = false;
  void Upload();
  void Free();
};

// Clump-based game object: position in world space
struct ClumpObject {
  std::string sectionName; // e.g. "rwID_CLUMP"
  std::string label;       // short display label (e.g. "CLUMP 0")
  glm::vec3 position;      // world-space position from FrameList
  glm::mat4 transform;     // full 3x4 → 4x4 transform
  int meshStart; // first g_Chunks index belonging to this clump (-1 = none)
  int meshCount; // how many g_Chunks indices
};

// A placed game-object instance, parsed from a 0x0704 chunk.
//
// These are what the 0x071C type directory counts (CZone, CPickupItem,
// CStaticCamera, …). Each chunk is a tagged property list; property 1 of the
// object's own component is a 4x4 column-major world matrix. Purely logical
// objects (CZone, GameMessage, CMessageRelay, …) legitimately carry identity.
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

  // Animation playback state
  int currentClipIndex = -1;
  float animTime = 0.0f;
  std::vector<int>
      clipSectionIndices; // indices into g_ShoSections for available clips
};

// A camera position recovered from a CStaticCamera / CIGCCamera object.
struct LevelCamera {
  std::string name; // instance name, e.g. "camRoom102Toilet"
  glm::vec3 position = glm::vec3(0.0f);
  glm::vec3 forward = glm::vec3(0, 0, 1);
  glm::vec3 up = glm::vec3(0, 1, 0);
  float fovDeg = 60.0f;
};

// Render mode for the 3D viewport
enum class RenderMode {
  Textured = 0,    // texture + vertex colors (default)
  VertexColor = 1, // only vertex colors, no texture
  FlatShaded = 2,  // per-face normals + directional light, grey
  Normals = 3,     // face normals visualised as RGB colour
  Depth = 4,       // linear depth grey-scale
  Checker = 5,     // UV checkerboard (no texture)
  Unlit = 6,       // texture only, no lighting, no vertex color
};

enum class RenderDevice {
  GPU = 0, // Hardware Accelerated (OpenGL 3.3 / Metal)
  CPU = 1  // Software Rasterizer (CPU Thread Shading & Rasterization)
};

struct ViewerState {
  RenderDevice renderDevice =
      RenderDevice::GPU; // GPU vs CPU render mode toggle

  // Orbit camera — camera rotates around camTarget at distance camDist
  float camTargetX = 0.0f, camTargetY = 2.0f, camTargetZ = 0.0f;
  float camYaw = 0.0f;    // horizontal rotation, degrees
  float camPitch = 20.0f; // vertical   rotation, degrees  (-89..89)
  float camDist = 15.0f;  // zoom distance

  // Free flight camera
  bool useWASD = false;
  float camPosX = 0.0f, camPosY = 2.0f, camPosZ = 15.0f;


  bool flipU = false;
  bool flipV = false;
  float uvOffsetX = 0.0f, uvOffsetY = 0.0f;
  float uvScaleX = 1.0f, uvScaleY = 1.0f;
  bool showWireframe = false;
  bool linearFilter = true;
  // The PS2 UVs regularly run outside 0..1 and only look right with REPEAT
  // wrapping, so this is no longer optional and no longer has a UI toggle.
  static constexpr bool forceRepeat = true;
  bool showUnplacedModels = false; // draw model sections no game object placed
  bool showUI = true;              // master switch for every panel (F1)
  bool useVertexColors = true;
  float brightness = 1.3f;
  bool showCollision = false;      // overlay collision wireframe
  bool showCollisionSolid = false; // fill collision as solid semi-transparent
  bool showClumps = true;          // show clump object markers
  bool showGameObjects = true;     // show 0x0704 game-object markers
  bool showOriginObjects = false; // include objects whose transform is identity
  bool showObjectLabels = true;   // draw the projected name next to each marker
  bool showStructure = true;      // show Structure hierarchy panel
  bool showTextures = true;       // show Textures browser panel
  bool showArc = true;            // show the SH.ARC contents browser
  bool showManual = false;        // show the built-in manual (F2)
  RenderMode renderMode = RenderMode::Textured;

  // Pivot gizmo (ImGuizmo)
  bool showPivotGizmo = true; // draw / allow dragging the pivot gizmo
  bool pivotSnapOn = false; // snap translation to a grid (also forced by Ctrl)
  float pivotSnap = 1.0f;   // grid step in world units

  // Sky / background
  float skyColorTop[3] = {0.07f, 0.07f, 0.09f}; // horizon-to-top colour
  float skyColorBot[3] = {0.11f, 0.11f, 0.14f}; // ground colour
  bool skyGradient = false;                     // draw gradient vs solid

  // Audio player
  bool showAudioPlayer = false;   // panel opens itself the first time audio appears
  bool audioAutoOpened = false;   // ...but only once, so closing it sticks
  bool isAudioPlaying = false;
  bool audioLoop = false;
  float audioVolume = 1.0f;
  float audioProgress = 0.0f;     // 0..1 through the current clip
  int  audioSelected = -1;        // index into g_Sounds, -1 = the dropped file
  char audioFilter[64] = "";

  // Wii: draw the frozen variant of any material that has one, and shade the
  // ice surfaces. Off by default -- both are approximations of engine state.
  bool frozenVariant = false;
  bool iceShading = true;
};

// ------------------- ГЛОБАЛЬНІ ДАНІ (Оголошення) -------------------
extern ViewerState state;
extern std::vector<RawTexture> g_RawTextures;
extern std::vector<std::string> g_MaterialNames;
extern std::map<std::string, GLuint> g_TextureMap;
extern std::map<std::string, TexPreviewInfo> g_TexInfo;
extern std::map<std::string, bool> g_TexGradient;
// Texture name -> has no transparent texel at all; a black-backed effect using
// it must be drawn additively because nothing can cut the background away.
extern std::map<std::string, bool> g_TexOpaque;
extern std::vector<ContainerChunkInfo> g_ContainerChunks;
extern std::map<std::string, std::vector<MeshChunk*>> g_MeshTexMap;

// SHO container meta
extern std::vector<ShoTypeEntry> g_ShoTypes;  // from file header type table
extern std::vector<ShoSection> g_ShoSections; // all 0x716 blocks
extern CollisionMesh g_Collision;             // CBSP floor collision
extern std::vector<ClumpObject> g_Clumps;     // animated/placed objects
extern std::vector<GameObject> g_GameObjects; // 0x0704 placed instances
extern std::vector<LevelCamera> g_Cameras;    // camera objects in the level

// Every sound the current container carries, decoded from its rwaID_WAVEDICT
// section when the level is loaded.
extern std::vector<AudioClip> g_Sounds;

// A track the panel can play but has not decoded yet. The 75 music streams
// alone would be about a gigabyte of PCM, so the library only remembers where
// each one lives and decodes it when it is picked.
struct AudioSourceRef {
    std::string name;
    std::string group;   // "Music" or "Cutscenes"
    std::string path;    // file on disk; empty when the entry is in an archive
    int arcIndex = -1;   // entry index inside IGC.ARC
};
extern std::vector<AudioSourceRef> g_AudioLibrary;

extern std::string g_CurrentMeshContainer;
extern std::vector<std::string> g_CurrentTxdPaths;
