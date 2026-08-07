#include "ClimaxEngine/Loader/ResourceLoader.h"
#include "ClimaxEngine/Core/RWS/RwStream.h"
#include "ClimaxEngine/SG/SceneObject.h"
#include "ClimaxEngine/Loader/Loader.h"
#include "ClimaxEngine/Core/RWS/FileSystem/CArchiveManager.h"
#include "ClimaxEngine/Core/Common.h"

static std::vector<MeshChunk> g_Chunks;
#include "ClimaxEngine/Platform/PS2/PS2Texture.h"
#include "ClimaxEngine/Platform/Wii/WiiTexture.h"
#include "ClimaxEngine/Platform/Wii/WiiGeometry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <map>
#include <set>
#include <vector>

// --- Utilits ---

std::vector<uint8_t> ReadWholeFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    return {};
  const std::streamoff len = f.tellg();
  if (len <= 0)
    return {};
  f.seekg(0);
  std::vector<uint8_t> data((size_t)len);
  if (!f.read((char *)data.data(), len))
    return {};
  return data;
}

// ------------------- LOADER LOGIC -------------------

// ---------------------------------------------------------------------------
// PS2 display-list geometry (see SH_FORMAT.md section 4)
//
// Geometry is a stream of VIF1 commands, not a vertex array. Each triangle strip
// is uploaded by one packet and then kicked with MSCAL:
//
//     STCYCL 4,1 / UNPACK V3-32 or V4-32  imm 0x8000   positions
//     STCYCL 4,1 / UNPACK V2-32 or V2-16  imm 0x8001   texture coords
//     STCYCL 4,1 / UNPACK V4-8            imm 0xC002   vertex colours
//     STCYCL 4,1 / UNPACK V3-8            imm 0x8003   normals
//     ITOP / MSCAL
//
// Packets are located by anchoring on the position UNPACK. Two things about the
// search matter:
//
//   * The scan must step one byte at a time. Packets are not aligned to any
//     boundary: of the 887 packets in IntroRoad's first world section only 300
//     begin at a 4-byte offset, the rest sit at offsets 1, 2 and 3.
//   * The anchor must be the UNPACK, not STCYCL. The encoded STCYCL word occurs
//     3548 times inside vertex data in that same section.
// ---------------------------------------------------------------------------
static size_t g_DbgNoColor = 0;

namespace {

struct VifStream {
  int vn = 0, vl = 0; // components - 1, element width selector
  int num = 0;        // vectors written
  int addr = 0;       // VU slot, 0..3
  int bpv = 0;        // bytes per vector
  size_t dataOff = 0;
};

struct VifPacket {
  size_t offset = 0;
  std::vector<VifStream> streams;
  int vertexCount = 0;
};

int BytesPerVector(int vn, int vl) {
  if (vl == 3 && vn == 3)
    return 2; // V4-5: four components packed into 16 bits
  static const int BITS[4] = {32, 16, 8, 16};
  return ((vn + 1) * BITS[vl] + 7) / 8;
}

bool IsPositionUnpack(uint32_t cmd) {
  const uint32_t op = (cmd >> 24) & 0x7F;
  if ((op & 0x60) != 0x60)
    return false;
  const int vn = (op >> 2) & 3, vl = op & 3;
  return vl == 0 && (vn == 2 || vn == 3) && (cmd & 0xFFFF) == 0x8000;
}

// Reads one packet beginning at its position UNPACK. `after` receives the offset
// just past the MSCAL that ends it.
bool ReadPacket(const std::vector<uint8_t> &d, size_t p, size_t end,
                VifPacket &out, size_t &after) {
  auto word = [&](size_t o) {
    uint32_t v;
    memcpy(&v, &d[o], 4);
    return v;
  };
  if (p + 4 > end || !IsPositionUnpack(word(p)))
    return false;

  out.offset = p;
  out.streams.clear();

  while (p + 4 <= end) {
    const uint32_t cmd = word(p);
    const uint32_t op = (cmd >> 24) & 0x7F;
    const uint32_t num = (cmd >> 16) & 0xFF;
    const uint32_t imm = cmd & 0xFFFF;
    p += 4;

    if (op == 0x14 || op == 0x15 || op == 0x17) { // MSCAL / MSCALF / MSCNT
      after = p;
      out.vertexCount = out.streams.empty() ? 0 : out.streams[0].num;
      // A genuine packet uploads at least positions and one more stream.
      return out.streams.size() >= 2;
    }
    if ((op & 0x60) == 0x60) {
      VifStream s;
      s.vn = (op >> 2) & 3;
      s.vl = op & 3;
      s.bpv = BytesPerVector(s.vn, s.vl);
      s.num = num ? (int)num : 256;
      s.addr = (int)(imm & 0x3FF);
      s.dataOff = p;
      // Streams within a packet all describe the same vertices.
      if (!out.streams.empty() && s.num != out.streams[0].num)
        return false;
      if (s.addr > 3)
        return false;
      // CL=4 with WL=1 means CL >= WL, so every written vector has a source.
      const size_t payload = ((size_t)s.num * s.bpv + 3) & ~size_t(3);
      if (p + payload > end)
        return false;
      out.streams.push_back(s);
      p += payload;
      continue;
    }
    switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03: // NOP STCYCL OFFSET BASE
    case 0x04: case 0x05: case 0x06: case 0x07: // ITOP STMOD MSKPATH3 MARK
    case 0x10: case 0x11: case 0x13:            // FLUSHE FLUSH FLUSHA
      continue;
    case 0x20: p += 4;  continue;               // STMASK
    case 0x30: case 0x31: p += 16; continue;    // STROW STCOL
    default:
      return false; // not part of a vertex packet
    }
  }
  return false;
}

} // namespace



// ---------------------------------------------------------------------------
// Two-pass transparency: "GreyAlpha_<base>" is a white-on-black mask that the
// game draws over the same geometry as <base>. Rather than replay the second
// pass, fold the mask's luminance into the base texture's alpha so the ordinary
// alpha test cuts the foliage out.
//
// NOTE: In release 0.1.1.5 this pass did not exist at all. Textures already
// carry the correct alpha in their CLUT (palette[transparent].alpha = 0).
// The merge loop was overwriting that correct alpha and making trees/wires
// appear as white squares. Removing the body restores 0.1.1.5 behaviour while
// keeping the Unswizzle4 fix that corrected the pine-tree texture decoding.
// ---------------------------------------------------------------------------
static void ApplyAlphaMasks() {
  // Nothing to do — CLUT-decoded alpha is already correct for all textures.
  (void)g_TexInfo;
}

// ---------------------------------------------------------------------------
// Shattered Memories containers
//
// Same chunk types as Origins, but with no 0x071C type directory and with the
// section header fields in big-endian. Geometry is GameCube native data and is
// not decoded yet; the texture dictionaries are, so a container loads as its
// full texture set plus a section listing.
//
// docs/SHSM_ARC_FORMAT.md section 4 has the layout and the figures behind it.
// ---------------------------------------------------------------------------
// Creates the GL objects for every chunk that does not have them yet. The
// Origins path builds its buffers as it goes; the Wii decoder produces plain
// vertex arrays and leaves the upload to here.
static void UploadChunks() {
  for (auto &m : g_Chunks) {
    if (m.vao || m.vertices.empty()) continue;
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizei)(m.vertices.size() * sizeof(Vertex)),
                 m.vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, uv));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, color));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, boneWeights));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, boneIds));
    glEnableVertexAttribArray(4);
    glBindVertexArray(0);
  }
}

static bool IsShsmContainer(const std::vector<uint8_t> &d) {
  if (d.size() < 64) return false;
  auto le = [&](size_t o) {
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) |
           ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
  };
  auto be = [&](size_t o) {
    return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) |
           ((uint32_t)d[o + 2] << 8) | (uint32_t)d[o + 3];
  };
  // Origins opens with the 0x071C type directory; Shattered Memories opens
  // straight with a section. The version word is not a discriminator -- 1698 of
  // the 1857 containers in data.arc carry the same RW 3.7.0.2 build 0x0065 that
  // Origins uses, and only 159 carry Climax's own 0x1802FFFF. What separates
  // them is the byte order: reading the section header big-endian yields a
  // plausible tag in all 1857, and nonsense lengths on an Origins container.
  if (le(0) != 0x0716) return false;
  const uint32_t tagLen = be(16);
  if (tagLen > 1024 || 20 + tagLen + 20 >= d.size()) return false;
  const uint32_t nameLen = be(20 + tagLen + 16);
  if (nameLen >= 256 || 20 + tagLen + 20 + nameLen > d.size()) return false;
  return d[20 + tagLen + 20] == 'r' && d[20 + tagLen + 21] == 'w';
}

