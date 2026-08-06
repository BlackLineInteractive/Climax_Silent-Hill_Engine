#include "ClimaxEngine/Platform/Wii/WiiGeometry.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace {

inline uint32_t rle32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
inline uint32_t rbe32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
inline uint16_t rbe16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
inline float rbef(const uint8_t* p) {
    const uint32_t v = rbe32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// GX vertex attribute ids, as they appear in the native header.
enum { GX_VA_POS = 9, GX_VA_NRM = 10, GX_VA_CLR0 = 11, GX_VA_TEX0 = 13,
       GX_VA_TEX1 = 14 };

// GX attribute types. Only the two indexed ones occur, and they are what
// decides how wide the index is in the display list.
enum { GX_INDEX8 = 2, GX_INDEX16 = 3 };

struct Attr {
    uint32_t offset = 0;
    uint8_t  id = 0;
    uint8_t  stride = 0;
    uint8_t  type = 0;
    uint16_t count = 0;
};

struct Chunk {
    uint32_t type = 0, size = 0, version = 0;
    size_t   payload = 0;
};

// RenderWare chunk headers stay little-endian even on the Wii; only the
// platform-native blocks inside them switch to big-endian.
bool ReadChunk(const uint8_t* d, size_t size, size_t off, Chunk& c) {
    if (off + 12 > size) return false;
    c.type = rle32(d + off);
    c.size = rle32(d + off + 4);
    c.version = rle32(d + off + 8);
    if (off + 12 + (size_t)c.size > size) return false;
    c.payload = off + 12;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Material list
//
// Standard RenderWare: a Struct giving the count and a per-material index
// table, then one Material chunk each. The texture name is the first String
// inside the material's Texture chunk.
// ---------------------------------------------------------------------------
void WiiGeom::ReadMaterialList(const uint8_t* d, size_t size, size_t off,
                               std::vector<WiiMaterial>& out) {
    Chunk list;
    if (!ReadChunk(d, size, off, list) || list.type != 0x0008) return;

    Chunk st;
    if (!ReadChunk(d, size, list.payload, st) || st.type != 0x0001) return;
    size_t o = list.payload + 12 + st.size;
    const size_t end = list.payload + list.size;

    while (o + 12 <= end) {
        Chunk mat;
        if (!ReadChunk(d, size, o, mat) || mat.type != 0x0007) break;

        WiiMaterial m;
        Chunk ms;
        size_t inner = mat.payload;
        if (ReadChunk(d, size, inner, ms) && ms.type == 0x0001 && ms.size >= 28) {
            const uint8_t* p = d + ms.payload;
            m.color = glm::vec4(p[4] / 255.0f, p[5] / 255.0f, p[6] / 255.0f,
                                p[7] / 255.0f);
            m.textured = rle32(p + 12) != 0;
            inner += 12 + ms.size;
        }

        // Reads the first String inside a Texture chunk.
        auto textureName = [&](const Chunk& tex) {
            std::string name;
            size_t t = tex.payload;
            while (t + 12 <= tex.payload + tex.size) {
                Chunk s2;
                if (!ReadChunk(d, size, t, s2)) break;
                if (s2.type == 0x0002) {
                    const char* q = (const char*)d + s2.payload;
                    name.assign(q, strnlen(q, s2.size));
                    break;
                }
                t += 12 + s2.size;
            }
            return name;
        };

        // Walk the material's remaining children for the Texture chunk; its
        // first String child is the texture name. The 0x0129 extension holds
        // the alternate (frozen) texture; its header is 12 + count bytes, which
        // holds on 1747 of 1771 materials.
        while (inner + 12 <= mat.payload + mat.size) {
            Chunk c;
            if (!ReadChunk(d, size, inner, c)) break;
            if (c.type == 0x0003) {
                size_t e = c.payload;
                while (e + 12 <= c.payload + c.size) {
                    Chunk x;
                    if (!ReadChunk(d, size, e, x)) break;
                    if (x.type == 0x0129 && x.size > 16) {
                        const uint8_t count = d[x.payload + 5];
                        size_t q = x.payload + 12 + count;
                        Chunk tex;
                        if (ReadChunk(d, size, q, tex) && tex.type == 0x0006 &&
                            tex.version == 0x1C020065)
                            m.altTexName = textureName(tex);
                    }
                    e += 12 + x.size;
                }
            }
            if (c.type == 0x0006 && m.texName.empty()) m.texName = textureName(c);
            inner += 12 + c.size;
        }
        out.push_back(std::move(m));
        o += 12 + mat.size;
    }
}

// ---------------------------------------------------------------------------
// NativeDataPLG (0x0510), GameCube flavour
//
// The plugin wraps a Struct whose payload is:
//
//   +0x00  u32 LE  platform, 6 = GameCube / Wii
//   +0x04  u32 LE  header block length
//   +0x08  u32 LE  data block length
//   header block, big-endian:
//     +0x00  u32   flags
//     +0x04  u32   0
//     +0x08  u32   attribute count
//     +0x0C  attribute[] of 12 bytes:
//              u32 offset into the data block
//              u8  GX attribute id     9 POS, 10 NRM, 11 CLR0, 13 TEX0, 14 TEX1
//              u8  stride              12, 12, 4, 8, 8
//              u8  GX type             2 = INDEX8, 3 = INDEX16
//              u8  padding
//              u16 element count
//              u16 padding
//     then one 8-byte record per BinMesh mesh: [u32 offset][u32 size] of its
//     display list
//   data block: the display lists first, then the attribute arrays
//
// Verified over 904 world sectors: the display-list sizes sum to exactly the
// offset of the first attribute array, every index falls inside its array, and
// all 599 380 decoded positions land inside their own sector's bounding box.
//
// A display list is GX commands. Only 0x98 occurs -- GX_DRAW_TRIANGLESTRIP with
// vertex-attribute table 0 -- followed by a big-endian u16 vertex count and
// then one index per enabled attribute, each u8 or u16 as its type says. The
// list is zero-padded to a 32-byte boundary.
// ---------------------------------------------------------------------------
bool WiiGeom::ReadNative(const uint8_t* d, size_t size, size_t off,
                         const std::vector<uint32_t>& meshMaterial,
                         const std::vector<WiiMaterial>& materials,
                         int matListWindowBase, int /*sectionIndex*/,
                         std::vector<MeshChunk>& out) {
    Chunk plug;
    if (!ReadChunk(d, size, off, plug) || plug.type != 0x0510) return false;
    Chunk st;
    if (!ReadChunk(d, size, plug.payload, st) || st.type != 0x0001) return false;
    if (st.size < 12) return false;

    const uint8_t* p = d + st.payload;
    if (rle32(p) != 6) return false;                    // not GameCube data
    const uint32_t headerLen = rle32(p + 4);
    const uint32_t dataLen = rle32(p + 8);
    if ((size_t)12 + headerLen + dataLen > st.size) return false;

    const uint8_t* hdr = p + 12;
    const uint8_t* data = hdr + headerLen;
    if (headerLen < 12) return false;

    const uint32_t attrCount = rbe32(hdr + 8);
    if (attrCount == 0 || 12 + attrCount * 12 > headerLen) return false;

    std::vector<Attr> attrs(attrCount);
    size_t vertexWidth = 0;
    for (uint32_t i = 0; i < attrCount; i++) {
        const uint8_t* a = hdr + 12 + i * 12;
        attrs[i].offset = rbe32(a);
        attrs[i].id     = a[4];
        attrs[i].stride = a[5];
        attrs[i].type   = a[6];
        attrs[i].count  = rbe16(a + 8);
        if (attrs[i].type != GX_INDEX8 && attrs[i].type != GX_INDEX16) return false;
        vertexWidth += attrs[i].type == GX_INDEX16 ? 2 : 1;
        if ((size_t)attrs[i].offset + (size_t)attrs[i].count * attrs[i].stride > dataLen)
            return false;
    }

    const size_t dlCount = (headerLen - 12 - attrCount * 12) / 8;
    const uint8_t* dlTable = hdr + 12 + attrCount * 12;

    // Fetches one attribute value by index, or a default when the mesh does
    // not carry that attribute at all.
    auto fetch = [&](const Attr& a, uint32_t idx, float* dst, int n) {
        if (idx >= a.count) { for (int i = 0; i < n; i++) dst[i] = 0.0f; return; }
        const uint8_t* q = data + a.offset + (size_t)idx * a.stride;
        for (int i = 0; i < n; i++) dst[i] = rbef(q + i * 4);
    };

    for (size_t m = 0; m < dlCount; m++) {
        const uint32_t dlOff = rbe32(dlTable + m * 8);
        const uint32_t dlSize = rbe32(dlTable + m * 8 + 4);
        if ((size_t)dlOff + dlSize > dataLen) continue;

        MeshChunk chunk;
        // chunk.sectionIndex = sectionIndex;
        const int matId = m < meshMaterial.size()
                              ? (int)meshMaterial[m] + matListWindowBase : -1;
        if (matId >= 0 && matId < (int)materials.size()) {
            chunk.texName = materials[matId].texName;
            chunk.altTexName = materials[matId].altTexName;
            chunk.matColor = materials[matId].color;
            chunk.untextured = !materials[matId].textured ||
                               materials[matId].texName.empty();
        } else {
            chunk.untextured = true;
        }
        chunk.materialIndex = matId;

        const uint8_t* dl = data + dlOff;
        size_t o = 0;
        while (o + 3 <= dlSize) {
            const uint8_t cmd = dl[o];
            if (cmd == 0) break;                        // trailing padding
            if ((cmd & 0xF8) != 0x98) break;            // only tri-strips occur
            const uint16_t n = rbe16(dl + o + 1);
            if (n == 0 || o + 3 + (size_t)n * vertexWidth > dlSize) break;

            // Read the strip, then emit triangles with the alternating winding
            // a triangle strip implies.
            std::vector<Vertex> strip(n);
            for (uint16_t v = 0; v < n; v++) {
                const uint8_t* vp = dl + o + 3 + (size_t)v * vertexWidth;
                size_t c = 0;
                Vertex& out2 = strip[v];
                out2.color = glm::vec4(1.0f);
                for (const Attr& a : attrs) {
                    uint32_t idx;
                    if (a.type == GX_INDEX16) { idx = rbe16(vp + c); c += 2; }
                    else { idx = vp[c]; c += 1; }

                    switch (a.id) {
                        case GX_VA_POS: {
                            float f[3];
                            fetch(a, idx, f, 3);
                            out2.pos = glm::vec3(f[0], f[1], f[2]);
                            break;
                        }
                        case GX_VA_TEX0: {
                            float f[2];
                            fetch(a, idx, f, 2);
                            out2.uv = glm::vec2(f[0], f[1]);
                            break;
                        }
                        case GX_VA_CLR0: {
                            if (idx < a.count) {
                                const uint8_t* q = data + a.offset + (size_t)idx * a.stride;
                                out2.color = glm::vec4(q[0] / 255.0f, q[1] / 255.0f,
                                                       q[2] / 255.0f, q[3] / 255.0f);
                            }
                            break;
                        }
                        default: break;                 // normals and TEX1 unused
                    }
                }
            }

            for (uint16_t t = 0; t + 2 < n; t++) {
                const Vertex& a0 = strip[t];
                const Vertex& a1 = strip[t + 1];
                const Vertex& a2 = strip[t + 2];
                // A degenerate triangle is a strip stitch, not a surface.
                if (a0.pos == a1.pos || a1.pos == a2.pos || a0.pos == a2.pos)
                    continue;
                if (t & 1) { chunk.vertices.push_back(a0);
                             chunk.vertices.push_back(a2);
                             chunk.vertices.push_back(a1); }
                else       { chunk.vertices.push_back(a0);
                             chunk.vertices.push_back(a1);
                             chunk.vertices.push_back(a2); }
            }
            o += 3 + (size_t)n * vertexWidth;
        }

        if (!chunk.vertices.empty()) {
            double sum = 0.0;
            for (const auto& v : chunk.vertices) sum += v.color.r + v.color.g + v.color.b;
            if (sum / (chunk.vertices.size() * 3.0) < 0.004)
                chunk.unlitGeometry = true;
            out.push_back(std::move(chunk));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// World sector tree
//
//   0x000B World
//     0x0001 Struct
//     0x0008 MaterialList
//     0x000A PlaneSector  (recursive)  /  0x0009 AtomicSector (leaf)
//
// An AtomicSector holds a Struct with its material window base, counts and
// bounding box, then an Extension carrying BinMeshPLG and NativeDataPLG.
// ---------------------------------------------------------------------------
void WiiGeom::ReadWorld(const uint8_t* d, size_t size, size_t off, size_t len,
                        int /*sectionIndex*/, std::vector<MeshChunk>& out,
                        std::vector<std::string>* materialNames) {
    Chunk world;
    if (!ReadChunk(d, size, off, world) || world.type != 0x000B) return;

    Chunk st;
    if (!ReadChunk(d, size, world.payload, st)) return;

    std::vector<WiiMaterial> materials;
    ReadMaterialList(d, size, world.payload + 12 + st.size, materials);
    if (materialNames)
        for (const auto& m : materials) materialNames->push_back(m.texName);

    // Recursive walk; sectors nest a few levels deep in the retail data.
    struct Walker {
        const uint8_t* d; size_t size; int sectionIndex;
        const std::vector<WiiMaterial>* materials;
        std::vector<MeshChunk>* out;

        void Sector(size_t begin, size_t end, int depth) {
            if (depth > 32) return;
            size_t o = begin;
            while (o + 12 <= end) {
                Chunk c;
                if (!ReadChunk(d, size, o, c) || c.size == 0) break;
                if (c.type == 0x000A) Sector(c.payload, c.payload + c.size, depth + 1);
                else if (c.type == 0x0009) Atomic(c.payload, c.payload + c.size);
                o += 12 + c.size;
            }
        }

        void Atomic(size_t begin, size_t end) {
            size_t o = begin;
            int matBase = 0;
            size_t extBegin = 0, extEnd = 0;
            while (o + 12 <= end) {
                Chunk c;
                if (!ReadChunk(d, size, o, c)) break;
                if (c.type == 0x0001 && c.size >= 12) matBase = (int)rle32(d + c.payload);
                else if (c.type == 0x0003) { extBegin = c.payload; extEnd = c.payload + c.size; }
                o += 12 + c.size;
            }
            if (!extBegin) return;

            std::vector<uint32_t> meshMaterial;
            size_t nativeOff = 0;
            size_t e = extBegin;
            while (e + 12 <= extEnd) {
                Chunk c;
                if (!ReadChunk(d, size, e, c)) break;
                if (c.type == 0x050E && c.size >= 12) {
                    const uint32_t nmesh = rle32(d + c.payload + 4);
                    for (uint32_t i = 0; i < nmesh && 12 + i * 8 + 8 <= c.size; i++)
                        meshMaterial.push_back(rle32(d + c.payload + 12 + i * 8 + 4));
                } else if (c.type == 0x0510) {
                    nativeOff = e;
                }
                e += 12 + c.size;
            }
            if (nativeOff)
                WiiGeom::ReadNative(d, size, nativeOff, meshMaterial, *materials,
                                    matBase, sectionIndex, *out);
        }
    };

    Walker w{d, size, -1, &materials, &out};
    w.Sector(world.payload, world.payload + world.size, 0);
}

// ---------------------------------------------------------------------------
// Plain RenderWare geometry
//
// Not every geometry in a Wii clump is native. A character's head is a normal
// RenderWare geometry -- because it also carries delta-morph targets for facial
// animation -- and its Struct holds everything inline, little-endian:
//
//   u32 flags, u32 numTriangles, u32 numVertices, u32 numMorphTargets
//   if TEXTURED:  numTexCoordSets x numVertices x 2 floats
//   if PRELIT:    numVertices x RGBA8            (before the UVs)
//   triangles:    numTriangles x [u16 v2][u16 v1][u16 material][u16 v3]
//   morph target: 4 floats bounding sphere, u32 hasVertices, u32 hasNormals
//                 then numVertices x 3 floats each, as flagged
//
// Checked against Adult_Cheryl's head: 16 + 7864 + 13784 + 24 + 11796 + 11796
// comes to 45 280 bytes, exactly the declared struct size.
//
// Skipping these is what left every character without a face.
// ---------------------------------------------------------------------------
bool WiiGeom::ReadPlainGeometry(const uint8_t* d, size_t size, size_t structOff,
                                uint32_t structSize,
                                const std::vector<WiiMaterial>& materials,
                                int sectionIndex, std::vector<MeshChunk>& out) {
    if (structSize < 16 || structOff + structSize > size) return false;
    const uint8_t* p = d + structOff;

    auto lef = [](const uint8_t* q) {
        const uint32_t v = rle32(q);
        float f; std::memcpy(&f, &v, 4); return f;
    };

    const uint32_t flags = rle32(p);
    const uint32_t nTri = rle32(p + 4);
    const uint32_t nVert = rle32(p + 8);
    if (flags & 0x01000000) return false;               // native, handled elsewhere
    if (nVert == 0 || nTri == 0 || nVert > 1u << 20 || nTri > 1u << 20) return false;

    const bool prelit = (flags & 0x08) != 0;
    const bool textured = (flags & 0x04) != 0 || (flags & 0x80) != 0;
    const uint32_t texSets = textured ? std::max(1u, (flags >> 16) & 0x0F) : 0;

    size_t o = 16;
    const size_t colours = prelit ? o : 0;
    if (prelit) o += (size_t)nVert * 4;
    const size_t uvs = textured ? o : 0;
    o += (size_t)texSets * nVert * 8;
    const size_t tris = o;
    o += (size_t)nTri * 8;
    const size_t morph = o;
    o += 24;
    if (o + (size_t)nVert * 12 > structSize) return false;
    const size_t positions = o;
    o += (size_t)nVert * 12;
    const bool hasNormals = rle32(p + morph + 20) != 0;
    (void)hasNormals;

    // One chunk per material, so the renderer can bind a texture per draw.
    std::map<uint32_t, MeshChunk> byMaterial;
    for (uint32_t t = 0; t < nTri; t++) {
        const uint8_t* tri = p + tris + (size_t)t * 8;
        const uint16_t v2 = (uint16_t)(tri[0] | (tri[1] << 8));
        const uint16_t v1 = (uint16_t)(tri[2] | (tri[3] << 8));
        const uint16_t mi = (uint16_t)(tri[4] | (tri[5] << 8));
        const uint16_t v3 = (uint16_t)(tri[6] | (tri[7] << 8));
        if (v1 >= nVert || v2 >= nVert || v3 >= nVert) continue;

        MeshChunk& chunk = byMaterial[mi];
        if (chunk.vertices.empty()) {
            // chunk.sectionIndex = sectionIndex;
            chunk.materialIndex = (int)mi;
            if (mi < materials.size()) {
                chunk.texName = materials[mi].texName;
                chunk.altTexName = materials[mi].altTexName;
                chunk.matColor = materials[mi].color;
                chunk.untextured = !materials[mi].textured ||
                                   materials[mi].texName.empty();
            } else {
                chunk.untextured = true;
            }
        }
        const uint16_t idx[3] = {v1, v2, v3};
        for (int k = 0; k < 3; k++) {
            Vertex v;
            const uint8_t* pos = p + positions + (size_t)idx[k] * 12;
            v.pos = glm::vec3(lef(pos), lef(pos + 4), lef(pos + 8));
            if (textured) {
                const uint8_t* uv = p + uvs + (size_t)idx[k] * 8;
                v.uv = glm::vec2(lef(uv), lef(uv + 4));
            }
            if (prelit) {
                const uint8_t* c = p + colours + (size_t)idx[k] * 4;
                v.color = glm::vec4(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f,
                                    c[3] / 255.0f);
            } else {
                v.color = glm::vec4(1.0f);
            }
            chunk.vertices.push_back(v);
        }
    }

    for (auto& kv : byMaterial)
        if (!kv.second.vertices.empty()) out.push_back(std::move(kv.second));
    return true;
}

// ---------------------------------------------------------------------------
// Clumps
//
//   0x0010 Clump
//     0x0001 Struct        atomic / light / camera counts
//     0x000E FrameList     [u32 count][frame[count]] then one Extension each
//                          frame = 3x3 rotation, position, parent index, flags
//     0x001A GeometryList  [u32 count] then 0x000F Geometry chunks
//       0x000F Geometry
//         0x0001 Struct        format and counts; empty for native geometry
//         0x0008 MaterialList
//         0x0003 Extension     BinMeshPLG + NativeDataPLG, as in a world sector
//     0x0014 Atomic        [u32 frameIndex][u32 geometryIndex][u32 flags][u32]
//
// Some geometries are not native at all -- the delta-morph targets carry a
// full 0x0001 struct and no 0x0510 -- and those are skipped here.
// ---------------------------------------------------------------------------
void WiiGeom::ReadClump(const uint8_t* d, size_t size, size_t off, size_t len,
                        int /*sectionIndex*/, std::vector<MeshChunk>& out,
                        std::vector<std::string>* materialNames) {
    Chunk clump;
    if (!ReadChunk(d, size, off, clump) || clump.type != 0x0010) return;

    std::vector<glm::mat4> frames;      // local
    std::vector<int> parents;
    struct Geo { size_t nativeOff = 0; size_t structOff = 0; uint32_t structSize = 0;
                 std::vector<uint32_t> meshMat;
                 std::vector<WiiMaterial> materials; };
    std::vector<Geo> geos;
    struct Atomic { uint32_t frame = 0, geom = 0; };
    std::vector<Atomic> atomics;

    size_t o = clump.payload;
    const size_t end = clump.payload + clump.size;
    while (o + 12 <= end) {
        Chunk c;
        if (!ReadChunk(d, size, o, c)) break;

        if (c.type == 0x000E) {                       // FrameList
            Chunk st;
            if (ReadChunk(d, size, c.payload, st) && st.size >= 4) {
                const uint32_t n = rle32(d + st.payload);
                for (uint32_t i = 0; i < n && 4 + (size_t)i * 56 + 56 <= st.size; i++) {
                    const uint8_t* f = d + st.payload + 4 + (size_t)i * 56;
                    auto lef = [&](int k) {
                        const uint32_t v = rle32(f + k * 4);
                        float x; std::memcpy(&x, &v, 4); return x;
                    };
                    glm::mat4 m(1.0f);
                    for (int col = 0; col < 3; col++)
                        for (int row = 0; row < 3; row++)
                            m[col][row] = lef(col * 3 + row);
                    m[3] = glm::vec4(lef(9), lef(10), lef(11), 1.0f);
                    frames.push_back(m);
                    parents.push_back((int)rle32(f + 48));
                }
            }
        } else if (c.type == 0x001A) {                // GeometryList
            Chunk st;
            size_t g = c.payload;
            if (ReadChunk(d, size, g, st) && st.type == 0x0001) g += 12 + st.size;
            while (g + 12 <= c.payload + c.size) {
                Chunk gc;
                if (!ReadChunk(d, size, g, gc) || gc.type != 0x000F) break;
                Geo geo;
                size_t q = gc.payload;
                while (q + 12 <= gc.payload + gc.size) {
                    Chunk sub;
                    if (!ReadChunk(d, size, q, sub)) break;
                    if (sub.type == 0x0001) {
                        geo.structOff = sub.payload;
                        geo.structSize = sub.size;
                    } else if (sub.type == 0x0008) {
                        ReadMaterialList(d, size, q, geo.materials);
                        if (materialNames)
                            for (const auto& mm : geo.materials)
                                materialNames->push_back(mm.texName);
                    } else if (sub.type == 0x0003) {
                        size_t e = sub.payload;
                        while (e + 12 <= sub.payload + sub.size) {
                            Chunk x;
                            if (!ReadChunk(d, size, e, x)) break;
                            if (x.type == 0x050E && x.size >= 12) {
                                const uint32_t nmesh = rle32(d + x.payload + 4);
                                for (uint32_t i = 0;
                                     i < nmesh && 12 + i * 8 + 8 <= x.size; i++)
                                    geo.meshMat.push_back(
                                        rle32(d + x.payload + 12 + i * 8 + 4));
                            } else if (x.type == 0x0510) {
                                geo.nativeOff = e;
                            }
                            e += 12 + x.size;
                        }
                    }
                    q += 12 + sub.size;
                }
                geos.push_back(std::move(geo));
                g += 12 + gc.size;
            }
        } else if (c.type == 0x0014) {                // Atomic
            Chunk st;
            if (ReadChunk(d, size, c.payload, st) && st.size >= 8)
                atomics.push_back({rle32(d + st.payload), rle32(d + st.payload + 4)});
        }
        o += 12 + c.size;
    }

    // Frame matrices are relative to the parent; compose them once.
    std::vector<glm::mat4> world(frames.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < frames.size(); i++) {
        glm::mat4 m = frames[i];
        int p = parents[i];
        for (int guard = 0; p >= 0 && p < (int)frames.size() && guard < 64; guard++) {
            m = frames[p] * m;
            p = parents[p];
        }
        world[i] = m;
    }

    for (const Atomic& a : atomics) {
        if (a.geom >= geos.size()) continue;
        const Geo& geo = geos[a.geom];
        const size_t before = out.size();
        if (geo.nativeOff) {
            ReadNative(d, size, geo.nativeOff, geo.meshMat, geo.materials, 0,
                       -1, out);
        } else if (geo.structSize > 16) {
            ReadPlainGeometry(d, size, geo.structOff, geo.structSize,
                              geo.materials, -1, out);
        }
        if (out.size() == before) continue;
        if (a.frame < world.size()) {
            const glm::mat4& m = world[a.frame];
            for (size_t i = before; i < out.size(); i++)
                for (auto& v : out[i].vertices)
                    v.pos = glm::vec3(m * glm::vec4(v.pos, 1.0f));
        }
    }
}
