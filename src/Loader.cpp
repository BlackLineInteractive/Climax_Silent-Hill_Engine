#include "Loader.h"
#include "Arc.h"
#include "Common.h"
#include "PS2Texture.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <iostream>
#include <algorithm>
#include <vector>
#include <map>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// --- УТИЛІТИ ---
static size_t FindPattern(const std::vector<uint8_t>& d, const std::vector<uint8_t>& p, size_t s) {
    if (d.size() < p.size()) return -1;
    for (size_t i = s; i <= d.size() - p.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < p.size(); ++j)
            if (d[i + j] != p[j]) { match = false; break; }
        if (match) return i;
    }
    return -1;
}

std::vector<uint8_t> ReadWholeFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamoff len = f.tellg();
    if (len <= 0) return {};
    f.seekg(0);
    std::vector<uint8_t> data((size_t)len);
    if (!f.read((char*)data.data(), len)) return {};
    return data;
}

// ------------------- LOADER LOGIC -------------------
void LoadTexturesFromTxdData(const std::vector<uint8_t>& data,
                             const std::vector<std::string>& allowedNames, bool fallback) {
    const size_t sz = data.size();
    // `pos < sz - 100` underflows on any file smaller than 100 bytes and turns the
    // loop bound into SIZE_MAX, walking off the end of the buffer.
    if (sz < 128) return;

    const int MAGIC_OFFSET = 80;
    size_t pos = 0;

    while (pos < sz - 100) {
        uint32_t type; memcpy(&type, &data[pos], 4);
        if (type == 0x15) {
            try {
                uint32_t chunkSz; memcpy(&chunkSz, &data[pos + 4], 4);
                size_t curr = pos + 32;
                uint32_t sLen; memcpy(&sLen, &data[curr + 4], 4);
                std::string tName = (sLen < 128) ? std::string((char*)&data[curr + 12]) : "Unknown";
                curr += 12 + sLen; memcpy(&sLen, &data[curr + 4], 4); curr += 12 + sLen; curr += 24;

                uint32_t w, h, d;
                memcpy(&w, &data[curr], 4); memcpy(&h, &data[curr + 4], 4); memcpy(&d, &data[curr + 8], 4);
                uint32_t dSz, pSz;
                memcpy(&dSz, &data[curr + 48], 4); memcpy(&pSz, &data[curr + 52], 4);
                uint32_t rawWrapU, rawWrapV;
                memcpy(&rawWrapU, &data[curr + 56], 4);
                memcpy(&rawWrapV, &data[curr + 60], 4);
                curr += 76;

                RawTexture t;
                t.name = tName; t.width = w; t.height = h; t.depth = d;
                t.clampU = (rawWrapU != 0);
                t.clampV = (rawWrapV != 0);

                bool nameAllowed = fallback;
                if (!fallback) {
                    for (const auto& allowed : allowedNames) {
                        if (sho_stricmp(t.name.c_str(), allowed.c_str()) == 0) {
                            nameAllowed = true;
                            break;
                        }
                    }
                }
                if (!nameAllowed) {
                    pos += 12 + chunkSz; continue;
                }

                if (curr + MAGIC_OFFSET + (dSz - MAGIC_OFFSET) <= sz && dSz > MAGIC_OFFSET) {
                    t.pixels.resize(dSz - MAGIC_OFFSET);
                    memcpy(t.pixels.data(), &data[curr + MAGIC_OFFSET], dSz - MAGIC_OFFSET);
                }
                curr += dSz;
                if (pSz > MAGIC_OFFSET && curr + MAGIC_OFFSET + (pSz - MAGIC_OFFSET) <= sz) {
                    t.palette.resize(pSz - MAGIC_OFFSET);
                    memcpy(t.palette.data(), &data[curr + MAGIC_OFFSET], pSz - MAGIC_OFFSET);
                }

                if (!t.pixels.empty() && g_TextureMap.find(t.name) == g_TextureMap.end()) {
                    ProcessAndUploadTexture(t);
                }

                pos += 12 + chunkSz; continue;
            }
            catch (...) {}
        }
        pos++;
    }
}