static void ParseShsmContainer(const std::vector<uint8_t> &d) {
  g_ContainerChunks.clear();
  g_ShoTypes.clear();
  g_ShoSections.clear();
  g_Clumps.clear();
  g_GameObjects.clear();
  g_Sounds.clear();
  g_Collision.Free();

  auto le = [&](size_t o) -> uint32_t {
    if (o + 4 > d.size()) return 0;
    return (uint32_t)d[o] | ((uint32_t)d[o + 1] << 8) |
           ((uint32_t)d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24);
  };
  auto be = [&](size_t o) -> uint32_t {
    if (o + 4 > d.size()) return 0;
    return ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) |
           ((uint32_t)d[o + 2] << 8) | (uint32_t)d[o + 3];
  };

  size_t off = 0;
  int objects = 0, textures = 0, meshes = 0;
  size_t skipped = 0;
  while (off + 12 <= d.size()) {
    const uint32_t type = le(off);
    const uint32_t size = le(off + 4);
    if (size == 0 || off + 12 + size > d.size()) break;

    if (type == 0x0704) {
      objects++;
      off += 12 + size;
      continue;
    }
    if (type != 0x0716) {
      off += 12 + size;
      continue;
    }

    const size_t inner = off + 12;
    const uint32_t headerSize = be(inner);
    const uint32_t tagLen = be(inner + 4);
    if (tagLen > 1024) { off += 12 + size; continue; }

    const size_t guidOff = inner + 8 + tagLen;
    const uint32_t nameLen = be(guidOff + 16);
    ShoSection sec;
    sec.offset = (uint32_t)off;
    sec.size = size;
    if (nameLen < 256 && guidOff + 20 + nameLen <= d.size()) {
      const char *p = (const char *)&d[guidOff + 20];
      sec.name.assign(p, strnlen(p, nameLen));
    }
    if (guidOff + 16 <= d.size())
      sec.guid.assign((const char *)&d[guidOff], 16);

    const size_t dataOff = inner + 4 + headerSize;
    if (headerSize > 0 && dataOff + 8 <= d.size()) {
      sec.dataStart = (uint32_t)dataOff;
      sec.payloadSize = be(dataOff);
    }
    g_ShoSections.push_back(sec);

    if (sec.name == "rwID_WORLD" && sec.dataStart + 4 + 12 <= d.size()) {
      const size_t before = g_Chunks.size();
      const size_t avail = d.size() - (sec.dataStart + 4);
      const size_t len = sec.payloadSize && sec.payloadSize <= avail
                             ? sec.payloadSize : avail;
      // NOTE: the section is already in g_ShoSections, so the flag has to be
      // set on the stored copy. Setting it on the local `sec` here left every
      // stored section at isWorldSpace = false, and the renderer then hid all
      // of them as unplaced models.
      g_ShoSections.back().isWorldSpace = true;
      WiiGeom::ReadWorld(d.data(), d.size(), sec.dataStart + 4, len,
                         (int)g_ShoSections.size() - 1, g_Chunks,
                         &g_MaterialNames);
      meshes += (int)(g_Chunks.size() - before);
    }

    if ((sec.name == "rwID_CLUMP" || sec.name == "rwID_RWS") &&
        sec.dataStart + 4 + 12 <= d.size()) {
      // ReadClump bakes each atomic's composed frame matrix into its vertices,
      // so the result is already in its final position and draws with identity.
      // Marking it world-space is also what keeps it visible: the renderer
      // hides a model section that no game object placed, and the Wii 0x0704
      // records are not decoded yet.
      g_ShoSections.back().isWorldSpace = true;
      const size_t before = g_Chunks.size();
      const size_t avail = d.size() - (sec.dataStart + 4);
      const size_t len = sec.payloadSize && sec.payloadSize <= avail
                             ? sec.payloadSize : avail;
      WiiGeom::ReadClump(d.data(), d.size(), sec.dataStart + 4, len,
                         (int)g_ShoSections.size() - 1, g_Chunks,
                         &g_MaterialNames);
      meshes += (int)(g_Chunks.size() - before);
    }

    if (sec.name == "rwID_TEXDICTIONARY" && sec.dataStart + 4 + 12 <= d.size()) {
      const size_t avail = d.size() - (sec.dataStart + 4);
      const size_t len = sec.payloadSize && sec.payloadSize <= avail
                             ? sec.payloadSize : avail;
      std::vector<uint8_t> wiiData(&d[sec.dataStart + 4], &d[sec.dataStart + 4 + len]);
      Wii::WiiTextureDecoder().LoadDictionary(wiiData, {}, true);
      textures++;
    }
    off += 12 + size;
  }

  // Ice and water are shaded by the GX TEV stages, which the container does
  // not store -- the material only names a colour map and, sometimes, its
  // frozen twin. The naming convention is the only marker in the data, so the
  // surfaces are picked by name, the same hand-maintained approach the PS2
  // effect sheets need.
  static const char *kIceWords[] = {"ice", "frozen", "refract", "water"};
  for (auto &c : g_Chunks) {
    std::string low = c.texName;
    for (auto &ch : low) ch = (char)tolower((unsigned char)ch);
    for (const char *w : kIceWords)
      if (low.find(w) != std::string::npos) { c.iceEffect = true; break; }
  }

  size_t tris = 0;
  for (const auto &c : g_Chunks) tris += c.vertices.size() / 3;
  std::cout << "[shsm] " << g_ShoSections.size() << " sections, " << objects
            << " game objects, " << textures << " textures, " << meshes
            << " meshes / " << tris << " triangles";
  if (skipped)
    std::cout << " (" << skipped << " paletted, not supported yet)";
  std::cout << "\n";
}

