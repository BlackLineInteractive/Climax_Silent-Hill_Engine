#include "ClimaxEngine/Core/Common.h"
#include <cstring>

// Визначення глобальних змінних
ViewerState state;
std::vector<RawTexture>       g_RawTextures;
std::vector<std::string>      g_MaterialNames;
std::map<std::string, GLuint>           g_TextureMap;
std::map<std::string, TexPreviewInfo>   g_TexInfo;
std::map<std::string, bool>             g_TexGradient;
std::map<std::string, bool>             g_TexOpaque;
std::vector<ContainerChunkInfo>         g_ContainerChunks;
std::map<std::string, std::vector<MeshChunk*>> g_MeshTexMap;

std::map<std::string, UVAnimClip> g_UVAnims;
std::vector<AnimClip> g_AnimClips;
std::map<std::string, std::string> g_MatUVAnim;

std::vector<ShoTypeEntry>  g_ShoTypes;
std::vector<ShoSection>    g_ShoSections;
CollisionMesh              g_Collision;
std::vector<ClumpObject>   g_Clumps;
std::vector<GameObject>    g_GameObjects;
std::vector<LevelCamera>   g_Cameras;
std::vector<AudioClip>     g_Sounds;
std::vector<AudioSourceRef> g_AudioLibrary;

std::string              g_CurrentMeshContainer;
std::vector<std::string> g_CurrentTxdPaths;
