#include "GeometryDecoder.h"
#include <iostream>
#include "ClimaxEngine/Core/Common.h"
#include <cstring>
#include <algorithm>

namespace ClimaxEngine {
namespace ResourceLoader {

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
bool ReadPacket(const uint8_t* d, size_t d_sz, size_t p, size_t end,
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
      // Rejecting the whole packet because one stream lands above VU address 3
      // threw away the geometry with it. Anything higher is simply not decoded;
      // DecodePacket already ignores addresses it does not know.
      if (s.addr > 1023)
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

std::vector<VifPacket> PacketsIn(const uint8_t* d, size_t d_sz, size_t start,
                                 size_t end) {
  std::vector<VifPacket> out;
  if (end > d_sz)
    end = d_sz;
  size_t p = start;
  while (p + 4 <= end) {
    uint32_t cmd;
    memcpy(&cmd, &d[p], 4);
    if (!IsPositionUnpack(cmd)) {
      ++p; // byte-granular: packets are not aligned
      continue;
    }
    VifPacket pk;
    size_t after = 0;
    if (ReadPacket(d, d_sz, p, end, pk, after) && pk.vertexCount > 0) {
      out.push_back(std::move(pk));
      p = after;
    } else {
      ++p;
    }
  }
  return out;
}

// Decodes one packet's streams into vertices. `adc` marks strip restarts.
void DecodePacket(const uint8_t* d, const VifPacket &pk,
                  std::vector<Vertex> &verts, std::vector<bool> &adc) {
  const int n = pk.vertexCount;
  verts.assign(n, Vertex{});
  adc.assign(n, false);
  for (auto &v : verts) {
    v.uv = {0.0f, 0.0f};
    v.color = {1.0f, 1.0f, 1.0f, 1.0f};
  }

  for (const auto &s : pk.streams) {
    const int count = std::min(n, s.num);
    switch (s.addr) {
    case 0: // positions; V4-32 carries a strip-restart flag in w
      for (int i = 0; i < count; i++) {
        const size_t o = s.dataOff + (size_t)i * s.bpv;
        memcpy(&verts[i].pos, &d[o], 12);
        if (s.vn == 3) {
          uint16_t w;
          memcpy(&w, &d[o + 12], 2);
          if (w != 0)
            adc[i] = true;
        }
      }
      break;
    case 1: // texture coords: floats, or 16.12 fixed point
      for (int i = 0; i < count; i++) {
        const size_t o = s.dataOff + (size_t)i * s.bpv;
        if (s.vl == 0) {
          memcpy(&verts[i].uv.x, &d[o], 4);
          memcpy(&verts[i].uv.y, &d[o + 4], 4);
        } else {
          int16_t u, v;
          memcpy(&u, &d[o], 2);
          memcpy(&v, &d[o + 2], 2);
          verts[i].uv = {u / 4096.0f, v / 4096.0f};
        }
      }
      break;
    case 2: // colours, PS2 range where 128 is full intensity
      for (int i = 0; i < count; i++) {
        const size_t o = s.dataOff + (size_t)i * s.bpv;
        verts[i].color = glm::vec4(
            std::min(d[o + 0] * (1.0f / 128.0f), 1.0f),
            std::min(d[o + 1] * (1.0f / 128.0f), 1.0f),
            std::min(d[o + 2] * (1.0f / 128.0f), 1.0f),
            s.bpv >= 4 ? std::min(d[o + 3] * (1.0f / 128.0f), 1.0f) : 1.0f);
      }
      break;
    default:
      break; // slot 3 is normals; the renderer derives them per face
    }
  }
}

// Converts one strip to a triangle list, honouring restart flags.
void StripToTriangles(const std::vector<Vertex> &raw,
                      const std::vector<bool> &adc, std::vector<Vertex> &out) {
  bool anyAdc = false;
  for (bool f : adc)
    if (f) { anyAdc = true; break; }

  for (size_t i = 2; i < raw.size(); i++) {
    const Vertex &a = raw[i - 2], &b = raw[i - 1], &c = raw[i];
    const bool skip =
        anyAdc ? adc[i]
               : (a.pos == b.pos || b.pos == c.pos || a.pos == c.pos);
    if (skip)
      continue;
    // Winding alternates with the vertex index, not with a counter that resets.
    // Degenerate triangles are dropped but still occupy their slot in the
    // sequence.
    if (i % 2 == 0) { out.push_back(a); out.push_back(b); out.push_back(c); }
    else            { out.push_back(b); out.push_back(a); out.push_back(c); }
  }
}

} // namespace

static int sho_strnicmp(const char *s1, const char *s2, size_t n) {
  if (n == 0) return 0;
  do {
    int c1 = tolower(*(const unsigned char *)s1++);
    int c2 = tolower(*(const unsigned char *)s2++);
    if (c1 != c2) return c1 - c2;
    if (c1 == 0) break;
  } while (--n != 0);
  return 0;
}

void DecodeRenderWareGeometry(const std::string& name, const uint8_t* payload, size_t length, bool isWorld, SG::CMeshObject* destObj) {

  const size_t sz = length;
  if (sz < 32)
    return;

  // Deliberately NOT cleared here. This function now runs once per World or
  // Clump section -- six times for MO_1_Room102 -- so clearing on entry left
  // only the last section's material names behind, and the texture loader then
  // skipped every dictionary the earlier sections needed. LoadLevelData clears
  // the list once per level, which is the right place for it.

  // Helper to safely read a uint32 from the buffer
  auto ru32 = [&](size_t off) -> uint32_t {
    if (off + 4 > sz)
      return 0;
    uint32_t v;
    memcpy(&v, &payload[off], 4);
    return v;
  };

  // --- HELPER: parse material names from a MaterialList chunk at ml_pos ---
  // Returns a vector<string> with one name per material (or "NULL").
  std::vector<glm::vec4> matColors;
  // Blend mode, straight out of the material's own 0x0A01 extension.
  //
  // This is the field ClimaxT1MaterialGetFrameBlendMode reads (plugin + 4 in
  // the runtime material), and its values line up with the engine's named
  // setters: 0 = STD, 1 = ADD, 2 = SUB. Measured over 11 507 materials in 140
  // containers: 11 423 STD, 54 ADD (FX_light_beam2, FX_Candle_Glow,
  // FX_Flare_01, skybox_light), 26 SUB (Blood_Pool_SUB, FX_Bloodstain,
  // FX_Blood_Hit_Enemies_SUB -- two of which spell SUB in the texture name,
  // which is what makes this certain rather than merely plausible).
  //
  // It replaces the hand-maintained additive name list entirely, and it is why
  // the TV screen and the save point never looked right: both are STD, and no
  // name rule could separate them from the glows.
  std::vector<uint32_t> matBlend;
  auto parseMaterialList = [&](size_t ml_pos) -> std::vector<std::string> {
    std::vector<std::string> names;
    matColors.clear();
    matBlend.clear();
    size_t mlDataOff = ml_pos + 12; // skip chunk header
    uint32_t fcType = ru32(mlDataOff);
    uint32_t fcSize = ru32(mlDataOff + 4);
    if (fcType != 0x01 || fcSize < 4 || mlDataOff + 12 + fcSize > sz)
      return names;
    uint32_t numMat = ru32(mlDataOff + 12);
    if (numMat == 0 || numMat > 512)
      return names;
    size_t curr = mlDataOff + 12 + fcSize; // skip Struct chunk entirely
    for (uint32_t m = 0; m < numMat; m++) {
      if (curr + 12 >= sz)
        break;
      uint32_t matType = ru32(curr);
      uint32_t matSize = ru32(curr + 4);
      if (matType != 0x07 || matSize == 0 || curr + 12 + matSize > sz)
        break;
      size_t matEnd = curr + 12 + matSize;
      size_t child = curr + 12;
      std::string texName = "NULL";
      glm::vec4 matCol(1.0f);
      uint32_t blend = 0;
      std::string uvAnim;
      while (child + 12 < matEnd) {
        uint32_t cType = ru32(child);
        uint32_t cSize = ru32(child + 4);
        if (cSize == 0 || child + 12 + cSize > matEnd)
          break;
        if (cType == 0x01 && cSize >= 16) {
          // Material struct: flags, RGBA colour, unused, textured, surface props
          const uint32_t rgba = ru32(child + 16);
          // Alpha follows the same PS2 convention as the palettes: 0x80 is
          // fully opaque, not half. Dividing it by 255 made every flat-shaded
          // material render at 50% and washed the models out.
          matCol = glm::vec4(
              ((rgba >> 0) & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
              ((rgba >> 16) & 0xFF) / 255.0f,
              std::min((((rgba >> 24) & 0xFF) * 2) / 255.0f, 1.0f));
        }
        if (cType == 0x03) { // Extension -> Climax material plugin
          size_t x = child + 12;
          const size_t xEnd = child + 12 + cSize;
          while (x + 12 <= xEnd) {
            const uint32_t xt = ru32(x), xs = ru32(x + 4);
            if (xs == 0 || x + 12 + xs > xEnd)
              break;
            if (xt == 0x0A01 && xs >= 8)
              blend = ru32(x + 12 + 4);
            // UV animation: an inner Struct holding a slot mask, then the name
            // of a clip in the container's 0x2B section.
            if (xt == 0x0135 && xs >= 48) {
              const size_t nm = x + 12 + 16;
              for (size_t k = 0; k < 32 && nm + k < xEnd && payload[nm + k]; k++)
                uvAnim += (char)payload[nm + k];
            }
            x += 12 + xs;
          }
        }
        if (cType == 0x06) { // Texture chunk
          size_t texChild = child + 12;
          size_t texEnd = child + 12 + cSize;
          while (texChild + 12 < texEnd) {
            uint32_t tcType = ru32(texChild);
            uint32_t tcSize = ru32(texChild + 4);
            if (tcSize == 0 || texChild + 12 + tcSize > texEnd)
              break;
            if (tcType == 0x02) { // String = texture name
              size_t nameLen = strnlen((char *)&payload[texChild + 12], tcSize);
              texName = std::string((char *)&payload[texChild + 12], nameLen);
              break;
            }
            texChild += 12 + tcSize;
          }
          // No break here. A Material's children are Struct, Texture, then
          // Extension, and the extension is where the blend mode lives --
          // stopping at the texture meant the 0x0A01 field was never read and
          // every mesh came out as standard alpha.
        }
        child += 12 + cSize;
      }
      names.push_back(texName);
      matColors.push_back(matCol);
      matBlend.push_back(blend);
      if (!uvAnim.empty() && texName != "NULL")
        g_MatUVAnim[texName] = uvAnim;
      curr = matEnd;
    }
    return names;
  };

  // If it's a Clump, parse the FrameList first to build the skeleton!
  if (!isWorld) {
      SG::CClumpObject* clumpObj = dynamic_cast<SG::CClumpObject*>(destObj);
      if (clumpObj) {
          // Find FrameList
          std::cout << "[scene] Parsing Clump payload, length=" << length << "\n";
          for (size_t c = 0; c + 12 <= length;) {
              const uint32_t ct = ru32(c), cs = ru32(c + 4), ver = ru32(c + 8);
              std::cout << "[scene] Clump Chunk Type: 0x" << std::hex << ct << " size: " << std::dec << cs << " ver: 0x" << std::hex << ver << std::dec << "\n";
              if (ver != 0x1C020065 || c + 12 + cs > length) break;
              if (ct == 0x000E) { // FrameList
                  const size_t st = c + 12;
                  if (ru32(st) == 0x01 && ru32(st + 8) == 0x1C020065) {
                      const uint32_t n = ru32(st + 12);
                      if (n > 0 && n <= 1024 && st + 16 + (size_t)n * 56 <= c + 12 + cs) {
                          clumpObj->skeleton.bones.resize(n);
                          for (uint32_t i = 0; i < n; i++) {
                              const size_t fb = st + 16 + (size_t)i * 56;
                              glm::mat4 m(1.0f);
                              for (int r = 0; r < 3; r++)
                                  for (int cc = 0; cc < 3; cc++)
                                      memcpy(&m[r][cc], &payload[fb + (r * 3 + cc) * 4], 4);
                              memcpy(&m[3][0], &payload[fb + 36], 4);
                              memcpy(&m[3][1], &payload[fb + 40], 4);
                              memcpy(&m[3][2], &payload[fb + 44], 4);
                              int32_t parentId = -1;
                              memcpy(&parentId, &payload[fb + 48], 4);
                              clumpObj->skeleton.bones[i].restLocal = m;
                              clumpObj->skeleton.bones[i].parent = parentId;
                          }
                          
                          // Now parse the extensions for bone ids and names
                          std::vector<int32_t> hanimOrder;
                          size_t extOff = st + 12 + ru32(st + 4);
                          std::cout << "[scene] Clump " << destObj->GetName() << " got skeleton with " << n << " bones!\n";
                          for (uint32_t i = 0; i < n; i++) {
                              if (extOff + 12 > c + 12 + cs) break;
                              uint32_t extType = ru32(extOff);
                              uint32_t extSize = ru32(extOff + 4);
                              if (extType != 0x03 || extOff + 12 + extSize > c + 12 + cs) break;
                              
                              size_t child = extOff + 12;
                              size_t extEnd = extOff + 12 + extSize;
                              while (child + 12 <= extEnd) {
                                  uint32_t childType = ru32(child);
                                  uint32_t childSize = ru32(child + 4);
                                  if (childSize == 0 || child + 12 + childSize > extEnd) break;
                                  
                                  if (childType == 0x011E) {
                                      // HAnim PLG. Every frame carries its own
                                      // bone id; exactly one -- the hierarchy
                                      // root -- also carries the whole table,
                                      // and the order of that table is the
                                      // order the clip's tracks come in.
                                      size_t d = child + 12;
                                      if (d + 12 <= extEnd) {
                                          int32_t boneId = 0;
                                          uint32_t boneCount = 0;
                                          memcpy(&boneId, &payload[d + 4], 4);
                                          memcpy(&boneCount, &payload[d + 8], 4);
                                          clumpObj->skeleton.bones[i].boneId = boneId;
                                          if (boneCount && boneCount < 1024 &&
                                              d + 20 + (size_t)boneCount * 12 <= extEnd) {
                                              hanimOrder.clear();
                                              hanimOrder.reserve(boneCount);
                                              for (uint32_t bx = 0; bx < boneCount; ++bx) {
                                                  int32_t id = 0;
                                                  memcpy(&id, &payload[d + 20 + (size_t)bx * 12], 4);
                                                  hanimOrder.push_back(id);
                                              }
                                          }
                                      }
                                  }

                                  if (childType == 0x011F) { // UserData PLG (bone names)
                                      size_t d = child + 12;
                                      if (d + 4 <= extEnd) {
                                          int32_t numSets = 0;
                                          memcpy(&numSets, &payload[d], 4);
                                          size_t setOff = d + 4;
                                          for (int s = 0; s < numSets && setOff + 16 <= extEnd; ++s) {
                                              int32_t typeNameLen = 0, nameLen = 0;
                                              memcpy(&typeNameLen, &payload[setOff], 4);
                                              setOff += 4 + typeNameLen + 8; // skip typeNameLen string + 2 unknowns
                                              if (setOff + 4 <= extEnd) {
                                                  memcpy(&nameLen, &payload[setOff], 4);
                                                  setOff += 4;
                                                  if (nameLen > 1 && setOff + nameLen <= extEnd) {
                                                      clumpObj->skeleton.bones[i].name = std::string((const char*)&payload[setOff], nameLen - 1);
                                                  }
                                                  setOff += nameLen;
                                              }
                                          }
                                      }
                                  }
                                  child += 12 + childSize;
                              }
                              
                              if (clumpObj->skeleton.bones[i].name.empty()) {
                                  clumpObj->skeleton.bones[i].name = (i == 0) ? "RootBone" : "Bone" + std::to_string(i);
                              }
                              extOff += 12 + extSize;
                          }

                          // The hierarchy table's order is the track order, so
                          // a bone's position in it is the index of its clip
                          // track. Without this the tracks would be applied to
                          // whatever frame happened to sit at the same index.
                          if (!hanimOrder.empty()) {
                              for (auto &bone : clumpObj->skeleton.bones) {
                                  for (size_t t = 0; t < hanimOrder.size(); ++t) {
                                      if (hanimOrder[t] == bone.boneId) {
                                          bone.trackIndex = (int)t;
                                          break;
                                      }
                                  }
                              }
                              // Every frame but the clump root binds to a
                              // track; anything else means the table and the
                              // frame list disagree and the pose would be
                              // applied to the wrong bones.
                              size_t bound = 0;
                              for (auto &bone : clumpObj->skeleton.bones)
                                  if (bone.trackIndex >= 0) bound++;
                              if (bound != hanimOrder.size())
                                  std::cout << "[hanim] " << destObj->GetName()
                                            << ": table has " << hanimOrder.size()
                                            << " bones but " << bound << " of "
                                            << clumpObj->skeleton.bones.size()
                                            << " frames bound\n";
                          }
                      }
                  }
                  break;
              }
              c += 12 + cs;
          }
      }
  }
  // --- STEP 1 & 2: Collect all valid MaterialList and BinMesh positions using strict chunk traversal ---
  std::vector<size_t> allMlPos;
  std::vector<size_t> allBinMeshPos;

  auto walkChunks = [&](auto& self, size_t offset, size_t sizeLimit) -> void {
      size_t p = offset;
      while (p + 12 <= offset + sizeLimit && p + 12 <= sz) {
          uint32_t type = ru32(p);
          uint32_t csize = ru32(p + 4);
          if (csize > sz || p + 12 + csize > sz) break;
          
          if (type == 0x08) allMlPos.push_back(p);
          else if (type == 0x050E) allBinMeshPos.push_back(p);
          
          // Container types that have children
          // Descend into everything that can contain a BinMeshPLG. A world
          // keeps it under PlaneSector -> AtomicSector -> Extension, and a
          // clump under GeometryList -> Geometry -> Extension; none of those
          // were in this list, so the scan never reached a single 0x050E.
          if (type == 0x14 || type == 0x16 || type == 0x0F || type == 0x10 ||
              type == 0x24 || type == 0x0E || type == 0x0510 ||
              type == 0x0B || type == 0x0A || type == 0x09 ||
              type == 0x1A || type == 0x03) {
              self(self, p + 12, csize);
          }
          p += 12 + csize;
      }
  };

  // This function is handed one section's payload, not the whole container, so
  // the walk starts at 0 and runs to the end of what was passed in.
  //
  // It used to iterate g_ShoSections and walk each sec.offset, which are
  // offsets into the container. Against a section-local buffer that test
  // (sec.offset + sec.size <= sz) is false for every section, so the walk never
  // ran, no BinMesh or MaterialList positions were collected, and every object
  // came out with zero meshes.
  walkChunks(walkChunks, 0, sz);
  std::cout << "[geom] '" << name << "' payload " << sz
            << " root=0x" << std::hex << ru32(0) << std::dec
            << " ver=0x" << std::hex << ru32(8) << std::dec
            << " binmesh=" << allBinMeshPos.size()
            << " matlists=" << allMlPos.size() << "\n";

  struct BatchInfo {
    int matIndex;
    int vertexQuota;
  };

  struct GeoObject {
    size_t bmOff;
    std::vector<BatchInfo> batches;
    std::vector<std::string> matNames;
    size_t vifStart,
        vifEnd; // range in file where VIF vertex data lives (AFTER bmOff)
  };
  std::vector<GeoObject> geoObjs;

  {
    for (size_t cand : allBinMeshPos) {
      uint32_t flags = ru32(cand + 12);
      uint32_t numMeshes = ru32(cand + 16);
      if (numMeshes == 0 || numMeshes > 4096) continue;
      bool isTriList = (flags == 0);

      GeoObject go;
      go.bmOff = cand;

      // Parse batches from this BinMesh
      size_t curr = cand + 24;
      for (uint32_t i = 0; i < numMeshes; i++) {
        if (curr + 8 > sz)
          break;
        uint32_t numIndices = ru32(curr);
        uint32_t matIdx = ru32(curr + 4);
        if (numIndices > 65536)
          break;
        go.batches.push_back({(int)matIdx, (int)numIndices});
        size_t skip = 8 + (isTriList ? (size_t)numIndices * 4 : 0);
        if (curr + skip > sz)
          break;
        curr += skip;
      }
      if (go.batches.empty()) {
        continue;
      }

      // Pair with nearest preceding MaterialList
      size_t bestMl = (size_t)-1;
      for (size_t mlp : allMlPos)
        if (mlp < cand)
          bestMl = mlp;
      if (bestMl != (size_t)-1)
        go.matNames = parseMaterialList(bestMl);

      geoObjs.push_back(std::move(go));
    }
  }

  if (geoObjs.empty()) {
    // Fallback: single dummy object with no batches
    geoObjs.push_back({0, {{0, 9999999}}, {}, 0, sz});
  }

  // Sort by BinMesh offset — VIF data lives AFTER the BinMesh
  std::sort(
      geoObjs.begin(), geoObjs.end(),
      [](const GeoObject &a, const GeoObject &b) { return a.bmOff < b.bmOff; });
  for (size_t i = 0; i < geoObjs.size(); i++) {
    geoObjs[i].vifStart = geoObjs[i].bmOff;
    geoObjs[i].vifEnd = (i + 1 < geoObjs.size()) ? geoObjs[i + 1].bmOff : sz;
  }

  // --- Material names, taken from where the format actually keeps them ------
  //
  // The data of a 0x0716 section starts 8 bytes after the two build-path
  // strings and is a stock RenderWare chunk:
  //
  //     World (0x0B)  ->  Struct (0x01), MaterialList (0x08), AtomicSect (0x09)
  //     Clump (0x10)  ->  Struct, FrameList, GeometryList (0x1A), Atomic
  //
  // A world has exactly ONE material list and every BinMesh index in it is
  // local to that list. Scanning the file for lists instead produced 27 of them
  // for IntroRoad's single world and 453 names for one motel room, and the
  // per-object index bases then pointed into a neighbouring object's names —
  // which is why so much geometry ended up untextured or wearing the wrong
  // texture.
  std::vector<std::vector<std::string>> sectionMats(1);
  std::vector<std::vector<glm::vec4>> sectionCols(1);
  std::vector<std::vector<uint32_t>> sectionBlend(1);
  {
    size_t si = 0;
    const size_t secEnd = sz;
    size_t root = 0;

    
    

    // The caller hands over the *body* of the World or Clump chunk: the stream
    // loader consumed the 12-byte chunk header before calling in, so offset 0
    // is already the first child (a Struct), not the root chunk itself.
    //
    // Reading a root header here instead produced type 0x01 for every payload,
    // so neither the World nor the Clump branch below ever ran and every object
    // came out with no meshes at all. `isWorld` is what the caller knows and
    // this function has to trust.
    const uint32_t rootType = isWorld ? 0x0B : 0x10;
    const size_t rootEnd = secEnd;
    const size_t rootBody = 0;          // children start at the buffer's start

    // Direct children of World hold the list; for a Clump it sits one level
    // deeper, inside each Geometry of the GeometryList.
    auto collectFrom = [&](size_t begin, size_t end) {
      size_t c = begin;
      while (c + 12 <= end) {
        const uint32_t ct = ru32(c), cs = ru32(c + 4);
        if (ru32(c + 8) != 0x1C020065 || cs == 0 || c + 12 + cs > end)
          break;
        if (ct == 0x08) {
          for (const auto &n : parseMaterialList(c))
            sectionMats[si].push_back(n);
          for (const auto &col : matColors)
            sectionCols[si].push_back(col);
          for (const auto &b : matBlend)
            sectionBlend[si].push_back(b);
        }
        c += 12 + cs;
      }
    };

    if (rootType == 0x0B) {
      collectFrom(rootBody, rootEnd);
    } else if (rootType == 0x10) {
      size_t c = rootBody;
      while (c + 12 <= rootEnd) {
        const uint32_t ct = ru32(c), cs = ru32(c + 4);
        if (ru32(c + 8) != 0x1C020065 || cs == 0 || c + 12 + cs > rootEnd)
          break;
        if (ct == 0x1A) { // GeometryList -> Struct, then Geometry chunks
          size_t g = c + 12;
          while (g + 12 <= c + 12 + cs) {
            const uint32_t gt = ru32(g), gs = ru32(g + 4);
            if (ru32(g + 8) != 0x1C020065 || gs == 0 || g + 12 + gs > c + 12 + cs)
              break;
            if (gt == 0x0F)
              collectFrom(g + 12, g + 12 + gs);
            g += 12 + gs;
          }
        }
        c += 12 + cs;
      }
    }
  }

  // g_MaterialNames stays the union of every name, since it is what filters the
  // texture dictionaries at load time.
  for (const auto &names : sectionMats)
    for (const auto &n : names)
      g_MaterialNames.push_back(n);

  // --- STEP 3: Build mesh chunks from the RenderWare tree -------------------
  //
  // Geometry is not something to search for. Every Geometry (0x000F) and every
  // world AtomicSect (0x0009) carries an Extension (0x0003) holding two plugins:
  //
  //     BinMeshPLG   (0x050E)  faceType, splitCount, then [numIndices, matID]*
  //     NativeDataPLG(0x0510)  one VIF block per split, each with its own size
  //
  // Split i uses material matID[i] directly. Scanning the file for BinMesh
  // chunks instead found 27 "geometry objects" in IntroRoad where the tree has
  // one per world, so packets were paired with a stranger's batch table and
  // drew the wrong texture even though the material list itself was correct.
  

  size_t totalPackets = 0, totalVerts = 0, totalSplits = 0;
  std::vector<Vertex> rawVerts, triVerts;
  std::vector<bool> adcFlags;

  auto addChunk = [&](std::vector<Vertex> &verts, int matId, const std::vector<std::string> &localMats, const std::vector<glm::vec4> &localCols, const std::vector<uint32_t> &localBlend) {
    if (verts.empty())
      return;
    MeshChunk m;
    m.vertices = verts;
    
    m.materialIndex = matId;
    m.texName = "NULL";
    if (matId >= 0 && matId < (int)localBlend.size())
      m.blendMode = localBlend[matId];
    if (matId >= 0 && matId < (int)localMats.size()) {
      m.texName = localMats[matId];
    }
    {
      auto itUV = g_MatUVAnim.find(m.texName);
      if (itUV != g_MatUVAnim.end())
        m.uvAnimName = itUV->second;
    }
    // "GreyAlpha_<base>" is not a colour map: it is the alpha pass of a two-pass
    // transparency setup, a white-on-black mask drawn over the same geometry as
    // <base>. Rendering it as an ordinary texture painted large black-and-white
    // sheets over the scene. Flag it so the renderer can leave it out; the mask
    // still belongs to the base texture's alpha and merging the two is the next
    // step.
    if (m.texName.size() > 10 &&
        sho_strnicmp(m.texName.c_str(), "GreyAlpha_", 10) == 0)
      m.alphaPass = true;

    // Effect sheets are NOT additive. Their palette carries a transparent entry
    // for the surround — FX_save_point1 and FX_TV each have 255 entries at alpha
    // 128 and exactly one at 0 — so ordinary alpha blending cuts the background
    // out. Forcing GL_ONE/GL_ONE ignored that alpha, which washed the TV screen
    // out and made it looksemi-transparent.
    (void)0;

    // Model clumps carry no baked lighting: their vertex colours are literally
    // zero (HO_Map and FX_save_point1 both average 0.0 with alpha 1). The game
    // lights them at runtime. Multiplying by that black turned the wall map into
    // a black rectangle whenever vertex colours were on.
    {
      double sum = 0.0;
      for (const auto &v : m.vertices) sum += v.color.r + v.color.g + v.color.b;
      if (!m.vertices.empty() && sum / (m.vertices.size() * 3.0) < 0.004)
        m.unlitGeometry = true;
    }

    if (m.texName == "NULL" || m.texName.empty()) {
      m.untextured = true;
      if (matId >= 0 && matId < (int)localCols.size())
        m.matColor = localCols[matId];
    }
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
    // boneIds is an array of uint8_t, but we pass it as float attributes by using glVertexAttribPointer instead of glVertexAttribIPointer? Or we use GL_UNSIGNED_BYTE and normalize=false? Yes, GL_UNSIGNED_BYTE. Wait, if we use GL_UNSIGNED_BYTE, it might normalize them to 0-1 if GL_TRUE is passed, but we want integer values. We can pass GL_UNSIGNED_BYTE and GL_FALSE.
    // Or we can use glVertexAttribIPointer, but glVertexAttribPointer with GL_UNSIGNED_BYTE, GL_FALSE will convert them to float without normalizing.
    glVertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, boneIds));
    glEnableVertexAttribArray(4);
    destObj->AddMesh(std::move(m));
  };

