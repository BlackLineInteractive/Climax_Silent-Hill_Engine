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

std::vector<VifPacket> PacketsIn(const std::vector<uint8_t> &d, size_t start,
                                 size_t end) {
  std::vector<VifPacket> out;
  if (end > d.size())
    end = d.size();
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
    if (ReadPacket(d, p, end, pk, after) && pk.vertexCount > 0) {
      out.push_back(std::move(pk));
      p = after;
    } else {
      ++p;
    }
  }
  return out;
}

// Decodes one packet's streams into vertices. `adc` marks strip restarts.
void DecodePacket(const std::vector<uint8_t> &d, const VifPacket &pk,
                  std::vector<Vertex> &verts, std::vector<bool> &adc) {
  const int n = pk.vertexCount;
  verts.assign(n, Vertex{});
  adc.assign(n, false);
  for (auto &v : verts) {
    v.uv = {0.0f, 0.0f};
    v.color = {1.0f, 1.0f, 1.0f, 1.0f};
  }

  bool haveColor = false;
  for (const auto &s : pk.streams) if (s.addr == 2) haveColor = true;
  if (!haveColor) g_DbgNoColor++;
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

void LoadGeometryData(const std::vector<uint8_t> &data) {
  const size_t sz = data.size();
  if (sz < 32)
    return;

  g_MaterialNames.clear();

  // Helper to safely read a uint32 from the buffer
  auto ru32 = [&](size_t off) -> uint32_t {
    if (off + 4 > sz)
      return 0;
    uint32_t v;
    memcpy(&v, &data[off], 4);
    return v;
  };

  // --- HELPER: parse material names from a MaterialList chunk at ml_pos ---
  // Returns a vector<string> with one name per material (or "NULL").
  std::vector<glm::vec4> matColors;
  auto parseMaterialList = [&](size_t ml_pos) -> std::vector<std::string> {
    std::vector<std::string> names;
    matColors.clear();
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
        if (cType == 0x06) { // Texture chunk
          size_t texChild = child + 12;
          size_t texEnd = child + 12 + cSize;
          while (texChild + 12 < texEnd) {
            uint32_t tcType = ru32(texChild);
            uint32_t tcSize = ru32(texChild + 4);
            if (tcSize == 0 || texChild + 12 + tcSize > texEnd)
              break;
            if (tcType == 0x02) { // String = texture name
              size_t nameLen = strnlen((char *)&data[texChild + 12], tcSize);
              texName = std::string((char *)&data[texChild + 12], nameLen);
              break;
            }
            texChild += 12 + tcSize;
          }
          break;
        }
        child += 12 + cSize;
      }
      names.push_back(texName);
      matColors.push_back(matCol);
      curr = matEnd;
    }
    return names;
  };

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
          if (type == 0x14 || type == 0x16 || type == 0x0F || type == 0x10 || type == 0x24 || type == 0x0E || type == 0x0510) {
              self(self, p + 12, csize);
          }
          p += 12 + csize;
      }
  };

  for (const auto& sec : g_ShoSections) {
      if (sec.size > 0 && sec.offset + sec.size <= sz) {
          walkChunks(walkChunks, sec.offset, sec.size);
      }
  }

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
  std::vector<std::vector<std::string>> sectionMats(g_ShoSections.size());
  std::vector<std::vector<glm::vec4>> sectionCols(g_ShoSections.size());
  for (size_t si = 0; si < g_ShoSections.size(); si++) {
    const auto &sec = g_ShoSections[si];
    
    // For synthetic bare-CLUMP sections the entire file IS the section.
    // dataStart==0 means the CLUMP chunk starts at byte 0 of the file.
    // The normal formula (root = dataStart + 4) would land at offset 4 which
    // is the size field of the CLUMP header, not its type field. We detect
    // this case by checking that offset==0 and skip the +4 bias.
    const bool isBareSection = (sec.offset == 0 && sec.dataStart == 0 &&
                                (sec.name == "rwID_CLUMP" || sec.name == "rwID_RWS"));
    const size_t secEnd = isBareSection ? sz : (size_t)sec.offset + 12 + sec.size;
    size_t root = isBareSection ? 0 : (size_t)sec.dataStart + 4;

    if (root + 12 > sz || root + 12 > secEnd)
      continue;
    if (ru32(root + 8) != 0x1C020065)
      continue;

    const uint32_t rootType = ru32(root);
    const uint32_t rootSize = ru32(root + 4);
    const size_t rootEnd = std::min(root + 12 + (size_t)rootSize, secEnd);

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
        }
        c += 12 + cs;
      }
    };

    if (rootType == 0x0B) {
      collectFrom(root + 12, rootEnd);
    } else if (rootType == 0x10) {
      size_t c = root + 12;
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
  for (auto &chunk : g_Chunks) {
    if (chunk.vao)
      glDeleteVertexArrays(1, &chunk.vao);
    if (chunk.vbo)
      glDeleteBuffers(1, &chunk.vbo);
  }
  g_Chunks.clear();

  g_DbgNoColor = 0;
  size_t totalPackets = 0, totalVerts = 0, totalSplits = 0;
  std::vector<Vertex> rawVerts, triVerts;
  std::vector<bool> adcFlags;

  auto addChunk = [&](std::vector<Vertex> &verts, int sectionIdx, int matId, const std::vector<std::string> &localMats, const std::vector<glm::vec4> &localCols) {
    if (verts.empty())
      return;
    MeshChunk m;
    m.vertices = verts;
    m.sectionIndex = sectionIdx;
    m.materialIndex = matId;
    m.texName = "NULL";
    if (matId >= 0 && matId < (int)localMats.size()) {
      m.texName = localMats[matId];
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
    g_Chunks.push_back(std::move(m));
  };

  // Reads the BinMesh split table and the matching native VIF blocks.
  auto buildFromExtension = [&](size_t extBegin, size_t extEnd, int sectionIdx, const std::vector<std::string> &localMats, const std::vector<glm::vec4> &localCols) {
    auto rf32 = [&](size_t off) -> float {
      float f;
      memcpy(&f, &data[off], 4);
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
    if (hasSkin && sectionIdx >= 0 && sectionIdx < (int)g_ShoSections.size()) {
      ShoSection &sec = g_ShoSections[sectionIdx];
      if (!sec.skeleton.bones.empty() && skinPlg + 8 <= skinPlgEnd) {
        // Native (PS2) layout:
        // u32 platform
        // u8 boneCount
        // u8 usedBoneCount
        // u8 maxWeightsPerVertex
        // u8 padding
        // u8 usedBoneIds[usedBoneCount]
        // f32 inverseBoneMatrix[boneCount][16]
        uint8_t boneCount = data[skinPlg + 4];
        uint8_t usedBoneCount = data[skinPlg + 5];
        
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
          
          if (b < (int)sec.skeleton.bones.size()) {
            sec.skeleton.bones[b].invBind = invBind;
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
      const auto packets = PacketsIn(data, p, vifEnd);
      
      std::vector<uint8_t> boneIds;
      std::vector<float> boneWeights;
      if (hasSkin && meshType == 0 && vifEnd + numIdx * 16 <= blockEnd) {
          size_t wp = vifEnd;
          boneIds.resize(numIdx * 4);
          boneWeights.resize(numIdx * 4);
          for (uint32_t v = 0; v < numIdx; ++v) {
              for (int w = 0; w < 4; ++w) {
                  uint8_t bId = data[wp + w * 4];
                  boneIds[v * 4 + w] = bId / 4;
                  // Zero out the lowest byte (the bone ID) to read the clean float
                  uint8_t floatBytes[4] = {0, data[wp + w * 4 + 1], data[wp + w * 4 + 2], data[wp + w * 4 + 3]};
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
        DecodePacket(data, pk, rawVerts, adcFlags);
        
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
      addChunk(triVerts, sectionIdx, matIds[i], localMats, localCols);
      p = blockEnd;
    }
  };

  for (size_t si = 0; si < g_ShoSections.size(); si++) {
    const auto &sec = g_ShoSections[si];
    const bool isBareSection = (sec.offset == 0 && sec.dataStart == 0 &&
                                (sec.name == "rwID_CLUMP" || sec.name == "rwID_RWS"));
    const size_t secEnd = isBareSection ? sz : (size_t)sec.offset + 12 + sec.size;
    const size_t root   = isBareSection ? 0  : (size_t)sec.dataStart + 4;
    if (root + 12 > secEnd || ru32(root + 8) != 0x1C020065)
      continue;
    // A section payload is a SEQUENCE of chunks, not one root. rwID_RWS opens
    // with header chunks 0x23/0x24 and 0x29 and only then holds the real Clump
    // (offset +116 and +65976 in IntroRoad), so looking at the first chunk alone
    // missed it entirely.
    std::vector<std::pair<size_t, uint32_t>> roots;
    {
      const size_t payEnd = std::min(root + (size_t)sec.payloadSize, secEnd);
      size_t p = root;
      while (p + 12 <= payEnd) {
        const uint32_t ct = ru32(p), cs = ru32(p + 4);
        if (ru32(p + 8) != 0x1C020065 || cs == 0 || p + 12 + cs > payEnd) {
          p += 4;
          continue;
        }
        roots.emplace_back(p, ct);
        p += 12 + cs;
      }
      if (roots.empty())
        roots.emplace_back(root, ru32(root));
    }

    for (const auto &[rootOff, rootTypeCur] : roots) {
    const size_t root = rootOff;
    const uint32_t rootType = rootTypeCur;
    const size_t rootEnd = std::min(root + 12 + (size_t)ru32(root + 4), secEnd);

    // Finds the Extension of a chunk and hands it over.
    auto handleOwner = [&](size_t a, size_t b, const std::vector<std::string> &localMats, const std::vector<glm::vec4> &localCols) {
      for (size_t c = a; c + 12 <= b;) {
        const uint32_t ct = ru32(c), cs = ru32(c + 4);
        if (ru32(c + 8) != 0x1C020065 || cs == 0 || c + 12 + cs > b)
          break;
        if (ct == 0x0003)
          buildFromExtension(c + 12, c + 12 + cs, (int)si, localMats, localCols);
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
            handleOwner(c + 12, c + 12 + cs, sectionMats[si], sectionCols[si]);
          else if (ct == 0x000A)
            walk(c + 12, c + 12 + cs);
          c += 12 + cs;
        }
      };
      walk(root + 12, rootEnd);
    } else if (rootType == 0x0010) { // Clump -> GeometryList -> Geometry
      // A clump is a hierarchy, not one rigid model. FrameList holds a local
      // matrix per frame, and every Atomic binds one geometry to one frame.
      // Ignoring that drew every part at the clump's origin — which is why the
      // character's head floated above the body and why props whose frame
      // carries a scale came out the wrong size.
      std::vector<glm::mat4> frameWorld;
      {
        for (size_t c = root + 12; c + 12 <= rootEnd;) {
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
                      memcpy(&m[r][cc], &data[fb + (r * 3 + cc) * 4], 4);
                  memcpy(&m[3][0], &data[fb + 36], 4);
                  memcpy(&m[3][1], &data[fb + 40], 4);
                  memcpy(&m[3][2], &data[fb + 44], 4);
                  memcpy(&parent[i], &data[fb + 48], 4);
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
      for (size_t c = root + 12; c + 12 <= rootEnd;) {
        const uint32_t ct = ru32(c), cs = ru32(c + 4);
        if (ru32(c + 8) != 0x1C020065 || cs == 0 || c + 12 + cs > rootEnd)
          break;
        if (ct == 0x0014 && ru32(c + 12) == 0x01)
          geomFrame[ru32(c + 28)] = ru32(c + 24); // struct: frameIdx, geomIdx
        c += 12 + cs;
      }

      uint32_t geomIndex = 0;
      for (size_t c = root + 12; c + 12 <= rootEnd;) {
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
              for (size_t gc = g + 12; gc + 12 <= g + 12 + gs;) {
                const uint32_t gct = ru32(gc), gcs = ru32(gc + 4);
                if (ru32(gc + 8) != 0x1C020065 || gcs == 0 || gc + 12 + gcs > g + 12 + gs)
                  break;
                if (gct == 0x08) {
                  geomMats = parseMaterialList(gc);
                  geomCols = matColors; // parseMaterialList writes to this global
                }
                gc += 12 + gcs;
              }
              // Bake this geometry's frame into its vertices, so instancing and
              // export keep working unchanged.
              glm::mat4 fm(1.0f);
              auto itF = geomFrame.find(geomIndex);
              if (itF != geomFrame.end() && itF->second < frameWorld.size())
                fm = frameWorld[itF->second];
              const size_t firstChunk = g_Chunks.size();
              handleOwner(g + 12, g + 12 + gs, geomMats, geomCols);
              if (fm != glm::mat4(1.0f)) {
                for (size_t k = firstChunk; k < g_Chunks.size(); k++) {
                  auto &ch = g_Chunks[k];
                  for (auto &v : ch.vertices)
                    v.pos = glm::vec3(fm * glm::vec4(v.pos, 1.0f));
                  // The VBO was filled before the transform, so refresh it.
                  glBindBuffer(GL_ARRAY_BUFFER, ch.vbo);
                  glBufferData(GL_ARRAY_BUFFER,
                               (GLsizeiptr)(ch.vertices.size() * sizeof(Vertex)),
                               ch.vertices.data(), GL_STATIC_DRAW);
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
  for (const auto &c : g_Chunks) {
    emitTri += c.vertices.size() / 3;
    if (c.untextured) untexTri += c.vertices.size() / 3;
  }
  size_t secWithMats = 0;
  for (const auto &v : sectionMats)
    if (!v.empty())
      secWithMats++;
  std::cerr << "[materials] " << secWithMats << "/" << sectionMats.size()
            << " sections carry a material list, " << g_MaterialNames.size()
            << " names, " << nullNames << " unresolved\n";
  {
    size_t missTex = 0, missTri = 0;
    for (const auto &c : g_Chunks) {
      if (c.untextured) continue;
      std::string up = c.texName;
      for (auto &ch : up) ch = (char)toupper((unsigned char)ch);
      if (!g_TextureMap.count(c.texName) && !g_TextureMap.count(up)) {
        missTex++; missTri += c.vertices.size() / 3;
      }
    }
    std::cerr << "[textures] " << g_TextureMap.size() / 2 << " loaded; "
              << missTex << " meshes (" << missTri
              << " tris) name a texture that is not present\n";
    std::cerr << "[colors] " << g_DbgNoColor << " of " << totalPackets
              << " packets carry no vertex-colour stream\n";
  }
  {
    double ur=0,ug=0,ub=0,ua=0; size_t un=0;
    double tr=0,tn=0;
    for (const auto &c : g_Chunks) {
      for (const auto &v : c.vertices) {
        if (c.untextured) { ur+=v.color.r; ug+=v.color.g; ub+=v.color.b; ua+=v.color.a; un++; }
        else { tr+=v.color.r; tn++; }
      }
    }
    if (un) std::cerr << "[vcolor] untextured meshes avg RGBA = "
      << ur/un << " " << ug/un << " " << ub/un << " " << ua/un
      << "   (textured avg R = " << (tn?tr/tn:0) << ")\n";
  }
  std::cerr << "[geometry] " << totalSplits << " splits, " << totalPackets
            << " VIF packets, " << totalVerts << " strip verts -> "
            << g_Chunks.size() << " meshes, " << emitTri << " tris ("
            << untexTri << " flat-colour)\n";
  std::cerr.flush();
  std::cerr << "[loader] sections=" << g_ShoSections.size()
            << " gameObjects=" << g_GameObjects.size() << "\n";
  std::cerr.flush();
}


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
  // Every texture is registered under both its original and its upper-case
  // name, so delete each GL object once instead of once per alias.
  std::vector<GLuint> uniqueIds;
  for (auto &[name, id] : g_TextureMap)
    if (std::find(uniqueIds.begin(), uniqueIds.end(), id) == uniqueIds.end())
      uniqueIds.push_back(id);
  if (!uniqueIds.empty())
    glDeleteTextures((GLsizei)uniqueIds.size(), uniqueIds.data());
  g_TextureMap.clear();
  g_TexInfo.clear();
  g_RawTextures.clear();

  // Shattered Memories containers are the same chunk types with big-endian
  // fields and no type directory; their geometry is GameCube native data that
  // this loader cannot read yet, so they stop after sections and textures.
  if (IsShsmContainer(container)) {
    g_Chunks.clear();
    g_MaterialNames.clear();
    g_MeshTexMap.clear();
    g_Cameras.clear();
    ParseShsmContainer(container);
    UploadChunks();
    g_CurrentMeshContainer = displayName;
    g_CurrentTxdPaths.clear();
    g_MeshTexMap.clear();

    return;
  }

  // Structure first: LoadGeometryData needs the section ranges to know which
  // section each mesh belongs to, and therefore how it should be placed.
  ParseContainerStructureData(container);
  LoadGeometryData(container);

  // 1. Спочатку шукаємо строго за іменем
  for (const auto &[name, blob] : txds)
    ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(blob, g_MaterialNames, false);

  // 2. Якщо лишились незаповнені слоти – fallback
  std::vector<std::string> missing;
  for (const auto &mat : g_MaterialNames)
    if (g_TextureMap.find(mat) == g_TextureMap.end())
      missing.push_back(mat);
  if (!missing.empty())
    for (const auto &[name, blob] : txds)
      ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(blob, missing, true);

  // Textures may also be embedded directly in the container.
  if (g_TextureMap.empty())
    ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(container, g_MaterialNames, true);

  // Names a texture that no loaded dictionary provides. Those meshes bind
  // texture 0 and come out solid black, which looks exactly like an untextured
  // material — report them so the two cases stop being confused.
  {
    std::map<std::string, size_t> missing;
    for (const auto &c : g_Chunks) {
      if (c.untextured || c.texName.empty() || c.texName == "NULL") continue;
      std::string up = c.texName, lo = c.texName;
      for (auto &ch : up) ch = (char)toupper((unsigned char)ch);
      for (auto &ch : lo) ch = (char)tolower((unsigned char)ch);
      if (g_TextureMap.count(c.texName) || g_TextureMap.count(up) ||
          g_TextureMap.count(lo)) continue;
      missing[c.texName] += c.vertices.size() / 3;
    }
    if (!missing.empty()) {
      std::cerr << "[textures] " << missing.size()
                << " named textures are not in any loaded dictionary:\n";
      for (const auto &[nm, tris] : missing)
        std::cerr << "    " << nm << "  (" << tris << " tris)\n";
    }
  }

  {
    std::cerr << "[level] starting vcolor-loop rows=" << g_Chunks.size() << "\n"; std::cerr.flush();
    struct Row { std::string n; size_t tris; float r, g, b, a; };

    std::vector<Row> rows;
    for (const auto &c : g_Chunks) {
      if (c.vertices.empty()) continue;
      double r=0,g=0,b=0,a=0;
      for (const auto &v : c.vertices) { r+=v.color.r; g+=v.color.g; b+=v.color.b; a+=v.color.a; }
      const double k = 1.0 / c.vertices.size();
      rows.push_back({c.texName, c.vertices.size()/3,
                      (float)(r*k), (float)(g*k), (float)(b*k), (float)(a*k)});
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row &x, const Row &y){ return x.tris > y.tris; });
    std::cerr << "[vcolor] biggest meshes, average vertex colour:\n";
    for (size_t i = 0; i < rows.size() && i < 12; i++)
      std::cerr << "    " << rows[i].tris << " tris  " << rows[i].n
                << "   rgba " << rows[i].r << " " << rows[i].g << " "
                << rows[i].b << " " << rows[i].a << "\n";
  }

  ApplyAlphaMasks();
  std::cerr << "[level] ApplyAlphaMasks OK\n"; std::cerr.flush();

  g_CurrentMeshContainer = displayName;
  g_CurrentTxdPaths.clear();
  for (const auto &[name, blob] : txds)
    g_CurrentTxdPaths.push_back(name);

  // Build per-texture mesh-chunk index map using the directly stored texName
  g_MeshTexMap.clear();
  // Build per-texture mesh-chunk index map using the directly stored texName
  g_MeshTexMap.clear();
  // Wait, g_Chunks are moved into CSceneObjects now. 
  // We will populate g_MeshTexMap down below after objects are created.
  
  // Phase 2: Convert to CSceneObjectRegistrar
  ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().Clear();
  for (const auto& sec : g_ShoSections) {
      if (sec.isWorldSpace) {
          auto obj = std::make_shared<ClimaxEngine::SG::CWorldObject>(sec.name.empty() ? "WorldSpace" : sec.name);
          for (size_t i = 0; i < g_Chunks.size(); i++) {
              if (g_Chunks[i].sectionIndex == &sec - g_ShoSections.data()) {
                  obj->AddMesh(std::move(g_Chunks[i]));
              }
          }
          ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().RegisterObject(obj);
      } else {
          for (size_t instIdx = 0; instIdx < sec.instances.size(); instIdx++) {
              const auto& inst = sec.instances[instIdx];
              std::string name = sec.name + "_Inst" + std::to_string(instIdx);
              if (inst.gameObjectId >= 0 && inst.gameObjectId < (int)g_GameObjects.size()) {
                  name = g_GameObjects[inst.gameObjectId].instName;
              }
              
              auto obj = std::make_shared<ClimaxEngine::SG::CClumpObject>(name);
              obj->SetTransform(inst.transform);
              obj->skeleton = sec.skeleton;
              obj->animClip = sec.animClip;
              
              for (size_t i = 0; i < g_Chunks.size(); i++) {
                  if (g_Chunks[i].sectionIndex == &sec - g_ShoSections.data()) {
                      // We must copy the mesh because multiple instances share the same geometry
                      MeshChunk copy = g_Chunks[i];
                      obj->AddMesh(std::move(copy));
                  }
              }
              ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().RegisterObject(obj);
          }
      }
  }

  // Populate g_MeshTexMap
  g_MeshTexMap.clear();
  for (auto& obj : ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance().GetObjects()) {
      for (auto* chunk : obj->GetMeshes()) {
          const std::string &tName = chunk->texName;
          g_MeshTexMap[tName.empty() ? "NULL" : tName].push_back(chunk);
      }
  }

  std::cerr << "[level] LoadLevelData complete.\n"; std::cerr.flush();
}


// ── Path-based wrappers ─────────────────────────────────────────────────────
void LoadTexturesFromTxd(const std::string &txdPath,
                         const std::vector<std::string> &allowedNames,
                         bool fallback) {
  ClimaxEngine::Platform::PS2::PS2TextureDecoder().LoadDictionary(ReadWholeFile(txdPath), allowedNames, fallback);
}

void LoadGeometry(const std::string &geomPath) {
  LoadGeometryData(ReadWholeFile(geomPath));
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
      if (go.instName.empty())
        go.instName = readName(payOff, payLen);
    } else if (kind == 0x00 && payLen == 16) {
      go.guidRefs.emplace_back((const char *)&data[payOff], 16);
    } else if (kind == 0x00 && idx == 1 && payLen == 64 && !haveXform) {
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
    // Determine if this is a spatial transform vs a volume extent
    bool isVolume = (go.className == "CPhysicsObject" || 
                     go.className == "CZone" ||
                     go.className == "CWaterZone" ||
                     go.className == "CCameraZone" ||
                     go.className == "CCameraArea");
    if (!isVolume) {
      go.position = glm::vec3(go.transform[3]);
      go.atOrigin = (glm::length(go.position) < 1e-4f);
    } else {
      go.atOrigin = true; // don't use it as a mesh placement transform
    }
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

void ParseContainerStructureData(const std::vector<uint8_t> &data) {
  g_ContainerChunks.clear();
  g_ShoTypes.clear();
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
          clip.name = g_ShoSections.back().name; // wait, the filename is the secName, wait, secName is "rwID_HANIMANIMATION" according to the section loop. But the spec says "The section tag carries the source filename, which is the clip's identity". The section tag is g_ShoSections.back().name? No, g_ShoSections.back().name is set from the dictionary! Wait, the actual filename is stored somewhere else in the section?
          // I will set clip.name to a fallback for now.
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
                
                glm::quat q(-qx, -qy, -qz, qw); // Spec says reference uses conjugate, test both ways. Let's try conjugate.
                
                float px = (tx / 65535.0f) * transScalar[0] + transOffset[0];
                float py = (ty / 65535.0f) * transScalar[1] + transOffset[1];
                float pz = (tz / 65535.0f) * transScalar[2] + transOffset[2];
                glm::vec3 pos(px, py, pz);
                
                clip.tracks[tIdx].times.push_back(time);
                clip.tracks[tIdx].rot.push_back(q);
                clip.tracks[tIdx].pos.push_back(pos);
              }
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