void LoadGeometryData(const std::vector<uint8_t>& data) {
    const size_t sz = data.size();
    if (sz < 32) return;

    g_MaterialNames.clear();

    // Helper to safely read a uint32 from the buffer
    auto ru32 = [&](size_t off) -> uint32_t {
        if (off + 4 > sz) return 0;
        uint32_t v; memcpy(&v, &data[off], 4); return v;
    };

    // --- HELPER: parse material names from a MaterialList chunk at ml_pos ---
    // Returns a vector<string> with one name per material (or "NULL").
    auto parseMaterialList = [&](size_t ml_pos) -> std::vector<std::string> {
        std::vector<std::string> names;
        size_t mlDataOff = ml_pos + 12; // skip chunk header
        uint32_t fcType = ru32(mlDataOff);
        uint32_t fcSize = ru32(mlDataOff + 4);
        if (fcType != 0x01 || fcSize < 4 || mlDataOff + 12 + fcSize > sz) return names;
        uint32_t numMat = ru32(mlDataOff + 12);
        if (numMat == 0 || numMat > 512) return names;
        size_t curr = mlDataOff + 12 + fcSize; // skip Struct chunk entirely
        for (uint32_t m = 0; m < numMat; m++) {
            if (curr + 12 >= sz) break;
            uint32_t matType = ru32(curr);
            uint32_t matSize = ru32(curr + 4);
            if (matType != 0x07 || matSize == 0 || curr + 12 + matSize > sz) break;
            size_t matEnd = curr + 12 + matSize;
            size_t child  = curr + 12;
            std::string texName = "NULL";
            while (child + 12 < matEnd) {
                uint32_t cType = ru32(child);
                uint32_t cSize = ru32(child + 4);
                if (cSize == 0 || child + 12 + cSize > matEnd) break;
                if (cType == 0x06) { // Texture chunk
                    size_t texChild = child + 12;
                    size_t texEnd   = child + 12 + cSize;
                    while (texChild + 12 < texEnd) {
                        uint32_t tcType = ru32(texChild);
                        uint32_t tcSize = ru32(texChild + 4);
                        if (tcSize == 0 || texChild + 12 + tcSize > texEnd) break;
                        if (tcType == 0x02) { // String = texture name
                            size_t nameLen = strnlen((char*)&data[texChild + 12], tcSize);
                            texName = std::string((char*)&data[texChild + 12], nameLen);
                            break;
                        }
                        texChild += 12 + tcSize;
                    }
                    break;
                }
                child += 12 + cSize;
            }
            names.push_back(texName);
            curr = matEnd;
        }
        return names;
    };

    // --- STEP 1: Collect all valid MaterialList positions ---
    std::vector<size_t> allMlPos;
    {
        size_t search = 0;
        while (search + 12 < sz) {
            size_t cand = FindPattern(data, {0x08,0x00,0x00,0x00}, search);
            if (cand == (size_t)-1) break;
            uint32_t mlSz = ru32(cand + 4);
            if (mlSz < 12 || cand + 12 + mlSz > sz) { search = cand + 1; continue; }
            size_t fc = cand + 12;
            if (ru32(fc) != 0x01 || ru32(fc+4) < 4 || fc + 12 + ru32(fc+4) > sz) { search = cand+1; continue; }
            uint32_t numMat = ru32(fc + 12);
            if (numMat == 0 || numMat > 512) { search = cand + 1; continue; }
            size_t fm = fc + 12 + ru32(fc+4);
            if (fm + 12 >= sz || ru32(fm) != 0x07) { search = cand + 1; continue; }
            allMlPos.push_back(cand);
            search = cand + 1;
        }
    }

    // --- STEP 2: Collect all BinMesh sections, pair with nearest preceding ML ---
    struct BatchInfo { int matIndex; int vertexQuota; };

    struct GeoObject {
        size_t bmOff;
        std::vector<BatchInfo> batches;
        std::vector<std::string> matNames;
        size_t vifStart, vifEnd; // range in file where VIF vertex data lives (AFTER bmOff)
    };
    std::vector<GeoObject> geoObjs;

    {
        size_t search = 0;
        while (search < sz) {
            size_t cand = FindPattern(data, {0x0E,0x05,0x00,0x00}, search);
            if (cand == (size_t)-1) break;
            uint32_t flags     = ru32(cand + 12);
            uint32_t numMeshes = ru32(cand + 16);
            if (numMeshes == 0 || numMeshes > 4096) { search = cand + 1; continue; }
            bool isTriList = (flags == 0);

            GeoObject go;
            go.bmOff = cand;

            // Parse batches from this BinMesh
            size_t curr = cand + 24;
            for (uint32_t i = 0; i < numMeshes; i++) {
                if (curr + 8 > sz) break;
                uint32_t numIndices = ru32(curr);
                uint32_t matIdx     = ru32(curr + 4);
                if (numIndices > 65536) break;
                go.batches.push_back({(int)matIdx, (int)numIndices});
                size_t skip = 8 + (isTriList ? (size_t)numIndices * 4 : 0);
                if (curr + skip > sz) break;
                curr += skip;
            }
            if (go.batches.empty()) { search = cand + 1; continue; }

            // Pair with nearest preceding MaterialList
            size_t bestMl = (size_t)-1;
            for (size_t mlp : allMlPos)
                if (mlp < cand) bestMl = mlp;
            if (bestMl != (size_t)-1)
                go.matNames = parseMaterialList(bestMl);

            geoObjs.push_back(std::move(go));
            search = cand + 1;
        }
    }

    if (geoObjs.empty()) {
        // Fallback: single dummy object with no batches
        geoObjs.push_back({0, {{0, 9999999}}, {}, 0, sz});
    }

    // Sort by BinMesh offset — VIF data lives AFTER the BinMesh
    std::sort(geoObjs.begin(), geoObjs.end(),
        [](const GeoObject& a, const GeoObject& b){ return a.bmOff < b.bmOff; });
    for (size_t i = 0; i < geoObjs.size(); i++) {
        geoObjs[i].vifStart = geoObjs[i].bmOff;
        geoObjs[i].vifEnd   = (i + 1 < geoObjs.size()) ? geoObjs[i+1].bmOff : sz;
    }

    // Build global g_MaterialNames as ALL names from ALL objects (union, in order)
    // and fill materialIndex with a global index (BinMesh matIdx offset by each object's base)
    std::vector<size_t> matBase(geoObjs.size(), 0); // global index base per object
    {
        size_t base = 0;
        for (size_t i = 0; i < geoObjs.size(); i++) {
            matBase[i] = base;
            for (const auto& n : geoObjs[i].matNames)
                g_MaterialNames.push_back(n);
            base += geoObjs[i].matNames.size();
        }
    }

    // --- STEP 3: Build mesh chunks ---
    for (auto& chunk : g_Chunks) {
        if (chunk.vao) glDeleteVertexArrays(1, &chunk.vao);
        if (chunk.vbo) glDeleteBuffers(1, &chunk.vbo);
    }
    g_Chunks.clear();

    std::vector<uint8_t> MV   = {0x05,0x04,0x01,0x00,0x01,0x00};
    std::vector<uint8_t> MUV  = {0x05,0x04,0x01,0x00,0x01,0x01};
    std::vector<uint8_t> MCOL = {0x05,0x04,0x01,0x00,0x01,0x02};

    // Per-object batch tracking
    std::vector<int> batchIdx(geoObjs.size(), 0);
    std::vector<int> batchAcc(geoObjs.size(), 0);

    size_t pos = 0;
    while (true) {
        pos = FindPattern(data, MV, pos);
        if (pos == (size_t)-1) break;
        size_t cS = pos; pos += 7;
        size_t next = FindPattern(data, MV, pos);
        if (next != (size_t)-1 && (next - pos) < 50) { pos = next + 7; cS = pos - 7; }
        if (pos >= sz) break;

        // Determine which GeoObject this block belongs to
        int geoIdx = -1;
        for (int g = (int)geoObjs.size() - 1; g >= 0; g--) {
            if (cS >= geoObjs[g].vifStart && cS < geoObjs[g].vifEnd) {
                geoIdx = g;
                break;
            }
        }
        if (geoIdx < 0) { pos = cS + 20; continue; } // not in any known range

        GeoObject& go = geoObjs[geoIdx];

        int vnum = data[pos++];
        uint8_t fmtByte = data[pos++];
        bool isV4_16 = (fmtByte == 0x6C);
        int stride = isV4_16 ? 16 : 12;

        std::vector<bool> adcFlags(vnum, false);
        bool hasAdc = false;

        std::vector<Vertex> rawVerts;
        size_t vRead = pos;
        for (int i = 0; i < vnum; i++) {
            Vertex v;
            memcpy(&v.pos, &data[vRead], 12);
            v.uv    = {0, 0};
            v.color = {1.0f, 1.0f, 1.0f, 1.0f};
            if (isV4_16 && vRead + 14 < sz) {
                uint16_t w;
                memcpy(&w, &data[vRead + 12], 2);
                if (w != 0) { adcFlags[i] = true; hasAdc = true; }
            }
            rawVerts.push_back(v);
            vRead += stride;
        }

        size_t nextMV   = FindPattern(data, MV, cS + 1);
        size_t searchEnd = (nextMV != (size_t)-1) ? nextMV : sz;

        size_t uvPos = FindPattern(data, MUV, cS);
        if (uvPos != (size_t)-1 && uvPos < searchEnd) {
            uint8_t uvFmt  = (uvPos + 8 < sz) ? data[uvPos + 8] : 0x64;
            bool    useI16 = (uvFmt == 0x65);
            int     uvStride = useI16 ? 4 : 8;
            uvPos += 9;
            for (int i = 0; i < vnum && i < (int)rawVerts.size(); i++) {
                if (uvPos + (size_t)uvStride <= sz) {
                    if (useI16) {
                        int16_t u_i, v_i;
                        memcpy(&u_i, &data[uvPos],     2);
                        memcpy(&v_i, &data[uvPos + 2], 2);
                        rawVerts[i].uv.x = (float)u_i / 4096.0f;
                        rawVerts[i].uv.y = (float)v_i / 4096.0f;
                    } else {
                        float u_f, v_f;
                        memcpy(&u_f, &data[uvPos],     4);
                        memcpy(&v_f, &data[uvPos + 4], 4);
                        if (std::abs(u_f) > 64.0f || std::abs(v_f) > 64.0f ||
                            std::isnan(u_f) || std::isnan(v_f)) {
                            int16_t u_i, v_i;
                            memcpy(&u_i, &data[uvPos],     2);
                            memcpy(&v_i, &data[uvPos + 2], 2);
                            rawVerts[i].uv.x = (float)u_i / 4096.0f;
                            rawVerts[i].uv.y = (float)v_i / 4096.0f;
                        } else {
                            rawVerts[i].uv.x = u_f;
                            rawVerts[i].uv.y = v_f;
                        }
                    }
                    uvPos += uvStride;
                }
            }
        }

        size_t colPos = FindPattern(data, MCOL, cS);
        if (colPos != (size_t)-1 && colPos < searchEnd) {
            colPos += 9;
            for (int i = 0; i < vnum && i < (int)rawVerts.size(); i++) {
                if (colPos + 4 <= sz) {
                    uint8_t r = data[colPos + 0], g = data[colPos + 1];
                    uint8_t b = data[colPos + 2], a = data[colPos + 3];
                    // PS2 vertex colour range: 0-128 where 128 = full white (not 0-255)
                    rawVerts[i].color = glm::vec4(
                        std::min(r * (1.0f / 128.0f), 1.0f),
                        std::min(g * (1.0f / 128.0f), 1.0f),
                        std::min(b * (1.0f / 128.0f), 1.0f),
                        std::min(a * (1.0f / 128.0f), 1.0f));
                    colPos += 4;
                }
            }
        }

        std::vector<Vertex> triVerts;
        bool prevWasRestart = true;
        int  stripPos       = 0;
        for (int i = 2; i < vnum; i++) {
            Vertex& v1 = rawVerts[i - 2];
            Vertex& v2 = rawVerts[i - 1];
            Vertex& v3 = rawVerts[i];
            bool skip = hasAdc ? adcFlags[i]
                               : (v1.pos == v2.pos || v2.pos == v3.pos || v1.pos == v3.pos);
            if (skip) { prevWasRestart = true; continue; }
            if (prevWasRestart) { stripPos = 0; prevWasRestart = false; }
            if (stripPos % 2 == 0) { triVerts.push_back(v1); triVerts.push_back(v2); triVerts.push_back(v3); }
            else                   { triVerts.push_back(v2); triVerts.push_back(v1); triVerts.push_back(v3); }
            stripPos++;
        }

        if (!triVerts.empty()) {
            MeshChunk m;
            m.vertices = triVerts;

            // Assign material from this object's batch list
            int& bi  = batchIdx[geoIdx];
            int& acc = batchAcc[geoIdx];
            if (bi < (int)go.batches.size()) {
                int globalMatIdx = (int)matBase[geoIdx] + go.batches[bi].matIndex;
                m.materialIndex  = globalMatIdx;
                m.texName        = (globalMatIdx < (int)g_MaterialNames.size())
                                   ? g_MaterialNames[globalMatIdx] : "NULL";
                acc += vnum;
                if (acc >= go.batches[bi].vertexQuota) { bi++; acc = 0; }
            } else {
                m.materialIndex = (int)matBase[geoIdx];
                m.texName       = go.matNames.empty() ? "NULL" : go.matNames[0];
            }

            // Remember which 0x0716 section this geometry lives in. World
            // sections draw with identity; model sections are placed by the game
            // objects that reference them.
            m.sectionIndex = -1;
            for (size_t si = 0; si < g_ShoSections.size(); si++) {
                const auto& sec = g_ShoSections[si];
                if (cS >= sec.offset && cS < (size_t)sec.offset + 12 + sec.size) {
                    m.sectionIndex = (int)si;
                    break;
                }
            }

            glGenVertexArrays(1, &m.vao);
            glGenBuffers(1, &m.vbo);
            glBindVertexArray(m.vao);
            glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
            glBufferData(GL_ARRAY_BUFFER, (GLsizei)(m.vertices.size() * sizeof(Vertex)),
                         m.vertices.data(), GL_STATIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));
            glEnableVertexAttribArray(2);
            g_Chunks.push_back(std::move(m));
        }
        pos = cS + 20;
    }
}