void LoadLevelData(const std::string &displayName,
                   const std::vector<uint8_t> &container,
                   const std::vector<NamedBlob> &txds) {
  std::vector<GLuint> uniqueIds;
  for (auto &[name, id] : g_TextureMap)
    if (std::find(uniqueIds.begin(), uniqueIds.end(), id) == uniqueIds.end())
      uniqueIds.push_back(id);
  if (!uniqueIds.empty())
    glDeleteTextures((GLsizei)uniqueIds.size(), uniqueIds.data());
  g_TextureMap.clear();
  g_TexInfo.clear();
  g_RawTextures.clear();

  if (IsShsmContainer(container)) {
    g_MaterialNames.clear();
    g_MeshTexMap.clear();
    g_Cameras.clear();
    ParseShsmContainer(container);
    return;
  }

  // Clear registrar for new level
  ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().Clear();
  
  // Parse sections (fills g_ShoSections)
  ParseContainerStructureData(container);
  
  // Use StreamLoader to process the entire container
  ClimaxEngine::RWS::RwMemoryStream stream(container.data(), container.size());
  ClimaxEngine::ResourceLoader::CResourceHandler::GetInstance().ProcessStream(displayName.c_str(), &stream, container.size());

  // Textures
  for (const auto &[name, blob] : txds)
    ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(blob, g_MaterialNames, false);

  std::vector<std::string> missing;
  for (const auto &mat : g_MaterialNames)
    if (g_TextureMap.find(mat) == g_TextureMap.end())
      missing.push_back(mat);
  if (!missing.empty())
    for (const auto &[name, blob] : txds)
      ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(blob, missing, true);

  if (g_TextureMap.empty())
    ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(container, g_MaterialNames, true);

  ApplyAlphaMasks();

  {
    size_t m = 0;
    for (auto &o : ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects())
      m += o->GetMeshes().size();
    {
      std::map<uint32_t, int> bm;
      std::map<uint32_t, std::string> ex;
      for (auto &o : ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects())
        for (auto *mc : o->GetMeshes()) {
          bm[mc->blendMode]++;
          if (mc->blendMode) ex[mc->blendMode] = mc->texName;
        }
      std::cout << "[scene] blend modes:";
      for (auto &kv : bm)
        std::cout << " " << kv.first << "=" << kv.second
                  << (ex.count(kv.first) ? " (" + ex[kv.first] + ")" : "");
      std::cout << "\n";
    }
    std::cout << "[scene] " << g_MaterialNames.size() << " material names, "
              << g_TextureMap.size() << " textures; registered "
              << ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects().size()
              << " objects with " << m << " meshes in total\n";
  }

  g_CurrentMeshContainer = displayName;
  g_CurrentTxdPaths.clear();
  for (const auto &[name, blob] : txds)
    g_CurrentTxdPaths.push_back(name);

  // Re-instantiate Clumps based on g_ShoSections
  auto& registrar = ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance();
  auto objects = registrar.GetObjects(); // Get a copy of the base objects
  {
    size_t withMeshes = 0, meshTotal = 0;
    for (auto &o : objects) {
      const size_t n = o->GetMeshes().size();
      meshTotal += n;
      if (n) withMeshes++;
    }
    std::cout << "[scene] loaders produced " << objects.size() << " objects, "
              << withMeshes << " of them carrying " << meshTotal << " meshes\n";
  }
  registrar.Clear(); // Clear so we can register the instanced versions

  for (const auto& sec : g_ShoSections) {
      // Find the base object parsed for this section
      std::shared_ptr<ClimaxEngine::SG::CSceneObject> baseObj = nullptr;
      std::string expectedName = std::to_string(sec.offset);
      for (auto& obj : objects) {
          if (obj->GetName() == expectedName || obj->GetName() == sec.name || obj->GetName() == (sec.name.empty() ? "WorldSpace" : sec.name)) {
              baseObj = obj;
              break;
          }
      }
      
      if (!baseObj) {
        std::cout << "[scene] section '" << sec.name << "' at " << sec.offset
                  << " has no loaded object\n";
        continue;
      }

      if (auto clump = std::dynamic_pointer_cast<ClimaxEngine::SG::CClumpObject>(baseObj)) {
          if (clump->skeleton.bones.empty()) clump->skeleton = sec.skeleton;
          if (clump->animClip.duration <= 0.0f) clump->animClip = sec.animClip;
      }

      if (sec.isWorldSpace || sec.instances.empty()) {
          registrar.RegisterObject(baseObj); 
      }
      if (!sec.isWorldSpace) {
          for (size_t instIdx = 0; instIdx < sec.instances.size(); instIdx++) {
              const auto& inst = sec.instances[instIdx];
              std::string name = sec.name + "_Inst" + std::to_string(instIdx);
              if (inst.gameObjectId >= 0 && inst.gameObjectId < (int)g_GameObjects.size()) {
                  name = g_GameObjects[inst.gameObjectId].instName;
              }
              
              if (auto clump = std::dynamic_pointer_cast<ClimaxEngine::SG::CClumpObject>(baseObj)) {
                  auto obj = std::make_shared<ClimaxEngine::SG::CClumpObject>(name);
                  obj->SetTransform(inst.transform);
                  obj->skeleton = clump->skeleton;
                  obj->animClip = clump->animClip;
                  
                  if (inst.gameObjectId >= 0 && inst.gameObjectId < (int)g_GameObjects.size()) {
                      const auto& go = g_GameObjects[inst.gameObjectId];
                      if (!go.clipSectionIndices.empty()) {
                          int animIdx = go.clipSectionIndices[0];
                          if (animIdx >= 0 && animIdx < (int)g_ShoSections.size()) {
                              obj->animClip = g_ShoSections[animIdx].animClip;
                          }
                      }
                  }
                  
                  for (auto* m : clump->GetMeshes()) {
                      MeshChunk copy = *m;
                      obj->AddMesh(std::move(copy));
                  }
                  registrar.RegisterObject(obj);
              }
          }
      }
  }

  // Populate g_MeshTexMap
  g_MeshTexMap.clear();
  for (auto& obj : registrar.GetObjects()) {
      for (auto* chunk : obj->GetMeshes()) {
          const std::string &tName = chunk->texName;
          g_MeshTexMap[tName.empty() ? "NULL" : tName].push_back(chunk);
      }
  }
}


// ── Path-based wrappers ─────────────────────────────────────────────────────
void LoadTexturesFromTxd(const std::string &txdPath,
                         const std::vector<std::string> &allowedNames,
                         bool fallback) {
  ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(ReadWholeFile(txdPath), allowedNames, fallback);
}

void LoadGeometry(const std::string &geomPath) {
  std::vector<uint8_t> data = ReadWholeFile(geomPath);
  ClimaxEngine::RWS::RwMemoryStream stream(data.data(), data.size());
  ClimaxEngine::ResourceLoader::CResourceHandler::GetInstance().ProcessStream(geomPath.c_str(), &stream, data.size());
}

void ParseContainerStructure(const std::string &path) {
  ParseContainerStructureData(ReadWholeFile(path));
}

void LoadLevel(const std::string &meshContainerPath,
               const std::vector<std::string> &txdPaths) {
  std::vector<NamedBlob> txds;
  txds.reserve(txdPaths.size());
  for (const auto &p : txdPaths) {
    auto blob = ReadWholeFile(p);
    if (!blob.empty())
      txds.emplace_back(p, std::move(blob));
  }
  LoadLevelData(meshContainerPath, ReadWholeFile(meshContainerPath), txds);
}

// ── Archive entry point ─────────────────────────────────────────────────────
bool LoadLevelFromArc(int entryIndex) {
  if (!ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive() || entryIndex < 0 ||
      entryIndex >= (int)ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Entries().size())
    return false;

  const std::string &name = ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Entries()[entryIndex].name;

  std::vector<uint8_t> container;
  if (!ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Read((size_t)entryIndex, container) || container.empty()) {
    std::cerr << "[arc] cannot inflate container '" << name << "'\n";
    return false;
  }

  std::vector<NamedBlob> txds;
  for (int ti : ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->TxdsFor(name)) {
    std::vector<uint8_t> blob;
    if (ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Read((size_t)ti, blob) && !blob.empty())
      txds.emplace_back(ClimaxEngine::RWS::FileSystem::CArchiveManager::GetInstance().GetFirstArchive()->Entries()[ti].name, std::move(blob));
  }

  std::cerr << "[arc] loading '" << name << "' (" << container.size()
            << " bytes) with " << txds.size() << " texture dictionaries\n";
  LoadLevelData(name, container, txds);
  return true;
}