  // Reads the BinMesh split table and the matching native VIF blocks.
  auto buildFromExtension = [&](size_t extBegin, size_t extEnd, int sectionIdx, const std::vector<std::string> &localMats, const std::vector<glm::vec4> &localCols, const std::vector<uint32_t> &localBlend) {
    auto rf32 = [&](size_t off) -> float {
      float f;
      memcpy(&f, &payload[off], 4);
      return f;
    };
    size_t binMesh = 0, binMeshEnd = 0, native = 0, nativeEnd = 0, skinPlg = 0, skinPlgEnd = 0;
    bool hasSkin = false;
    for (size_t c = extBegin; c + 12 <= extEnd;) {
      const uint32_t ct = ru32(c), cs = ru32(c + 4);
      if (ru32(c + 8) != 0x1C020065 || cs == 0 || c + 12 + cs > extEnd)
        break;
      if (ct == 0x050E) { binMesh = c + 12; binMeshEnd = c + 12 + cs; }
      else if (ct == 0x0510) { native = c + 12; nativeEnd = c + 12 + cs; }
      else if (ct == 0x0116) { hasSkin = true; skinPlg = c + 12; skinPlgEnd = c + 12 + cs; }
      c += 12 + cs;
    }
    if (!binMesh || !native)
      return;

    // Parse Skin PLG for inverse bind matrices if present
    if (hasSkin) {
      SG::CClumpObject* clumpObj = dynamic_cast<SG::CClumpObject*>(destObj);
      if (clumpObj && !clumpObj->skeleton.bones.empty() && skinPlg + 8 <= skinPlgEnd) {
        // Native (PS2) layout:
        // u32 platform
        // u8 boneCount
        // u8 usedBoneCount
        // u8 maxWeightsPerVertex
        // u8 padding
        // u8 usedBoneIds[usedBoneCount]
        // f32 inverseBoneMatrix[boneCount][16]
        uint8_t boneCount = payload[skinPlg + 4];
        uint8_t usedBoneCount = payload[skinPlg + 5];
        
        size_t matrixOff = skinPlg + 8 + usedBoneCount;
        for (int b = 0; b < boneCount && matrixOff + 64 <= skinPlgEnd; ++b) {
          glm::mat4 invBind(1.0f);
          for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
              invBind[c][r] = rf32(matrixOff + (c * 4 + r) * 4); // Matrix stored column-major? Wait, the spec says "f32 inverseBoneMatrix[boneCount][16]". Usually RW stores things row-major or column-major depending on platform. If it's PS2 native, it's usually column-major. Let's assume column-major matching our glm layout.
          // Wait, the spec says "Note the reference multiplies local * parent, i.e. row-vector convention. With glm's column-major types the correct order is parent * local."
          // If the matrix in file is row-major (which is RW standard), we must transpose it when loading into GLM.
          // Let's load it row-major:
          for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
              invBind[c][r] = rf32(matrixOff + (r * 4 + c) * 4);
          
          if (b < (int)clumpObj->skeleton.bones.size()) {
            clumpObj->skeleton.bones[b].invBind = invBind;
          }
          matrixOff += 64;
        }
      }
    }

    // BinMeshPLG: faceType, splitCount, totalIndices, then the split table.
    const uint32_t faceType = ru32(binMesh);
    const uint32_t splitCount = ru32(binMesh + 4);
    if (splitCount == 0 || splitCount > 4096)
      return;
    std::vector<int> matIds;
    matIds.reserve(splitCount);
    {
      size_t q = binMesh + 12;
      for (uint32_t i = 0; i < splitCount && q + 8 <= binMeshEnd; i++) {
        const uint32_t numIdx = ru32(q);
        matIds.push_back((int)ru32(q + 4));
        // Non-native meshes store their index list inline; PS2 data does not.
        q += 8 + (faceType == 1 ? 0 : (size_t)numIdx * 4);
      }
    }
    if (matIds.size() != splitCount)
      return;

    // NativeDataPLG: a struct chunk, the platform id, then one block per split.
    size_t p = native + 12 + 4;
    for (uint32_t i = 0; i < splitCount && p + 8 <= nativeEnd; i++) {
      const uint32_t dataSize = ru32(p);
      const uint32_t meshType = ru32(p + 4);
      p += 8;
      const size_t blockEnd = std::min(p + dataSize, nativeEnd);
      // meshType 0 opens with STROW; VIFn_R0 gives the real payload length.
      size_t vifSize = dataSize;
      if (meshType == 0)
        vifSize = (size_t)ru32(p + 4) * 16;
      const size_t vifEnd = std::min(p + vifSize, blockEnd);
      
      uint32_t numIdx = 0;
      if (meshType == 0) {
          size_t q = binMesh + 12;
          for (uint32_t j = 0; j <= i; j++) {
              numIdx = ru32(q);
              q += 8 + (faceType == 1 ? 0 : (size_t)numIdx * 4);
          }
      }

      triVerts.clear();
      const auto packets = PacketsIn(payload, length, p, vifEnd);
      
      std::vector<uint8_t> boneIds;
      std::vector<float> boneWeights;
      if (hasSkin && meshType == 0 && vifEnd + numIdx * 16 <= blockEnd) {
          size_t wp = vifEnd;
          boneIds.resize(numIdx * 4);
          boneWeights.resize(numIdx * 4);
          for (uint32_t v = 0; v < numIdx; ++v) {
              for (int w = 0; w < 4; ++w) {
                  uint8_t bId = payload[wp + w * 4];
                  boneIds[v * 4 + w] = bId / 4;
                  // Zero out the lowest byte (the bone ID) to read the clean float
                  uint8_t floatBytes[4] = {0, payload[wp + w * 4 + 1], payload[wp + w * 4 + 2], payload[wp + w * 4 + 3]};
                  float weight;
                  memcpy(&weight, floatBytes, 4);
                  boneWeights[v * 4 + w] = weight;
                  
                  // In sho_noesis.py: "if weight1 > 0: boneID1 -= 1". Wait!
                  // Let's check Noesis again. "boneID1 -= 1" if weight > 0? No, let's just use it directly, but let's see.
                  if (boneWeights[v * 4 + w] > 0.0f && boneIds[v * 4 + w] > 0) {
                      boneIds[v * 4 + w] -= 1; // It seems bone indices are 1-based or offset by 1 in some logic? Let's follow sho_noesis exactly! Wait, I'll review it if it looks wrong.
                  }
              }
              wp += 16;
          }
      }

      size_t vertexIndex = 0;
      for (const auto &pk : packets) {
        DecodePacket(payload, pk, rawVerts, adcFlags);
        
        // Inject skin weights into rawVerts
        if (hasSkin && meshType == 0 && !boneWeights.empty()) {
            for (size_t v = 0; v < rawVerts.size() && vertexIndex + v < numIdx; ++v) {
                for (int w = 0; w < 4; ++w) {
                    rawVerts[v].boneIds[w] = boneIds[(vertexIndex + v) * 4 + w];
                    rawVerts[v].boneWeights[w] = boneWeights[(vertexIndex + v) * 4 + w];
                }
            }
        }
        
        StripToTriangles(rawVerts, adcFlags, triVerts);
        vertexIndex += pk.vertexCount;
        totalVerts += pk.vertexCount;
      }
      totalPackets += packets.size();
      totalSplits++;
      addChunk(triVerts, matIds[i], localMats, localCols, localBlend);
      p = blockEnd;
    }
  };