void LoadLevelData(const std::string& displayName,
                   const std::vector<uint8_t>& container,
                   const std::vector<NamedBlob>& txds) {
    // Every texture is registered under both its original and its upper-case name,
    // so delete each GL object once instead of once per alias.
    std::vector<GLuint> uniqueIds;
    for (auto& [name, id] : g_TextureMap)
        if (std::find(uniqueIds.begin(), uniqueIds.end(), id) == uniqueIds.end())
            uniqueIds.push_back(id);
    if (!uniqueIds.empty())
        glDeleteTextures((GLsizei)uniqueIds.size(), uniqueIds.data());
    g_TextureMap.clear();
    // g_TexInfo held the same GL ids and was never cleared, so the texture browser
    // kept drawing with names that had just been deleted.
    g_TexInfo.clear();

    // Structure first: LoadGeometryData needs the section ranges to know which
    // section each mesh belongs to, and therefore how it should be placed.
    ParseContainerStructureData(container);
    LoadGeometryData(container);

    // 1. Спочатку шукаємо строго за іменем
    for (const auto& [name, blob] : txds)
        LoadTexturesFromTxdData(blob, g_MaterialNames, false);

    // 2. Якщо лишились незаповнені слоти – fallback
    std::vector<std::string> missing;
    for (const auto& mat : g_MaterialNames)
        if (g_TextureMap.find(mat) == g_TextureMap.end())
            missing.push_back(mat);
    if (!missing.empty())
        for (const auto& [name, blob] : txds)
            LoadTexturesFromTxdData(blob, missing, true);

    // Textures may also be embedded directly in the container.
    if (g_TextureMap.empty())
        LoadTexturesFromTxdData(container, g_MaterialNames, true);

    g_CurrentMeshContainer = displayName;
    g_CurrentTxdPaths.clear();
    for (const auto& [name, blob] : txds) g_CurrentTxdPaths.push_back(name);

    // Build per-texture mesh-chunk index map using the directly stored texName
    g_MeshTexMap.clear();
    for (int ci = 0; ci < (int)g_Chunks.size(); ci++) {
        const std::string& tName = g_Chunks[ci].texName;
        g_MeshTexMap[tName.empty() ? "NULL" : tName].push_back(ci);
    }
}

