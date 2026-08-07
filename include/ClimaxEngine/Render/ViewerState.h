#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClimaxEngine/Render/ViewerState.h  —  Toolkit-only state
//
// This header OWNS everything that belongs to the viewer tool:
//   ViewerState, RenderMode, RenderDevice, TexPreviewInfo,
//   all global scene lists (g_RawTextures, g_TextureMap, …).
//
// Rules:
//   • climax-core and climax-game must NOT include this header.
//   • Only climax-render, climax-toolkit, and the toolkit main include it.
//   • GL types (GLuint) are allowed here.
// ─────────────────────────────────────────────────────────────────────────────

#include <GL/glew.h>
#include <map>
#include <string>
#include <vector>

#include "ClimaxEngine/Core/Types.h"  // RawTexture, MeshChunk, CollisionMesh…

// ─────────────────────────────────────────────────────────────────────────────
// Render enums (viewer choices, not game data)
// ─────────────────────────────────────────────────────────────────────────────

enum class RenderMode {
    Textured    = 0,
    VertexColor = 1,
    FlatShaded  = 2,
    Normals     = 3,
    Depth       = 4,
    Checker     = 5,
    Unlit       = 6,
};

enum class RenderDevice {
    GPU = 0,
    CPU = 1,
};

// Texture metadata for the TXD preview window
struct TexPreviewInfo {
    GLuint glID  = 0;
    int    width = 0, height = 0;
    int    depth = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// ViewerState  —  all settings that live in the ImGui panels
// ─────────────────────────────────────────────────────────────────────────────

struct ViewerState {
  RenderDevice renderDevice =
      RenderDevice::GPU; // GPU vs CPU render mode toggle

  // Orbit camera — camera rotates around camTarget at distance camDist
  float camTargetX = 0.0f, camTargetY = 2.0f, camTargetZ = 0.0f;
  float camYaw = 0.0f;    // horizontal rotation, degrees
  float camPitch = 20.0f; // vertical   rotation, degrees  (-89..89)
  float camDist = 15.0f;  // zoom distance
  // Active field of view, degrees. Set from a level camera's own CStaticCamera
  // property when jumped to; otherwise the viewer's default.
  float camFovDeg = 60.0f;

  // Free flight camera
  bool useWASD = false;
  float camPosX = 0.0f, camPosY = 2.0f, camPosZ = 15.0f;
  // Walk mode: a body that obeys the level's collision instead of a free
  // camera. Off by default -- the viewer is still a viewer.
  bool  playMode = false;
  float walkSpeed = 3.2f;
  float eyeHeight = 1.55f;
  // Which level camera the switch planes have handed control to, or -1 for the
  // free look. Only used in walk mode.
  int   activeCamera = -1;
  bool  autoCameras = true;
  float wasdSpeed = 15.0f;
  float wasdSensitivity = 0.2f;


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

  // Animation player
  bool showAnimPlayer = false;
  // UI settings, kept with the rest of the viewer state so one Settings section
  // owns them instead of them being scattered across the panel.
  float uiScale     = 1.0f;   // font/global scale
  float uiAnimSpeed = 1.0f;   // ImAnim global time scale; 0 disables motion
  bool  uiTooltips  = true;
  // Which clip from g_AnimClips the Playback panel is driving; -1 for none.
  int   animClipIndex = -1;
  // Vertex skinning for the pieces whose native data carries four weights.
  // Rigid pieces keep following their frame either way.
  bool  animSkinning = true;
  // UV animation runs off the scene clock; these only pause and rescale it.
  bool  uvAnimRun = true;
  float uvAnimSpeed = 1.0f;
  float uvAnimTime = 0.0f;
  float animSpeed = 1.0f;
  bool showBoneOverlay = false;
  bool animRestPose = false;

  // Wii: draw the frozen variant of any material that has one, and shade the
  // ice surfaces. Off by default -- both are approximations of engine state.
  bool frozenVariant = false;
  bool iceShading = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// Global scene state (toolkit only)
// ─────────────────────────────────────────────────────────────────────────────

extern ViewerState                                    state;
extern std::vector<RawTexture>                        g_RawTextures;
extern std::vector<std::string>                       g_MaterialNames;
extern std::map<std::string, GLuint>                  g_TextureMap;
extern std::map<std::string, TexPreviewInfo>          g_TexInfo;
extern std::map<std::string, bool>                    g_TexGradient;
extern std::map<std::string, bool>                    g_TexOpaque;
extern std::vector<ContainerChunkInfo>                g_ContainerChunks;
extern std::map<std::string, std::vector<MeshChunk*>> g_MeshTexMap;
extern std::map<std::string, UVAnimClip>              g_UVAnims;
extern std::vector<AnimClip>                          g_AnimClips;
extern std::map<std::string, std::string>             g_MatUVAnim;
extern std::vector<ShoTypeEntry>                      g_ShoTypes;
extern std::vector<ShoSection>                        g_ShoSections;
extern CollisionMesh                                  g_Collision;
extern std::vector<ClumpObject>                       g_Clumps;
extern std::vector<GameObject>                        g_GameObjects;
extern std::vector<LevelCamera>                       g_Cameras;
extern std::vector<AudioClip>                         g_Sounds;
extern std::vector<AudioSourceRef>                    g_AudioLibrary;
extern std::string                                    g_CurrentMeshContainer;
extern std::vector<std::string>                       g_CurrentTxdPaths;