// ============================================================
// SHO container structure parser — reads the REAL file header,
// enumerates all 0x716 sections, parses CBSP collision, clumps
// ============================================================
// ---------------------------------------------------------------------------
// 0x0704 — a placed game-object instance.
//
// The chunk body is a flat list of tagged records:
//
//     [u32 recordSize][u32 recordId][payload (recordSize - 8 bytes)]
//
// The top byte of recordId selects the record kind, the low 24 bits are the
// property index within the current component:
//
//     0x20  component class name   ("CPickupItem", "CStaticCamera", …)
//     0x40  16-byte GUID (usually a reference to a resource section)
//     0x80  instance / base-class name
//     0x00  indexed property
//
// Property 1 of the object's own component is a 64-byte, column-major 4x4 world
// matrix — this is the placement the viewer was missing, which is why every
// object used to sit at the origin. Names are padded with 0xBF filler bytes.
// ---------------------------------------------------------------------------
static void ParseGameObject(const std::vector<uint8_t> &data, size_t off,
                            uint32_t size) {
  const size_t sz = data.size();
  const size_t body = off + 12;
  const size_t bodyEnd = body + size;
  if (bodyEnd > sz)
    return;

  auto ru32l = [&](size_t o) -> uint32_t {
    uint32_t v;
    memcpy(&v, &data[o], 4);
    return v;
  };
  // Names are NUL-terminated and then padded with 0xBF up to the record size.
  auto readName = [&](size_t o, size_t len) -> std::string {
    size_t end = o;
    while (end < o + len && data[end] != 0x00 && data[end] != 0xBF)
      ++end;
    return std::string((const char *)&data[o], end - o);
  };

  GameObject go;
  go.offset = (uint32_t)off;

  bool haveClass = false;
  bool haveXform = false;
  // A 0x80 record is not the instance name -- it opens a new component and
  // names its class. Property indices restart at 0 inside each one, because
  // the engine's attribute iterator is filtered by class id, so an index means
  // nothing without knowing which component it belongs to.
  std::string component;

  size_t p = body + 4; // the body opens with a 4-byte field we skip
  while (p + 8 <= bodyEnd) {
    const uint32_t rs = ru32l(p);
    const uint32_t rid = ru32l(p + 4);
    if (rs < 8 || p + rs > bodyEnd)
      break;

    const size_t payOff = p + 8;
    const size_t payLen = rs - 8;
    const uint32_t kind = rid >> 24;
    const uint32_t idx = rid & 0x00FFFFFF;

    if (kind == 0x20) {
      if (!haveClass) {
        go.className = readName(payOff, payLen);
        haveClass = true;
      }
    } else if (kind == 0x80) {
      component = readName(payOff, payLen);
      if (go.instName.empty())
        go.instName = component;
    } else if (kind == 0x00 && payLen == 16) {
      go.guidRefs.emplace_back((const char *)&data[payOff], 16);
    } else if (kind == 0x00 && payLen == 64 && component == "CZone" && idx == 3) {
      // CZone carries its own 64-byte value as well. It is the zone's volume,
      // not a placement -- measured on 120 containers: all 70 objects that hold
      // two 64-byte properties are exactly (CSystemCommands, CZone) pairs.
      memcpy(&go.volume[0][0], &data[payOff], 64);
      go.haveVolume = true;
    } else if (kind == 0x00 && idx == 1 && payLen == 64 &&
               component == "CSystemCommands" && !haveXform) {
      // The placement matrix always lives here: property 1 of the
      // CSystemCommands component, in 3726 of 3726 placed objects across the
      // sample. Keying on the index alone would let another component's
      // 64-byte property win by arriving first.
      glm::mat4 m;
      memcpy(&m[0][0], &data[payOff], 64);
      m[0][3] = 0.0f;
      m[1][3] = 0.0f;
      m[2][3] = 0.0f;
      m[3][3] = 1.0f;
      go.transform = m;
      haveXform = true;
    }
    p += rs;
  }

  if (go.className.empty())
    return;

  if (haveXform) {
    // Every class is placed the same way, so there is no list of "volume
    // classes" to maintain. What made those classes look different is that
    // they carry a *second* 64-byte property of their own; that one is now
    // read separately as go.volume and never mistaken for a placement.
    go.position = glm::vec3(go.transform[3]);
    go.atOrigin = (glm::length(go.position) < 1e-4f);
  }

  // Second pass for CColorLight: the payload is spread over two components.
  if (go.className == "CColorLight") {
    go.isLight = true;
    // Property indices restart at 0 for each component of the object, so a
    // group boundary is simply "the index stopped increasing". That is more
    // reliable than guessing which record type delimits a component.
    int group = 0;
    long lastIdx = -1;
    size_t q = body + 4;
    while (q + 8 <= bodyEnd) {
      const uint32_t rs = ru32l(q);
      const uint32_t rid = ru32l(q + 4);
      if (rs < 8 || q + rs > bodyEnd)
        break;
      const size_t payOff = q + 8, payLen = rs - 8;
      const uint32_t kind = rid >> 24, idx = rid & 0x00FFFFFF;

      if (kind == 0x00) {
        if ((long)idx <= lastIdx) {
          ++group;
        }
        lastIdx = (long)idx;

        if (payLen == 4) {
          float f;
          memcpy(&f, &data[payOff], 4);
          if (group == 0 && idx == 0) {
            go.lightColor =
                glm::vec3(data[payOff + 0] / 255.0f, data[payOff + 1] / 255.0f,
                          data[payOff + 2] / 255.0f);
          } else if (group == 1) {
            if (idx == 0)
              memcpy(&go.lightType, &data[payOff], 4);
            else if (idx == 1)
              go.lightAngle = f;
            else if (idx == 2)
              go.lightRange = f;
          }
        }
      }
      q += rs;
    }
  }

  go.label = go.className;
  if (!go.instName.empty() && go.instName != go.className)
    go.label += " (" + go.instName + ")";
  g_GameObjects.push_back(std::move(go));
}

// Reads the container's UV animations out of its 0x2B sections.
//
// 0x2B is an RWS section type, not a RenderWare chunk id -- Ghost Rider names
// it outright, `CUVAnimationStreamLoader::GetTypeID` returns 0x2B. Inside sits
// a Struct holding the animation count, then one 0x1B chunk per animation whose
// payload is 88 bytes of header (the standard RtAnimAnimation fields, a 32-byte
// name, and a block of saved runtime pointers) followed by the keyframes.
//
// A keyframe is 32 bytes on disk; the 0x18 that `ClimaxT1KeyFrameStreamGetSizeCB`
// returns is the size it occupies in memory, which is smaller. The last word is
// the index of the previous keyframe, and following those links splits the
// frames into one chain per texture layer.

static void ParseUVAnimations(const std::vector<uint8_t> &data) {

  const uint32_t RW_VER = 0x1c020065;
  const size_t sz = data.size();
  auto ru32 = [&](size_t o) -> uint32_t {
    uint32_t v = 0;
    if (o + 4 <= sz) memcpy(&v, &data[o], 4);
    return v;
  };
  auto rf32 = [&](size_t o) -> float {
    float v = 0.0f;
    if (o + 4 <= sz) memcpy(&v, &data[o], 4);
    return v;
  };

  // Scanned exhaustively rather than by walking chunk sizes: the 0x2B sections
  // sit inside the container's shells, so a top-level walk steps straight over
  // them. Byte-by-byte, not word-by-word — the sections are not word aligned
  // (DH_1_Exterior has one at 1053589), so a stride of 4 misses them entirely.
  for (size_t o = 0; o + 12 <= sz; o += 1) {
    const uint32_t t = ru32(o), s = ru32(o + 4), v = ru32(o + 8);
    if (t != 0x2B || v != RW_VER || s == 0 || o + 12 + s > sz) continue;

    // Struct chunk carrying the animation count, then the animations.
    size_t p = o + 12;
    const uint32_t hdrSize = ru32(p + 4);
    p += 12 + hdrSize;

    while (p + 12 <= o + 12 + s) {
      const uint32_t at = ru32(p), as = ru32(p + 4);
      if (at != 0x1B || as == 0 || p + 12 + as > o + 12 + s) break;
      const size_t h = p + 12;

      const uint32_t numFrames = ru32(h + 8);
      const float duration = rf32(h + 16);
      std::string name;
      for (size_t k = 0; k < 32 && data[h + 24 + k]; k++) name += (char)data[h + 24 + k];

      // 20 bytes of RtAnimAnimation header + 68 of RpUVAnimCustomData, then
      // the keyframes. RenderWare has two UV keyframe schemes and only the
      // linear one is 32 bytes; the stride the data implies tells them apart.
      const size_t keys = h + 88;
      const uint32_t stride =
          (numFrames && as > 88) ? (uint32_t)((as - 88) / numFrames) : 0;
      if (numFrames && numFrames < 4096 && stride == 32 &&
          keys + (size_t)numFrames * 32 <= h + as) {
        UVAnimClip clip;
        clip.duration = duration;
        std::vector<UVAnimKey> flat(numFrames);
        std::vector<uint32_t> prev(numFrames);
        for (uint32_t k = 0; k < numFrames; k++) {
          const size_t q = keys + k * 32;
          flat[k].time   = rf32(q);
          flat[k].uScale = rf32(q + 8);
          flat[k].vScale = rf32(q + 12);
          flat[k].uOff   = rf32(q + 20);
          flat[k].vOff   = rf32(q + 24);
          prev[k]        = ru32(q + 28);
        }
        // A frame whose previous index is not a real frame starts a new chain,
        // and every other frame joins the chain its predecessor is in.
        std::vector<int> layerOf(numFrames, -1);
        for (uint32_t k = 0; k < numFrames; k++) {
          if (prev[k] < numFrames && prev[k] != k && layerOf[prev[k]] >= 0)
            layerOf[k] = layerOf[prev[k]];
          else if (prev[k] >= numFrames) {
            layerOf[k] = (int)clip.layers.size();
            clip.layers.emplace_back();
          }
        }
        for (uint32_t k = 0; k < numFrames; k++)
          if (layerOf[k] >= 0) clip.layers[layerOf[k]].push_back(flat[k]);

        if (!clip.layers.empty() && !name.empty()) {
          for (auto &lay : clip.layers)
            std::sort(lay.begin(), lay.end(),
                      [](const UVAnimKey &a, const UVAnimKey &b) { return a.time < b.time; });
          g_UVAnims[name] = std::move(clip);
        }
      }
      p += 12 + as;
    }
  }

  if (!g_UVAnims.empty())
    std::cout << "[uvanim] " << g_UVAnims.size() << " clips\n";
}