// ── Path-based wrappers ─────────────────────────────────────────────────────
void LoadTexturesFromTxd(const std::string& txdPath,
                         const std::vector<std::string>& allowedNames, bool fallback) {
    LoadTexturesFromTxdData(ReadWholeFile(txdPath), allowedNames, fallback);
}

void LoadGeometry(const std::string& geomPath) {
    LoadGeometryData(ReadWholeFile(geomPath));
}

void ParseContainerStructure(const std::string& path) {
    ParseContainerStructureData(ReadWholeFile(path));
}

void LoadLevel(const std::string& meshContainerPath, const std::vector<std::string>& txdPaths) {
    std::vector<NamedBlob> txds;
    txds.reserve(txdPaths.size());
    for (const auto& p : txdPaths) {
        auto blob = ReadWholeFile(p);
        if (!blob.empty()) txds.emplace_back(p, std::move(blob));
    }
    LoadLevelData(meshContainerPath, ReadWholeFile(meshContainerPath), txds);
}

// ── Archive entry point ─────────────────────────────────────────────────────
bool LoadLevelFromArc(int entryIndex) {
    if (!g_Arc.IsOpen() || entryIndex < 0 ||
        entryIndex >= (int)g_Arc.Entries().size()) return false;

    const std::string& name = g_Arc.Entries()[entryIndex].name;

    std::vector<uint8_t> container;
    if (!g_Arc.Read((size_t)entryIndex, container) || container.empty()) {
        std::cerr << "[arc] cannot inflate container '" << name << "'\n";
        return false;
    }

    std::vector<NamedBlob> txds;
    for (int ti : g_Arc.TxdsFor(name)) {
        std::vector<uint8_t> blob;
        if (g_Arc.Read((size_t)ti, blob) && !blob.empty())
            txds.emplace_back(g_Arc.Entries()[ti].name, std::move(blob));
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
static void ParseGameObject(const std::vector<uint8_t>& data, size_t off, uint32_t size) {
    const size_t sz    = data.size();
    const size_t body  = off + 12;
    const size_t bodyEnd = body + size;
    if (bodyEnd > sz) return;

    auto ru32l = [&](size_t o) -> uint32_t {
        uint32_t v; memcpy(&v, &data[o], 4); return v;
    };
    // Names are NUL-terminated and then padded with 0xBF up to the record size.
    auto readName = [&](size_t o, size_t len) -> std::string {
        size_t end = o;
        while (end < o + len && data[end] != 0x00 && data[end] != 0xBF) ++end;
        return std::string((const char*)&data[o], end - o);
    };

    GameObject go;
    go.offset = (uint32_t)off;

    bool haveClass = false;
    bool haveXform = false;

    size_t p = body + 4;          // the body opens with a 4-byte field we skip
    while (p + 8 <= bodyEnd) {
        const uint32_t rs  = ru32l(p);
        const uint32_t rid = ru32l(p + 4);
        if (rs < 8 || p + rs > bodyEnd) break;

        const size_t payOff = p + 8;
        const size_t payLen = rs - 8;
        const uint32_t kind = rid >> 24;
        const uint32_t idx  = rid & 0x00FFFFFF;

        if (kind == 0x20) {
            if (!haveClass) { go.className = readName(payOff, payLen); haveClass = true; }
        } else if (kind == 0x80) {
            if (go.instName.empty()) go.instName = readName(payOff, payLen);
        } else if (kind == 0x00 && payLen == 16) {
            // A 16-byte property is a reference to a resource section's GUID.
            go.guidRefs.emplace_back((const char*)&data[payOff], 16);
        } else if (kind == 0x00 && idx == 1 && payLen == 64 && !haveXform) {
            // Column-major 4x4; glm::mat4 has the same memory order.
            glm::mat4 m;
            memcpy(&m[0][0], &data[payOff], 64);
            go.transform = m;
            go.position  = glm::vec3(m[3]);
            go.atOrigin  = (glm::length(go.position) < 1e-4f);
            haveXform    = true;
        }
        p += rs;
    }

    if (go.className.empty()) return;

    // Second pass for CColorLight: the payload is spread over two components.
    if (go.className == "CColorLight") {
        go.isLight = true;
        // Property indices restart at 0 for each component of the object, so a
        // group boundary is simply "the index stopped increasing". That is more
        // reliable than guessing which record type delimits a component.
        int  group   = 0;
        long lastIdx = -1;
        size_t q = body + 4;
        while (q + 8 <= bodyEnd) {
            const uint32_t rs  = ru32l(q);
            const uint32_t rid = ru32l(q + 4);
            if (rs < 8 || q + rs > bodyEnd) break;
            const size_t payOff = q + 8, payLen = rs - 8;
            const uint32_t kind = rid >> 24, idx = rid & 0x00FFFFFF;

            if (kind == 0x00) {
                if ((long)idx <= lastIdx) { ++group; }
                lastIdx = (long)idx;

                if (payLen == 4) {
                    float f; memcpy(&f, &data[payOff], 4);
                    if (group == 0 && idx == 0) {
                        go.lightColor = glm::vec3(data[payOff + 0] / 255.0f,
                                                  data[payOff + 1] / 255.0f,
                                                  data[payOff + 2] / 255.0f);
                    } else if (group == 1) {
                        if      (idx == 0) memcpy(&go.lightType, &data[payOff], 4);
                        else if (idx == 1) go.lightAngle = f;
                        else if (idx == 2) go.lightRange = f;
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

void ParseContainerStructureData(const std::vector<uint8_t>& data) {
    g_ContainerChunks.clear();
    g_ShoTypes.clear();
    g_ShoSections.clear();
    g_Clumps.clear();
    g_GameObjects.clear();
    g_Collision.Free();

    const size_t sz = data.size();
    if (sz < 16) return;

    const uint32_t RW_VER = 0x1c020065;

    auto ru32 = [&](size_t off) -> uint32_t {
        if (off + 4 > sz) return 0;
        uint32_t v; memcpy(&v, &data[off], 4); return v;
    };
    auto rf32 = [&](size_t off) -> float {
        if (off + 4 > sz) return 0.f;
        float v; memcpy(&v, &data[off], 4); return v;
    };
    auto safeCStr = [&](size_t off, size_t maxLen) -> std::string {
        size_t end = off;
        while (end < sz && end < off + maxLen && data[end] != 0) ++end;
        return std::string((char*)&data[off], end - off);
    };

    // ── 1. Parse file header (type 0x071c) ────────────────────────
    // header: [type(4)=0x071c][size(4)][version(4)][numEntries(4)] [entries...]
    uint32_t hdrType = ru32(0);
    uint32_t hdrSize = ru32(4);
    if (hdrType == 0x071c && hdrSize > 0 && hdrSize < sz) {
        size_t dirEnd = 0x0c + hdrSize;  // byte after last directory byte
        size_t off    = 0x10;            // entries start after numEntries field
        while (off < dirEnd) {
            if (data[off] == 0) { ++off; continue; }
            // Read null-terminated name
            size_t nameEnd = off;
            while (nameEnd < dirEnd && data[nameEnd] != 0) ++nameEnd;
            if (nameEnd == off) { ++off; continue; }
            std::string name((char*)&data[off], nameEnd - off);
            // Align to 4 bytes, then read count u32
            size_t padEnd = (nameEnd + 1 + 3) & ~size_t(3);
            if (padEnd + 4 > dirEnd) break;
            uint32_t count = ru32(padEnd);
            if (count > 0 && count < 65536) {
                ShoTypeEntry te; te.name = name; te.count = count;
                g_ShoTypes.push_back(std::move(te));
            }
            off = padEnd + 4;
        }
    }

    // ── 2. Walk the top-level chunk list ──────────────────────────
    // Start right after the 0x071C directory when it is sane, so the walk runs
    // chunk-to-chunk instead of byte-scanning through the header.
    size_t off716 = (hdrType == 0x071c && hdrSize > 0 && hdrSize < sz) ? 0x0c + hdrSize : 0;
    while (off716 + 12 < sz) {
        uint32_t t = ru32(off716);
        uint32_t s = ru32(off716 + 4);
        uint32_t v = ru32(off716 + 8);
        if (v != RW_VER || s == 0 || off716 + 12 + s > sz) {
            off716 += 4; continue;
        }
        // 0x0704 chunks are the placed game-object instances; parsed below.
        if (t == 0x0704) {
            ParseGameObject(data, off716, s);
            off716 += 12 + s; continue;
        }
        if (t != 0x716) { off716 += 12 + s; continue; }

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
        if (tagLen > 1024) tagLen = 4;
        size_t guidOff  = inner + 8 + tagLen;
        uint32_t nameLen = ru32(guidOff + 16);
        size_t nameOff  = guidOff + 20;

        std::string secName;
        if (nameLen < 256 && nameOff + nameLen <= sz)
            secName = safeCStr(nameOff, nameLen);

        // Navigate past the two embedded path strings to object_start
        size_t p1off = nameOff + nameLen;
        uint32_t p1len = ru32(p1off);
        size_t p2off = p1off + 4 + (p1len < 1024 ? p1len : 0);
        if (p2off + 4 > sz) { off716 += 12 + s; continue; }
        uint32_t p2len = ru32(p2off);
        size_t objStart = p2off + 4 + (p2len < 1024 ? p2len : 0);

        ShoSection sec;
        sec.offset    = (uint32_t)off716;
        sec.size      = s;
        sec.name      = secName;
        sec.dataStart = (uint32_t)objStart;
        if (guidOff + 16 <= sz) sec.guid.assign((const char*)&data[guidOff], 16);
        // World geometry is baked into level space; everything else is a model
        // that a game object has to place.
        sec.isWorldSpace = (secName == "rwID_WORLD");
        g_ShoSections.push_back(sec);

        // ── 2a. CBSP collision ─────────────────────────────────────
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
                if (!(cv == RW_VER && ct == 0x1100 && cs > 32)) { co += 4; continue; }
                {
                    // data starts at co+12
                    size_t doff = co + 12;
                    uint32_t numVerts = ru32(doff + 8);   // group 0 vertex count
                    uint32_t numNodes = ru32(doff + 12);  // BSP node count (8 bytes each)
                    if (numVerts > 0 && numVerts < 4096 &&
                        doff + 32 + numVerts*16 <= cs + co + 12) {
                        // Extract vertices (x,y,z, flags) 16 bytes each
                        size_t vbase = doff + 32;
                        for (uint32_t vi = 0; vi < numVerts; ++vi) {
                            float x = rf32(vbase + vi*16 + 0);
                            float y = rf32(vbase + vi*16 + 4);
                            float z = rf32(vbase + vi*16 + 8);
                            if (std::isfinite(x) && std::isfinite(z))
                                g_Collision.verts.push_back({x, y, z});
                        }
                        // Triangle indices (u8 packed: a,b,c,flags) after vert+node data
                        size_t faceOff = vbase + numVerts*16 + numNodes*8;
                        size_t faceEnd = co + 12 + cs;
                        uint32_t nv = (uint32_t)g_Collision.verts.size();
                        while (faceOff + 4 <= faceEnd) {
                            uint8_t a = data[faceOff], b = data[faceOff+1],
                                    c = data[faceOff+2];
                            faceOff += 4;
                            if (a < nv && b < nv && c < nv &&
                                a != b && b != c && a != c) {
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
                if (cv != RW_VER || cs == 0) { co += 4; continue; }

                if (ct == 0x0e) { // FrameList
                    // Struct child of FrameList has the actual frame data
                    size_t st_off = co + 12;
                    uint32_t st_t = ru32(st_off);
                    uint32_t st_s = ru32(st_off + 4);
                    uint32_t st_v = ru32(st_off + 8);
                    if (st_t == 0x01 && st_v == RW_VER && st_s >= 4) {
                        uint32_t numFrames = ru32(st_off + 12);
                        // Each frame: rot(9×f32=36b) + pos(3×f32=12b) + parentIdx(i32=4b) + flags(4b) = 56b
                        const size_t FS = 56;
                        if (numFrames > 0 && numFrames <= 256 &&
                            st_off + 12 + 4 + numFrames * FS <= sz) {

                            size_t fBase = st_off + 12 + 4; // skip numFrames field

                            // Build full world-transform for every frame by composing parent chain
                            std::vector<glm::mat4> worldMats(numFrames, glm::mat4(1.0f));
                            std::vector<int32_t>  parents(numFrames, -1);

                            for (uint32_t fi = 0; fi < numFrames; ++fi) {
                                size_t fb = fBase + fi * FS;
                                // Build 4×4 from local rot + pos (column-major: right/up/at as cols)
                                glm::mat4 local(1.0f);
                                for (int r = 0; r < 3; ++r)
                                    for (int c = 0; c < 3; ++c)
                                        local[c][r] = rf32(fb + (r * 3 + c) * 4);
                                local[3][0] = rf32(fb + 36);
                                local[3][1] = rf32(fb + 40);
                                local[3][2] = rf32(fb + 44);

                                int32_t par; memcpy(&par, &data[fb + 48], 4);
                                parents[fi] = par;
                                if (par >= 0 && par < (int32_t)fi)
                                    worldMats[fi] = worldMats[par] * local;
                                else
                                    worldMats[fi] = local;
                            }

                            // Pick the best frame: prefer frame 0 (world root) unless its
                            // position is near-zero, in which case walk to first non-zero child
                            int chosen = 0;
                            glm::vec3 pos0(worldMats[0][3]);
                            if (glm::length(pos0) < 0.01f && numFrames > 1) {
                                for (uint32_t fi = 1; fi < numFrames; ++fi) {
                                    glm::vec3 pfi(worldMats[fi][3]);
                                    if (glm::length(pfi) > 0.01f) { chosen = (int)fi; break; }
                                }
                            }

                            const glm::mat4& tm = worldMats[chosen];
                            ClumpObject co2;
                            co2.sectionName = secName;
                            co2.label       = "CLUMP " + std::to_string(g_Clumps.size());
                            co2.position    = glm::vec3(tm[3]);
                            co2.transform   = tm;
                            co2.meshStart   = -1;
                            co2.meshCount   = 0;
                            g_Clumps.push_back(std::move(co2));
                        }
                    }
                    break;
                }
                co += 12 + cs;
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
    // matrix. rwID_RWS / rwID_CLUMP sections are models, and the same section is
    // frequently referenced by several objects — e.g. in IntroRoad one RWS model
    // is instanced at four different spots. Collect one instance matrix per
    // reference so the renderer can draw the geometry where it actually belongs
    // instead of leaving every model stacked on the origin.
    {
        std::map<std::string, int> byGuid;
        for (size_t i = 0; i < g_ShoSections.size(); i++)
            if (!g_ShoSections[i].guid.empty())
                byGuid.emplace(g_ShoSections[i].guid, (int)i);

        for (const auto& go : g_GameObjects) {
            for (const auto& ref : go.guidRefs) {
                auto it = byGuid.find(ref);
                if (it == byGuid.end()) continue;
                ShoSection& sec = g_ShoSections[it->second];
                if (sec.isWorldSpace) continue;   // already in level space
                sec.instances.push_back(go.transform);
            }
        }
    }

    // ── 3c. Camera objects ────────────────────────────────────────
    // The game places fixed cameras in the level and cuts between them; the
    // object's matrix is the camera's world transform.
    g_Cameras.clear();
    for (const auto& go : g_GameObjects) {
        const bool isCam = go.className.find("Camera") != std::string::npos;
        if (!isCam || go.atOrigin) continue;
        LevelCamera cam;
        cam.name     = go.instName.empty() ? go.className : go.instName;
        cam.position = go.position;
        // Column 2 is the look direction, column 1 is up (RenderWare convention).
        cam.forward  = glm::normalize(glm::vec3(go.transform[2]));
        glm::vec3 up = glm::vec3(go.transform[1]);
        cam.up       = (glm::length(up) > 1e-4f) ? glm::normalize(up) : glm::vec3(0, 1, 0);
        g_Cameras.push_back(std::move(cam));
    }

    // ── 4. Summary ────────────────────────────────────────────────
    {
        size_t placed = 0;
        for (const auto& go : g_GameObjects) if (!go.atOrigin) placed++;
        uint32_t declared = 0;
        for (const auto& t : g_ShoTypes) declared += t.count;
        size_t modelSecs = 0, instances = 0, orphans = 0;
        for (const auto& s : g_ShoSections) {
            if (s.isWorldSpace) continue;
            modelSecs++;
            instances += s.instances.size();
            if (s.instances.empty()) orphans++;
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
    for (auto& te : g_ShoTypes) {
        ContainerChunkInfo ci;
        ci.offset  = 0;
        ci.typeId  = 0;
        ci.label   = te.name + " ×" + std::to_string(te.count);
        g_ContainerChunks.push_back(ci);
    }
}