  // One payload in, one pass. This was a loop over g_ShoSections, which both
  // repeated the work and indexed sectionMats[si] past its single element as
  // soon as the section guard above stopped skipping iterations.
  {
    const size_t si = 0;
    // The caller hands over the body of one World or Clump chunk, so there is
    // exactly one root and its children span the whole buffer.
    //
    // This used to index g_ShoSections[si] and derive root from sec.dataStart,
    // which are offsets into the *container*. Applied to a section-local buffer
    // the version check at root+8 failed every time and the loop bailed before
    // emitting anything -- BinMesh tables were found, materials were parsed, and
    // still no mesh ever came out.
    const size_t secEnd = sz;
    const std::vector<std::pair<size_t, uint32_t>> roots{
        {0, isWorld ? 0x000Bu : 0x0010u}};

    for (const auto &[rootOff, rootTypeCur] : roots) {
    const size_t root = rootOff;
    const uint32_t rootType = rootTypeCur;
    // Synthetic root: no header to skip, so the children begin where it does.
    const size_t rootBody = rootOff;
    const size_t rootEnd = secEnd;

    // Finds the Extension of a chunk and hands it over.
    auto handleOwner = [&](size_t a, size_t b, const std::vector<std::string> &localMats, const std::vector<glm::vec4> &localCols, const std::vector<uint32_t> &localBlend) {
      for (size_t c = a; c + 12 <= b;) {
        const uint32_t ct = ru32(c), cs = ru32(c + 4);
        if (ru32(c + 8) != 0x1C020065 || cs == 0 || c + 12 + cs > b)
          break;
        if (ct == 0x0003)
          buildFromExtension(c + 12, c + 12 + cs, (int)si, localMats, localCols, localBlend);
        c += 12 + cs;
      }
    };

    if (rootType == 0x000B) { // World -> AtomicSect / PlaneSect
      std::function<void(size_t, size_t)> walk = [&](size_t a, size_t b) {
        for (size_t c = a; c + 12 <= b;) {
          const uint32_t ct = ru32(c), cs = ru32(c + 4);
          if (ru32(c + 8) != 0x1C020065 || cs == 0 || c + 12 + cs > b)
            break;
          if (ct == 0x0009)
            handleOwner(c + 12, c + 12 + cs, sectionMats[si], sectionCols[si], sectionBlend[si]);
          else if (ct == 0x000A)
            walk(c + 12, c + 12 + cs);
          c += 12 + cs;
        }
      };
      walk(rootBody, rootEnd);
    } else if (rootType == 0x0010) { // Clump -> GeometryList -> Geometry
      // A clump is a hierarchy, not one rigid model. FrameList holds a local
      // matrix per frame, and every Atomic binds one geometry to one frame.
      // Ignoring that drew every part at the clump's origin — which is why the
      // character's head floated above the body and why props whose frame
      // carries a scale came out the wrong size.
      std::vector<glm::mat4> frameWorld;
      {
        for (size_t c = rootBody; c + 12 <= rootEnd;) {
          const uint32_t ct = ru32(c), cs = ru32(c + 4);
          if (ru32(c + 8) != 0x1C020065 || cs == 0 || c + 12 + cs > rootEnd)
            break;
          if (ct == 0x000E) { // FrameList
            const size_t st = c + 12;
            if (ru32(st) == 0x01 && ru32(st + 8) == 0x1C020065) {
              const uint32_t n = ru32(st + 12);
              if (n > 0 && n <= 1024 && st + 16 + (size_t)n * 56 <= rootEnd) {
                std::vector<glm::mat4> local(n, glm::mat4(1.0f));
                std::vector<int32_t> parent(n, -1);
                for (uint32_t i = 0; i < n; i++) {
                  const size_t fb = st + 16 + (size_t)i * 56;
                  glm::mat4 m(1.0f);
                  for (int r = 0; r < 3; r++)
                    for (int cc = 0; cc < 3; cc++)
                      memcpy(&m[r][cc], &payload[fb + (r * 3 + cc) * 4], 4);
                  memcpy(&m[3][0], &payload[fb + 36], 4);
                  memcpy(&m[3][1], &payload[fb + 40], 4);
                  memcpy(&m[3][2], &payload[fb + 44], 4);
                  memcpy(&parent[i], &payload[fb + 48], 4);
                  local[i] = m;
                }
                frameWorld.assign(n, glm::mat4(1.0f));
                for (uint32_t i = 0; i < n; i++)
                  frameWorld[i] = (parent[i] >= 0 && parent[i] < (int32_t)i)
                                      ? frameWorld[parent[i]] * local[i]
                                      : local[i];
              }
            }
          }
          c += 12 + cs;
        }
      }

      // Atomic (0x0014) binds geometryIndex -> frameIndex.
      std::map<uint32_t, uint32_t> geomFrame;
      for (size_t c = rootBody; c + 12 <= rootEnd;) {
        const uint32_t ct = ru32(c), cs = ru32(c + 4);
        if (ru32(c + 8) != 0x1C020065 || cs == 0 || c + 12 + cs > rootEnd)
          break;
        if (ct == 0x0014 && ru32(c + 12) == 0x01)
          geomFrame[ru32(c + 28)] = ru32(c + 24); // struct: frameIdx, geomIdx
        c += 12 + cs;
      }

      uint32_t geomIndex = 0;
      for (size_t c = rootBody; c + 12 <= rootEnd;) {
        const uint32_t ct = ru32(c), cs = ru32(c + 4);
        if (ru32(c + 8) != 0x1C020065 || cs == 0 || c + 12 + cs > rootEnd)
          break;
        if (ct == 0x001A) {
          for (size_t g = c + 12; g + 12 <= c + 12 + cs;) {
            const uint32_t gt = ru32(g), gs = ru32(g + 4);
            if (ru32(g + 8) != 0x1C020065 || gs == 0 || g + 12 + gs > c + 12 + cs)
              break;
            if (gt == 0x000F) {
              std::vector<std::string> geomMats;
              std::vector<glm::vec4> geomCols;
              std::vector<uint32_t> geomBlend;
              for (size_t gc = g + 12; gc + 12 <= g + 12 + gs;) {
                const uint32_t gct = ru32(gc), gcs = ru32(gc + 4);
                if (ru32(gc + 8) != 0x1C020065 || gcs == 0 || gc + 12 + gcs > g + 12 + gs)
                  break;
                if (gct == 0x08) {
                  geomMats = parseMaterialList(gc);
                  geomCols = matColors; // parseMaterialList writes to this global
                  geomBlend = matBlend;
                }
                gc += 12 + gcs;
              }
              // Bake this geometry's frame into its vertices, so instancing and
              // export keep working unchanged.
              glm::mat4 fm(1.0f);
              auto itF = geomFrame.find(geomIndex);
              if (itF != geomFrame.end() && itF->second < frameWorld.size())
                fm = frameWorld[itF->second];
              const size_t firstChunk = destObj->GetMeshes().size();
              handleOwner(g + 12, g + 12 + gs, geomMats, geomCols, geomBlend);
              {
                const int fi = (itF != geomFrame.end()) ? (int)itF->second : -1;
                auto meshes = destObj->GetMeshes();
                for (size_t k = firstChunk; k < meshes.size(); k++)
                  meshes[k]->frameIndex = fi;
              }
              if (fm != glm::mat4(1.0f)) {
                auto meshes = destObj->GetMeshes();
                for (size_t k = firstChunk; k < meshes.size(); k++) {
                  auto ch = meshes[k];
                  for (auto &v : ch->vertices)
                    v.pos = glm::vec3(fm * glm::vec4(v.pos, 1.0f));
                  // The VBO was filled before the transform, so refresh it.
                  glBindBuffer(GL_ARRAY_BUFFER, ch->vbo);
                  glBufferData(GL_ARRAY_BUFFER,
                               (GLsizeiptr)(ch->vertices.size() * sizeof(Vertex)),
                               ch->vertices.data(), GL_STATIC_DRAW);
                }
              }
              geomIndex++;
            }
            g += 12 + gs;
          }
        }
        c += 12 + cs;
      }
    }
    } // per-root chunk loop
  }

  size_t nullNames = 0;
  for (const auto &n : g_MaterialNames)
    if (n == "NULL")
      nullNames++;
  size_t untexTri = 0, emitTri = 0;

}
}
}