// Reads every skeletal animation clip in the container.
//
// Clips are 0x1B chunks sitting directly in the stream, not inside a 0x716
// shell, so the section walk never reaches them -- the same trap the UV
// animations sprang. Scanned byte-by-byte for the same reason: the chunks are
// not word aligned.
//
// Layout, confirmed by arithmetic over all 3029 clips in the archive: every one
// satisfies `chunkSize - records*20 - 20 == 24`, which is exactly the 20-byte
// RtAnimAnimation header, six floats of translation offset and scale, then one
// 20-byte record per keyframe.
static void ParseSkeletalAnimations(const std::vector<uint8_t> &data) {
  const uint32_t RW_VER = 0x1c020065;
  const size_t sz = data.size();
  auto ru32 = [&](size_t o) -> uint32_t {
    uint32_t v = 0;
    if (o + 4 <= sz) memcpy(&v, &data[o], 4);
    return v;
  };
  auto rf32 = [&](size_t o) -> float {
    float v = 0.0f;
    if (o + 4 <= sz) memcpy(&v, &data[o], 4);
    return v;
  };

  for (size_t o = 0; o + 32 <= sz; o += 1) {
    if (ru32(o) != 0x1B || ru32(o + 8) != RW_VER) continue;
    const uint32_t cs = ru32(o + 4);
    if (cs < 44 || o + 12 + cs > sz) continue;

    const size_t h = o + 12;
    if (ru32(h) != 0x100) continue;                  // version
    if (ru32(h + 4) != 0x1103) continue;             // Climax keyframe scheme
    const uint32_t records = ru32(h + 8);
    const float duration = rf32(h + 16);
    if (!records || records > 200000) continue;
    if (!(duration > 0.0f && duration < 600.0f)) continue;
    if (cs != 20 + 24 + records * 20) continue;      // the layout must add up

    float tOff[3], tScl[3];
    for (int j = 0; j < 3; j++) tOff[j] = rf32(h + 20 + j * 4);
    for (int j = 0; j < 3; j++) tScl[j] = rf32(h + 32 + j * 4);

    const size_t hdrBase = h + 44;
    const size_t datBase = hdrBase + (size_t)records * 8;

    AnimClip clip;
    clip.name = "Clip_" + std::to_string(g_AnimClips.size());
    clip.duration = duration;
    clip.fps = 30.0f;

    // Records are a flat list. Each points at its predecessor by byte offset,
    // and the stride is 20, so the chain gives the track a record belongs to.
    // The field is a byte offset back to the previous keyframe of the same
    // track -- except in the first record of each track, where the file keeps
    // the saved runtime pointer instead. Those read as huge negatives that step
    // by exactly the 20-byte stride, so anything that does not resolve to an
    // earlier record in this clip is a track root. The count that falls out is
    // the bone count.
    std::vector<int> track((size_t)records, -1);
    int tracks = 0;
    for (uint32_t k = 0; k < records; k++) {
      const int32_t prevOff = (int32_t)ru32(hdrBase + (size_t)k * 8);
      const long prev = (long)k - (long)prevOff / 20;
      if (prev < 0 || prev >= (long)k) {
        track[k] = tracks++;
        clip.tracks.emplace_back();
      } else {
        track[k] = track[(size_t)prev];
      }
    }
    if (tracks <= 0 || tracks > 512) continue;

    for (uint32_t k = 0; k < records; k++) {
      const int ti = track[k];
      if (ti < 0 || ti >= (int)clip.tracks.size()) continue;
      const float t = rf32(hdrBase + (size_t)k * 8 + 4);
      const size_t dp = datBase + (size_t)k * 12;
      const uint32_t c1 = ru32(dp);
      uint16_t c2, tx, ty, tz;
      memcpy(&c2, &data[dp + 4], 2);
      memcpy(&tx, &data[dp + 6], 2);
      memcpy(&ty, &data[dp + 8], 2);
      memcpy(&tz, &data[dp + 10], 2);

      // A quaternion in four 12-bit fields; the reference stores the conjugate.
      const float qx = ((float)(c1 >> 20) - 2048.0f) / 2047.0f;
      const float qy = ((float)((c1 >> 8) & 0xFFF) - 2048.0f) / 2047.0f;
      const float qz = ((float)((((c1 << 4) & 0xFFF) | (c2 >> 12))) - 2048.0f) / 2047.0f;
      const float qw = ((float)(c2 & 0xFFF) - 2048.0f) / 2047.0f;

      clip.tracks[(size_t)ti].times.push_back(t);
      clip.tracks[(size_t)ti].rot.push_back(glm::quat(qw, -qx, -qy, -qz));
      clip.tracks[(size_t)ti].pos.push_back(
          glm::vec3((tx / 65535.0f) * tScl[0] + tOff[0],
                    (ty / 65535.0f) * tScl[1] + tOff[1],
                    (tz / 65535.0f) * tScl[2] + tOff[2]));
    }
    g_AnimClips.push_back(std::move(clip));
    o += 12 + cs - 1;
  }

  if (!g_AnimClips.empty()) {
    size_t minT = SIZE_MAX, maxT = 0;
    for (auto &c : g_AnimClips) {
      minT = std::min(minT, c.tracks.size());
      maxT = std::max(maxT, c.tracks.size());
    }
    std::cout << "[anim] " << g_AnimClips.size() << " clips, "
              << minT << ".." << maxT << " tracks each\n";
  }
}

void ParseContainerStructureData(const std::vector<uint8_t> &data) {
  g_ContainerChunks.clear();
  g_ShoTypes.clear();
  g_UVAnims.clear();
  g_AnimClips.clear();
  g_ShoSections.clear();
  g_Clumps.clear();
  g_GameObjects.clear();
  g_Sounds.clear();
  g_Collision.Free();

  const size_t sz = data.size();
  if (sz < 16)
    return;

  // New StreamLoader API
  ClimaxEngine::RWS::RwMemoryStream memStream(data);
  ClimaxEngine::ResourceLoader::CResourceHandler::GetInstance().ProcessStream("Container", &memStream, sz);

  ParseUVAnimations(data);
  ParseSkeletalAnimations(data);


  const uint32_t RW_VER = 0x1c020065;

  auto ru32 = [&](size_t off) -> uint32_t {
    if (off + 4 > sz)
      return 0;
    uint32_t v;
    memcpy(&v, &data[off], 4);
    return v;
  };
  auto rf32 = [&](size_t off) -> float {
    if (off + 4 > sz)
      return 0.f;
    float v;
    memcpy(&v, &data[off], 4);
    return v;
  };
  auto safeCStr = [&](size_t off, size_t maxLen) -> std::string {
    size_t end = off;
    while (end < sz && end < off + maxLen && data[end] != 0)
      ++end;
    return std::string((char *)&data[off], end - off);
  };

  // ── 1. Parse file header (type 0x071c) ────────────────────────
  // header: [type(4)=0x071c][size(4)][version(4)][numEntries(4)] [entries...]
  uint32_t hdrType = ru32(0);
  uint32_t hdrSize = ru32(4);
  if (hdrType == 0x071c && hdrSize > 0 && hdrSize < sz) {
    size_t dirEnd = 0x0c + hdrSize; // byte after last directory byte
    size_t off = 0x10;              // entries start after numEntries field
    while (off < dirEnd) {
      if (data[off] == 0) {
        ++off;
        continue;
      }
      // Read null-terminated name
      size_t nameEnd = off;
      while (nameEnd < dirEnd && data[nameEnd] != 0)
        ++nameEnd;
      if (nameEnd == off) {
        ++off;
        continue;
      }
      std::string name((char *)&data[off], nameEnd - off);
      // Align to 4 bytes, then read count u32
      size_t padEnd = (nameEnd + 1 + 3) & ~size_t(3);
      if (padEnd + 4 > dirEnd)
        break;
      uint32_t count = ru32(padEnd);
      if (count > 0 && count < 65536) {
        ShoTypeEntry te;
        te.name = name;
        te.count = count;
        g_ShoTypes.push_back(std::move(te));
      }
      off = padEnd + 4;
    }
  }

  // ── 2. Walk the top-level chunk list ──────────────────────────
  // Start right after the 0x071C directory when it is sane, so the walk runs
  // chunk-to-chunk instead of byte-scanning through the header.
  size_t off716 =
      (hdrType == 0x071c && hdrSize > 0 && hdrSize < sz) ? 0x0c + hdrSize : 0;

  // ── BARE CLUMP / ANIM-PACKAGE FALLBACK ────────────────────────────────────
  // Character model files (CPlayerBehaviour.Travis, etc.) are bare RenderWare
  // files — they start directly with a CLUMP (0x10) or an animation-package
  // chunk (0x1E, 0x1B, etc.), not with a 0x071C SHO directory. The 0x716
  // section walker below will skip everything when these files are given, so
  // we create a single synthetic ShoSection covering the whole file and let
  // LoadGeometryData process it normally.
  if (g_ShoSections.empty() && sz >= 12) {
    uint32_t firstType = ru32(0);
    uint32_t firstSize = ru32(4);
    uint32_t firstVer  = ru32(8);
    // Treat as bare RW file: CLUMP=0x10, ANIM=0x1B/0x1E/0x1F, also 0x23/0x24 (RWS)
    const bool isBareRW = (firstVer == RW_VER) && (
        firstType == 0x10 || firstType == 0x1B || firstType == 0x1E ||
        firstType == 0x1F || firstType == 0x23 || firstType == 0x24) &&
        firstSize > 0 && 12 + firstSize <= sz;
    if (isBareRW) {
      ShoSection synth;
      synth.offset    = 0;
      synth.size      = (uint32_t)(firstSize);
      synth.name      = (firstType == 0x10) ? "rwID_CLUMP" :
                        (firstType == 0x1B) ? "rwID_HANIMANIMATION" :
                        "rwID_RWS";
      synth.dataStart = 0;   // payload starts right at offset 0 (the CLUMP chunk)
      synth.isWorldSpace = false;
      g_ShoSections.push_back(synth);
      std::cerr << "[loader] bare RW file detected (type=0x" << std::hex << firstType
                << ", ver=0x" << firstVer << std::dec << "), created synthetic section '"
                << synth.name << "'\n";
      // If there are multiple top-level chunks (e.g. CLUMP + HANIM) scan them
      size_t co = 0;
      while (co + 12 < sz) {
        uint32_t t = ru32(co), s2 = ru32(co + 4), v = ru32(co + 8);
        if (v != RW_VER || s2 == 0 || co + 12 + s2 > sz) break;
        if (t == 0x1B) {
          // rwID_HANIMANIMATION — register as its own section so the clip
          // resolver (GUID pass) doesn't need to find it inside a 0x716 shell.
          ShoSection animSec;
          animSec.offset    = (uint32_t)co;
          animSec.size      = s2;
          animSec.name      = "rwID_HANIMANIMATION";
          animSec.dataStart = (uint32_t)co;
          animSec.isWorldSpace = false;
          g_ShoSections.push_back(animSec);
        }
        co += 12 + s2;
      }
    }
  }


  while (off716 + 12 < sz) {
    uint32_t t = ru32(off716);
    uint32_t s = ru32(off716 + 4);
    uint32_t v = ru32(off716 + 8);
    if (v != RW_VER || s == 0 || off716 + 12 + s > sz) {
      off716 += 4;
      continue;
    }
    // 0x0704 chunks are the placed game-object instances; parsed below.
    if (t == 0x0704) {
      ParseGameObject(data, off716, s);
      off716 += 12 + s;
      continue;
    }
    if (t != 0x716) {
      off716 += 12 + s;
      continue;
    }

    // Inner header of a 0x716 section:
    //   [count(4)][tagLen(4)][tag(tagLen)][guid(16)][nameLen(4)][name(nameLen)]
    //
    // The old code assumed tagLen was always 4 and read the name at a fixed
    // inner+28. That holds for rwID_WORLD/CBSP/WAVEDICT but not for sections
    // that carry a real tag — rwID_AINAVMESH stores "MO_1_Room102_Navmesh.nav"
    // there, which pushed everything 24 bytes along and made the section come
    // out unnamed with a garbage nameLen of ~2 billion.
    size_t inner = off716 + 12;
    uint32_t tagLen = ru32(inner + 4);
    if (tagLen > 1024)
      tagLen = 4;
    size_t guidOff = inner + 8 + tagLen;
    uint32_t nameLen = ru32(guidOff + 16);
    size_t nameOff = guidOff + 20;

    std::string secName;
    if (nameLen < 256 && nameOff + nameLen <= sz)
      secName = safeCStr(nameOff, nameLen);

    // Navigate past the two embedded path strings to object_start
    size_t p1off = nameOff + nameLen;
    uint32_t p1len = ru32(p1off);
    size_t p2off = p1off + 4 + (p1len < 1024 ? p1len : 0);
    if (p2off + 4 > sz) {
      off716 += 12 + s;
      continue;
    }
    uint32_t p2len = ru32(p2off);
    size_t objStart = p2off + 4 + (p2len < 1024 ? p2len : 0);

    ShoSection sec;
    sec.offset = (uint32_t)off716;
    sec.size = s;
    sec.name = secName;
    // The first field of a section header is its own length, so the payload is
    // reachable without walking the strings at all:
    //
    //     dataOff  = inner + 4 + headerSize   -> u32 payload length
    //     chunk    = dataOff + 4              -> the RenderWare chunk
    //
    // This is what the QuickBMS unpack script does, and unlike stepping over the
    // tag, GUID and build paths it holds for every section type — including the
    // rwID_RWS 0x23/0x24 variants whose header layout is still unknown.
    {
      const uint32_t headerSize = ru32(inner);
      const size_t dataOff = inner + 4 + headerSize;
      if (headerSize > 0 && dataOff + 8 <= sz) {
        sec.dataStart = (uint32_t)dataOff;
        sec.payloadSize = ru32(dataOff);
      } else {
        sec.dataStart = (uint32_t)objStart;
      }
    }
    if (guidOff + 16 <= sz)
      sec.guid.assign((const char *)&data[guidOff], 16);
    // World geometry is baked into level space; everything else is a model
    // that a game object has to place.
    sec.isWorldSpace = (secName == "rwID_WORLD");
    g_ShoSections.push_back(sec);

    // ── 2a. The level's own sound bank ─────────────────────────
    // sec.dataStart points at [u32 payloadSize][chunk], so the 0x0809 wave
    // dictionary starts four bytes further in.
    if (secName == "rwaID_WAVEDICT" && sec.dataStart + 4 + 12 <= sz) {
      const size_t avail = sz - (sec.dataStart + 4);
      const size_t len = sec.payloadSize && sec.payloadSize <= avail
                             ? sec.payloadSize
                             : avail;
      const size_t before = g_Sounds.size();
      Audio::ParseWaveDictionary(&data[sec.dataStart + 4], len, g_Sounds);
      std::cout << "[audio] " << (g_Sounds.size() - before) << " samples from "
                << secName << "\n";
      if (g_Sounds.size() > before && !state.audioAutoOpened) {
        state.audioAutoOpened = true;
        state.showAudioPlayer = true;
      }
    }

    // ── 2b. CBSP collision ─────────────────────────────────────
    if (secName == "rwID_CBSP" && objStart + 12 < sz) {
      // Scan for the 0x1100 child rather than assuming it starts exactly at
      // objStart: in the retail containers it sits 8 bytes further in, and
      // the old fixed-stride walk (co += 12 + cs) stepped straight over it,
      // so the collision overlay silently had nothing to draw.
      size_t co = objStart;
      while (co + 12 <= off716 + 12 + s) {
        uint32_t ct = ru32(co);
        uint32_t cs = ru32(co + 4);
        uint32_t cv = ru32(co + 8);
        if (!(cv == RW_VER && ct == 0x1100 && cs > 32)) {
          co += 4;
          continue;
        }
        {
          // data starts at co+12
          size_t doff = co + 12;
          uint32_t numVerts = ru32(doff + 8);  // group 0 vertex count
          uint32_t numNodes = ru32(doff + 12); // BSP node count (8 bytes each)
          if (numVerts > 0 && numVerts < 4096 &&
              doff + 32 + numVerts * 16 <= cs + co + 12) {
            // Extract vertices (x,y,z, flags) 16 bytes each
            size_t vbase = doff + 32;
            for (uint32_t vi = 0; vi < numVerts; ++vi) {
              float x = rf32(vbase + vi * 16 + 0);
              float y = rf32(vbase + vi * 16 + 4);
              float z = rf32(vbase + vi * 16 + 8);
              if (std::isfinite(x) && std::isfinite(z))
                g_Collision.verts.push_back({x, y, z});
            }
            // Triangle indices (u8 packed: a,b,c,flags) after vert+node data
            size_t faceOff = vbase + numVerts * 16 + numNodes * 8;
            size_t faceEnd = co + 12 + cs;
            uint32_t nv = (uint32_t)g_Collision.verts.size();
            while (faceOff + 4 <= faceEnd) {
              uint8_t a = data[faceOff], b = data[faceOff + 1],
                      c = data[faceOff + 2];
              faceOff += 4;
              if (a < nv && b < nv && c < nv && a != b && b != c && a != c) {
                g_Collision.indices.push_back(a);
                g_Collision.indices.push_back(b);
                g_Collision.indices.push_back(c);
              }
            }
          }
          break;
        }
      }
    }

    // ── 2b. CLUMP (animated objects with frame transforms) ────
    if (secName == "rwID_CLUMP" && objStart + 12 < sz) {
      // Walk children looking for FrameList (type 0x0e)
      size_t co = objStart;
      while (co + 12 <= off716 + 12 + s) {
        uint32_t ct = ru32(co);
        uint32_t cs = ru32(co + 4);
        uint32_t cv = ru32(co + 8);
        if (cv != RW_VER || cs == 0) {
          co += 4;
          continue;
        }

        if (ct == 0x0e) { // FrameList
          // Struct child of FrameList has the actual frame data
          size_t st_off = co + 12;
          uint32_t st_t = ru32(st_off);
          uint32_t st_s = ru32(st_off + 4);
          uint32_t st_v = ru32(st_off + 8);
          if (st_t == 0x01 && st_v == RW_VER && st_s >= 4) {
            uint32_t numFrames = ru32(st_off + 12);
            // Each frame: rot(9×f32=36b) + pos(3×f32=12b) + parentIdx(i32=4b) +
            // flags(4b) = 56b
            const size_t FS = 56;
            if (numFrames > 0 && numFrames <= 256 &&
                st_off + 12 + 4 + numFrames * FS <= sz) {

              size_t fBase = st_off + 12 + 4; // skip numFrames field

              // Build full world-transform for every frame by composing parent
              // chain
              std::vector<glm::mat4> worldMats(numFrames, glm::mat4(1.0f));
              std::vector<int32_t> parents(numFrames, -1);

              for (uint32_t fi = 0; fi < numFrames; ++fi) {
                size_t fb = fBase + fi * FS;
                // Build 4×4 from local rot + pos (column-major: right/up/at as
                // cols)
                glm::mat4 local(1.0f);
                for (int r = 0; r < 3; ++r)
                  for (int c = 0; c < 3; ++c)
                    local[c][r] = rf32(fb + (r * 3 + c) * 4);
                local[3][0] = rf32(fb + 36);
                local[3][1] = rf32(fb + 40);
                local[3][2] = rf32(fb + 44);

                int32_t par;
                memcpy(&par, &data[fb + 48], 4);
                parents[fi] = par;
                if (par >= 0 && par < (int32_t)fi)
                  worldMats[fi] = worldMats[par] * local;
                else
                  worldMats[fi] = local;
              }

              // Parse Extensions to build the Skeleton
              Skeleton skeleton;
              skeleton.bones.resize(numFrames);
              
              size_t extOff = st_off + 12 + st_s;
              for (uint32_t fi = 0; fi < numFrames; ++fi) {
                if (extOff + 12 > co + 12 + cs) break;
                uint32_t ext_t = ru32(extOff);
                uint32_t ext_s = ru32(extOff + 4);
                if (ext_t != 0x03) break;
                
                size_t ext_body = extOff + 12;
                size_t ext_end = ext_body + ext_s;
                
                // Set default bone properties
                skeleton.bones[fi].name = (fi == 1) ? "RootBone" : ("Bone" + std::to_string(fi));
                skeleton.bones[fi].parent = parents[fi];
                skeleton.bones[fi].restLocal = (parents[fi] >= 0) ? (worldMats[parents[fi]] * glm::inverse(worldMats[fi])) : worldMats[fi]; // Wait, we have local! We don't need to do this.
                
                // Rebuild local properly
                glm::mat4 local(1.0f);
                size_t fb = fBase + fi * FS;
                for (int r = 0; r < 3; ++r)
                  for (int c = 0; c < 3; ++c)
                    local[c][r] = rf32(fb + (r * 3 + c) * 4);
                local[3][0] = rf32(fb + 36);
                local[3][1] = rf32(fb + 40);
                local[3][2] = rf32(fb + 44);
                skeleton.bones[fi].restLocal = local;
                
                size_t p = ext_body;
                while (p + 12 <= ext_end) {
                  uint32_t pt = ru32(p);
                  uint32_t ps = ru32(p + 4);
                  size_t pb = p + 12;
                  
                  if (pt == 0x011E && ps >= 8) { // HAnim PLG
                    int32_t version, boneId, boneCount;
                    memcpy(&version, &data[pb], 4);
                    memcpy(&boneId, &data[pb + 4], 4);
                    memcpy(&boneCount, &data[pb + 8], 4);
                    // boneCount is non-zero only on the frame that owns the hierarchy.
                    // The bone table maps bone ids to skin indices, but we'll extract invBind from Skin PLG.
                  } else if (pt == 0x011F && ps >= 4) { // User-data PLG
                    int32_t numSets;
                    memcpy(&numSets, &data[pb], 4);
                    size_t up = pb + 4;
                    for (int s = 0; s < numSets && up + 16 <= p + 12 + ps; ++s) {
                      int32_t typeLen;
                      memcpy(&typeLen, &data[up], 4);
                      up += 12 + typeLen; // skip typeLen, skip 2 unknowns
                      if (up + 4 <= p + 12 + ps) {
                        int32_t nameLen;
                        memcpy(&nameLen, &data[up], 4);
                        up += 4;
                        if (nameLen > 1 && up + nameLen <= p + 12 + ps) {
                          std::string name((const char*)&data[up], nameLen - 1);
                          skeleton.bones[fi].name = name;
                        }
                        up += nameLen;
                      }
                    }
                  }
                  p += 12 + ps;
                }
                extOff = ext_end;
              }

              if (numFrames > 0) {
                g_ShoSections.back().skeleton = std::move(skeleton);
              }

              // Pick the best frame: prefer frame 0 (world root) unless its
              // position is near-zero, in which case walk to first non-zero
              // child
              int chosen = 0;
              glm::vec3 pos0(worldMats[0][3]);
              if (glm::length(pos0) < 0.01f && numFrames > 1) {
                for (uint32_t fi = 1; fi < numFrames; ++fi) {
                  glm::vec3 pfi(worldMats[fi][3]);
                  if (glm::length(pfi) > 0.01f) {
                    chosen = (int)fi;
                    break;
                  }
                }
              }

              const glm::mat4 &tm = worldMats[chosen];
              ClumpObject co2;
              co2.sectionName = secName;
              co2.label = "CLUMP " + std::to_string(g_Clumps.size());
              co2.position = glm::vec3(tm[3]);
              co2.transform = tm;
              co2.meshStart = -1;
              co2.meshCount = 0;
              g_Clumps.push_back(std::move(co2));
            }
          }
          break;
        }
        co += 12 + cs;
      }
    } else if (secName == "rwID_HANIMANIMATION" && objStart + 12 < sz) {
      size_t co = objStart;
      uint32_t ct = ru32(co);
      uint32_t cs = ru32(co + 4);
      if (ct == 0x001B && co + 12 + cs <= sz && cs >= 16) {
        size_t b = co + 12;
        int32_t version, typeId, frameCount, flags;
        float duration;
        memcpy(&version, &data[b], 4);
        memcpy(&typeId, &data[b + 4], 4);
        memcpy(&frameCount, &data[b + 8], 4);
        memcpy(&flags, &data[b + 12], 4);
        memcpy(&duration, &data[b + 16], 4);
        
        if (typeId == 0x1103) {
          AnimClip clip;
          // The section name is the type tag ("rwID_HANIMANIMATION"), not the
          // clip's own filename, so every clip would be called the same thing.
          // Number them until the real name is located.
          clip.name = "Clip_" + std::to_string(g_ShoSections.size() - 1);
          clip.duration = duration;
          clip.fps = 30.0f;
          
          float transOffset[3], transScalar[3];
          size_t p = b + 20;
          for (int j=0; j<3; ++j) memcpy(&transOffset[j], &data[p + j*4], 4);
          for (int j=0; j<3; ++j) memcpy(&transScalar[j], &data[p + 12 + j*4], 4);
          p += 24;
          
          size_t frameHdrBase = p;
          size_t frameDataBase = frameHdrBase + frameCount * 8;
          
          if (frameDataBase + frameCount * 12 <= co + 12 + cs) {
            // Rebuild per-bone tracks
            // The first N records are the roots of the tracks for each bone.
            // We group by tracing prevFrameByteOffset.
            // Since we don't know the exact bone count, we can group records that share the same root record.
            std::vector<int> recordToTrack(frameCount, -1);
            int trackCount = 0;
            
            for (int k = 0; k < frameCount; ++k) {
              int32_t prevOffset;
              memcpy(&prevOffset, &data[frameHdrBase + k * 8], 4);
              int prevIndex = k - (prevOffset / 20);
              
              if (prevIndex == k) { // Root of a track
                recordToTrack[k] = trackCount++;
                AnimTrack track;
                clip.tracks.push_back(track);
              } else if (prevIndex >= 0 && prevIndex < frameCount) {
                recordToTrack[k] = recordToTrack[prevIndex];
              }
            }
            
            for (int k = 0; k < frameCount; ++k) {
              int tIdx = recordToTrack[k];
              if (tIdx >= 0 && tIdx < (int)clip.tracks.size()) {
                float time;
                memcpy(&time, &data[frameHdrBase + k * 8 + 4], 4);
                
                uint32_t c1;
                uint16_t c2, tx, ty, tz;
                size_t dp = frameDataBase + k * 12;
                memcpy(&c1, &data[dp], 4);
                memcpy(&c2, &data[dp + 4], 2);
                memcpy(&tx, &data[dp + 6], 2);
                memcpy(&ty, &data[dp + 8], 2);
                memcpy(&tz, &data[dp + 10], 2);
                
                float qx = ((float)((c1 >> 20)) - 2048.0f) / 2047.0f;
                float qy = ((float)(((c1 >> 8) & 0xFFF)) - 2048.0f) / 2047.0f;
                float qz = ((float)((((c1 << 4) & 0xFFF) | (c2 >> 12))) - 2048.0f) / 2047.0f;
                float qw = ((float)((c2 & 0xFFF)) - 2048.0f) / 2047.0f;
                
                // glm::quat takes the scalar first. Passing (x, y, z, w) here
                // put -qx into w and qw into z, which is a different rotation
                // entirely. The conjugate is what the reference stores.
                glm::quat q(qw, -qx, -qy, -qz);
                
                float px = (tx / 65535.0f) * transScalar[0] + transOffset[0];
                float py = (ty / 65535.0f) * transScalar[1] + transOffset[1];
                float pz = (tz / 65535.0f) * transScalar[2] + transOffset[2];
                glm::vec3 pos(px, py, pz);
                
                clip.tracks[tIdx].times.push_back(time);
                clip.tracks[tIdx].rot.push_back(q);
                clip.tracks[tIdx].pos.push_back(pos);
              }
            }
            {
              size_t keys = 0;
              float tmin = 1e9f, tmax = -1e9f;
              for (auto &tr : clip.tracks) {
                keys += tr.times.size();
                for (float t : tr.times) { tmin = std::min(tmin, t); tmax = std::max(tmax, t); }
              }
              std::cout << "[anim] " << clip.name << "  tracks=" << clip.tracks.size()
                        << " records=" << frameCount << " keys=" << keys
                        << " duration=" << clip.duration
                        << " t=" << (keys ? tmin : 0.0f) << ".." << (keys ? tmax : 0.0f)
                        << "\n";
            }
            g_ShoSections.back().animClip = std::move(clip);
          }
        }
      }
    }

    off716 += 12 + s;
  }

  // ── 3. Upload collision mesh to GPU ───────────────────────────
  if (!g_Collision.verts.empty())
    g_Collision.Upload();

  // ── 3b. Resolve GUID references: place re-usable model sections ─
  //
  // A 0x0704 object holds the GUIDs of the sections it owns and a world
  // matrix. rwID_RWS / rwID_CLUMP sections are models. The object also
  // references its animation clips (rwID_HANIMANIMATION).
  {
    std::map<std::string, int> byGuid;
    for (size_t i = 0; i < g_ShoSections.size(); i++)
      if (!g_ShoSections[i].guid.empty())
        byGuid.emplace(g_ShoSections[i].guid, (int)i);

    int goIdx = 0;
    for (auto &go : g_GameObjects) {
      bool isVolume = (go.className == "CPhysicsObject" || 
                         go.className == "CZone" ||
                         go.className == "CWaterZone" ||
                         go.className == "CCameraZone" ||
                         go.className == "CCameraArea");
                         
        for (const auto &ref : go.guidRefs) {
          auto it = byGuid.find(ref);
          if (it == byGuid.end())
            continue;
            
          int secIdx = it->second;
          ShoSection &sec = g_ShoSections[secIdx];
          
          if (sec.name == "rwID_HANIMANIMATION" || sec.name == "rwID_DMORPHANIMATION") {
            go.clipSectionIndices.push_back(secIdx);
            continue;
          }

          if (sec.isWorldSpace)
            continue; // already in level space
            
          // Use the matrix as a placement transform only if it's not a volume extent
          if (!isVolume) {
            sec.instances.push_back({go.transform, goIdx});
          }
        }
        goIdx++;
      }
  }

  // ── 3c. Camera objects ────────────────────────────────────────
  // The game places fixed cameras in the level and cuts between them; the
  // object's matrix is the camera's world transform.
  g_Cameras.clear();
  for (const auto &go : g_GameObjects) {
    const bool isCam = go.className.find("Camera") != std::string::npos;
    if (!isCam || go.atOrigin)
      continue;
    LevelCamera cam;
    cam.name = go.instName.empty() ? go.className : go.instName;
    cam.position = go.position;
    // Column 2 is the look direction, column 1 is up (RenderWare convention).
    cam.forward = glm::normalize(glm::vec3(go.transform[2]));
    glm::vec3 up = glm::vec3(go.transform[1]);
    cam.up =
        (glm::length(up) > 1e-4f) ? glm::normalize(up) : glm::vec3(0, 1, 0);
    g_Cameras.push_back(std::move(cam));
  }

  // ── 4. Summary ────────────────────────────────────────────────
  {
    size_t placed = 0;
    for (const auto &go : g_GameObjects)
      if (!go.atOrigin)
        placed++;
    uint32_t declared = 0;
    for (const auto &t : g_ShoTypes)
      declared += t.count;
    size_t modelSecs = 0, instances = 0, orphans = 0;
    for (const auto &s : g_ShoSections) {
      if (s.isWorldSpace)
        continue;
      modelSecs++;
      instances += s.instances.size();
      if (s.instances.empty())
        orphans++;
    }
    std::cerr << "[container] " << g_ShoSections.size() << " sections, "
              << g_GameObjects.size() << "/" << declared << " game objects ("
              << placed << " placed), " << g_Cameras.size() << " cameras, "
              << modelSecs << " model sections -> " << instances
              << " instances (" << orphans << " unreferenced), "
              << g_Collision.verts.size() << " collision verts\n";
  }

  // ── 5. Legacy g_ContainerChunks — fill from g_ShoTypes (for UI) ─
  g_ContainerChunks.clear();
  for (auto &te : g_ShoTypes) {
    ContainerChunkInfo ci;
    ci.offset = 0;
    ci.typeId = 0;
    ci.label = te.name + " ×" + std::to_string(te.count);
    g_ContainerChunks.push_back(ci);
  }
}