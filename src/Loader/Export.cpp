#include "ClimaxEngine/Loader/Export.h"
#include "ClimaxEngine/Core/Common.h"
#include "ClimaxEngine/SG/SceneObject.h"
#include "ClimaxEngine/SG/SceneObject.h"
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>
#include <zlib.h>

namespace {

// ── Minimal PNG writer ──────────────────────────────────────────────────────
// Only RGBA8, no interlacing, filter 0 on every scanline. zlib is already a
// dependency (the archive payloads use it), so this avoids pulling in stb.

void PutBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v >> 24)); out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 8));  out.push_back((uint8_t)v);
}

void PngChunk(std::vector<uint8_t>& out, const char tag[4],
              const uint8_t* data, size_t len) {
    PutBE32(out, (uint32_t)len);
    const size_t crcStart = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), data, data + len);
    const uLong c = crc32(0, out.data() + crcStart, (uInt)(4 + len));
    PutBE32(out, (uint32_t)c);
}

std::vector<uint8_t> EncodePNG(const uint8_t* rgba, int w, int h) {
    std::vector<uint8_t> png;
    static const uint8_t SIG[8] = {0x89,'P','N','G','\r','\n',0x1A,'\n'};
    png.insert(png.end(), SIG, SIG + 8);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16);
    ihdr[2] = (uint8_t)(w >> 8);  ihdr[3] = (uint8_t)w;
    ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16);
    ihdr[6] = (uint8_t)(h >> 8);  ihdr[7] = (uint8_t)h;
    ihdr[8] = 8;    // bit depth
    ihdr[9] = 6;    // colour type: RGBA
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    PngChunk(png, "IHDR", ihdr, sizeof(ihdr));

    // Raw scanlines, each prefixed with the filter byte.
    std::vector<uint8_t> raw;
    raw.reserve((size_t)h * ((size_t)w * 4 + 1));
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + (size_t)y * w * 4, rgba + (size_t)(y + 1) * w * 4);
    }

    uLongf bound = compressBound((uLong)raw.size());
    std::vector<uint8_t> comp(bound);
    if (compress2(comp.data(), &bound, raw.data(), (uLong)raw.size(), 6) != Z_OK)
        return {};
    comp.resize(bound);
    PngChunk(png, "IDAT", comp.data(), comp.size());
    PngChunk(png, "IEND", nullptr, 0);
    return png;
}

// ── glTF buffer accumulation ────────────────────────────────────────────────

struct Blob {
    std::vector<uint8_t> data;

    // Appends `len` bytes 4-byte aligned and returns the starting offset.
    size_t Append(const void* src, size_t len) {
        while (data.size() % 4) data.push_back(0);
        const size_t off = data.size();
        const uint8_t* p = (const uint8_t*)src;
        data.insert(data.end(), p, p + len);
        return off;
    }
};

std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) { /* drop control bytes */ }
                else o += c;
        }
    }
    return o;
}

// Reads a GL texture back into RGBA8.
bool ReadTexture(GLuint id, int w, int h, std::vector<uint8_t>& out) {
    if (!id || w <= 0 || h <= 0) return false;
    out.assign((size_t)w * h * 4, 0);
    glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    return glGetError() == GL_NO_ERROR;
}

const TexPreviewInfo* FindTexInfo(const std::string& name) {
    auto it = g_TexInfo.find(name);
    if (it != g_TexInfo.end()) return &it->second;
    std::string up = name;
    for (auto& c : up) c = (char)toupper((unsigned char)c);
    it = g_TexInfo.find(up);
    return it != g_TexInfo.end() ? &it->second : nullptr;
}


} // namespace

bool ExportGLB(const std::string& path, const GlbExportOptions& opt, std::string& error) {
    error.clear();
    
    auto& registrar = ClimaxEngine::SG::CSceneObjectRegistrar::GetInstance();
    if (registrar.GetObjects().empty()) { error = "nothing loaded"; return false; }


    // ── 1. Group every mesh chunk by the texture it uses ────────────────────
    // Instance transforms are baked in here, so a model placed four times
    // contributes four copies of its triangles to its texture's group.
    struct Group {
        std::vector<float> pos, uv;
        std::vector<uint8_t> col;
        float bbMin[3] = { 1e30f,  1e30f,  1e30f};
        float bbMax[3] = {-1e30f, -1e30f, -1e30f};
    };
    std::map<std::string, Group> groups;

    for (auto& obj : registrar.GetObjects()) {
        glm::mat4 x = obj->GetTransform();
        for (MeshChunk* chunk : obj->GetMeshes()) {
            const std::string key = chunk->texName.empty() ? "NULL" : chunk->texName;
            Group& g = groups[key];

            for (const auto& v : chunk->vertices) {
                const glm::vec4 wp = x * glm::vec4(v.pos, 1.0f);
                float vx = wp.x, vy = wp.y, vz = wp.z;
                g.pos.push_back(vx); g.pos.push_back(vy); g.pos.push_back(vz);
                g.uv.push_back(v.uv.x); g.uv.push_back(v.uv.y);
                for (int c = 0; c < 4; c++) {
                    float f = v.color[c];
                    f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
                    g.col.push_back((uint8_t)(f * 255.0f + 0.5f));
                }
                const float p3[3] = { wp.x, wp.y, wp.z };
                for (int c = 0; c < 3; c++) {
                    if (p3[c] < g.bbMin[c]) g.bbMin[c] = p3[c];
                    if (p3[c] > g.bbMax[c]) g.bbMax[c] = p3[c];
                }
            }
        }
    }
    for (auto it = groups.begin(); it != groups.end(); )
        if (it->second.pos.empty()) it = groups.erase(it); else ++it;
    if (groups.empty()) { error = "no geometry to export"; return false; }

    // ── 2. Build the binary buffer + JSON arrays ────────────────────────────
    Blob bin;
    std::ostringstream jViews, jAccessors, jMeshes, jNodes, jMaterials;
    std::ostringstream jImages, jTextures, jSamplers, jLights, jSceneNodes;
    int nViews = 0, nAcc = 0, nMesh = 0, nNode = 0, nMat = 0, nImg = 0, nTex = 0;

    auto comma = [](std::ostringstream& s, int n) { if (n) s << ","; };

    // One sampler: repeat wrapping, linear filtering.
    jSamplers << R"({"magFilter":9729,"minFilter":9729,"wrapS":10497,"wrapT":10497})";

    // Images / textures, one per group that has a texture.
    std::map<std::string, int> texIndex;
    if (opt.embedTextures) {
        for (const auto& [name, g] : groups) {
            const TexPreviewInfo* ti = FindTexInfo(name);
            if (!ti) continue;
            std::vector<uint8_t> rgba;
            if (!ReadTexture(ti->glID, ti->width, ti->height, rgba)) continue;
            std::vector<uint8_t> png = EncodePNG(rgba.data(), ti->width, ti->height);
            if (png.empty()) continue;

            const size_t off = bin.Append(png.data(), png.size());
            comma(jViews, nViews);
            jViews << "{\"buffer\":0,\"byteOffset\":" << off
                   << ",\"byteLength\":" << png.size() << "}";
            const int viewIdx = nViews++;

            comma(jImages, nImg);
            jImages << "{\"bufferView\":" << viewIdx << ",\"mimeType\":\"image/png\""
                    << ",\"name\":\"" << JsonEscape(name) << "\"}";
            comma(jTextures, nTex);
            jTextures << "{\"sampler\":0,\"source\":" << nImg << "}";
            texIndex[name] = nTex;
            nImg++; nTex++;
        }
    }

    // Meshes: one per texture group.
    for (const auto& [name, g] : groups) {
        const size_t count = g.pos.size() / 3;

        const size_t offP = bin.Append(g.pos.data(), g.pos.size() * 4);
        comma(jViews, nViews);
        jViews << "{\"buffer\":0,\"byteOffset\":" << offP
               << ",\"byteLength\":" << g.pos.size() * 4 << ",\"target\":34962}";
        const int vP = nViews++;

        const size_t offT = bin.Append(g.uv.data(), g.uv.size() * 4);
        comma(jViews, nViews);
        jViews << "{\"buffer\":0,\"byteOffset\":" << offT
               << ",\"byteLength\":" << g.uv.size() * 4 << ",\"target\":34962}";
        const int vT = nViews++;

        int vC = -1;
        if (opt.includeVertexColors) {
            const size_t offC = bin.Append(g.col.data(), g.col.size());
            comma(jViews, nViews);
            jViews << "{\"buffer\":0,\"byteOffset\":" << offC
                   << ",\"byteLength\":" << g.col.size() << ",\"target\":34962}";
            vC = nViews++;
        }

        comma(jAccessors, nAcc);
        jAccessors << "{\"bufferView\":" << vP << ",\"componentType\":5126,\"count\":" << count
                   << ",\"type\":\"VEC3\",\"min\":[" << g.bbMin[0] << "," << g.bbMin[1] << ","
                   << g.bbMin[2] << "],\"max\":[" << g.bbMax[0] << "," << g.bbMax[1] << ","
                   << g.bbMax[2] << "]}";
        const int aP = nAcc++;

        comma(jAccessors, nAcc);
        jAccessors << "{\"bufferView\":" << vT << ",\"componentType\":5126,\"count\":" << count
                   << ",\"type\":\"VEC2\"}";
        const int aT = nAcc++;

        int aC = -1;
        if (vC >= 0) {
            comma(jAccessors, nAcc);
            jAccessors << "{\"bufferView\":" << vC << ",\"componentType\":5121"
                       << ",\"normalized\":true,\"count\":" << count << ",\"type\":\"VEC4\"}";
            aC = nAcc++;
        }

        // Material — alpha MASK mirrors the viewer's `discard` on low alpha.
        comma(jMaterials, nMat);
        jMaterials << "{\"name\":\"" << JsonEscape(name) << "\",\"doubleSided\":true"
                   << ",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.1,\"pbrMetallicRoughness\":{"
                   << "\"metallicFactor\":0,\"roughnessFactor\":1";
        auto ti = texIndex.find(name);
        if (ti != texIndex.end())
            jMaterials << ",\"baseColorTexture\":{\"index\":" << ti->second << "}";
        jMaterials << "}}";
        const int matIdx = nMat++;

        comma(jMeshes, nMesh);
        jMeshes << "{\"name\":\"" << JsonEscape(name) << "\",\"primitives\":[{\"attributes\":{"
                << "\"POSITION\":" << aP << ",\"TEXCOORD_0\":" << aT;
        if (aC >= 0) jMeshes << ",\"COLOR_0\":" << aC;
        jMeshes << "},\"material\":" << matIdx << ",\"mode\":4}]}";

        comma(jNodes, nNode);
        jNodes << "{\"name\":\"" << JsonEscape(name) << "\",\"mesh\":" << nMesh << "}";
        comma(jSceneNodes, nNode);
        jSceneNodes << nNode;
        nMesh++; nNode++;
    }

    // Lights, as KHR_lights_punctual.
    int nLights = 0;
    if (opt.includeLights) {
        for (const auto& go : g_GameObjects) {
            if (!go.isLight) continue;
            // Cone angles past 180 degrees mean the light is omnidirectional.
            const bool spot = go.lightAngle > 0.0f && go.lightAngle < 180.0f;
            comma(jLights, nLights);
            jLights << "{\"type\":\"" << (spot ? "spot" : "point") << "\",\"color\":["
                    << go.lightColor.r << "," << go.lightColor.g << "," << go.lightColor.b
                    << "],\"intensity\":1,\"range\":" << go.lightRange;
            if (spot) {
                const double half = go.lightAngle * 0.5 * 3.14159265358979 / 180.0;
                jLights << ",\"spot\":{\"innerConeAngle\":0,\"outerConeAngle\":" << half << "}";
            }
            jLights << ",\"name\":\"" << JsonEscape(go.label) << "\"}";

            const glm::mat4& m = go.transform;
            comma(jNodes, nNode);
            jNodes << "{\"name\":\"" << JsonEscape(go.label) << "\",\"matrix\":[";
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++)
                    jNodes << (c || r ? "," : "") << m[c][r];
            jNodes << "],\"extensions\":{\"KHR_lights_punctual\":{\"light\":" << nLights << "}}}";
            comma(jSceneNodes, nNode);
            jSceneNodes << nNode;
            nNode++; nLights++;
        }
    }

    // ── 3. Assemble the JSON chunk ──────────────────────────────────────────
    std::ostringstream j;
    j << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"ClimaxGameEngineToolkit\"}";
    if (nLights) {
        j << ",\"extensionsUsed\":[\"KHR_lights_punctual\"]"
          << ",\"extensions\":{\"KHR_lights_punctual\":{\"lights\":[" << jLights.str() << "]}}";
    }
    j << ",\"scene\":0,\"scenes\":[{\"nodes\":[" << jSceneNodes.str() << "]}]"
      << ",\"nodes\":["     << jNodes.str()     << "]"
      << ",\"meshes\":["    << jMeshes.str()    << "]"
      << ",\"materials\":[" << jMaterials.str() << "]";
    if (nImg) {
        j << ",\"images\":["   << jImages.str()   << "]"
          << ",\"samplers\":[" << jSamplers.str() << "]"
          << ",\"textures\":[" << jTextures.str() << "]";
    }
    j << ",\"accessors\":["   << jAccessors.str() << "]"
      << ",\"bufferViews\":[" << jViews.str()     << "]"
      << ",\"buffers\":[{\"byteLength\":" << bin.data.size() << "}]}";

    std::string json = j.str();
    while (json.size() % 4) json += ' ';
    while (bin.data.size() % 4) bin.data.push_back(0);

    // ── 4. Write the GLB container ──────────────────────────────────────────
    std::ofstream f(path, std::ios::binary);
    if (!f) { error = "cannot write " + path; return false; }

    const uint32_t total = 12 + 8 + (uint32_t)json.size() + 8 + (uint32_t)bin.data.size();
    auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };

    f.write("glTF", 4); w32(2); w32(total);
    w32((uint32_t)json.size()); w32(0x4E4F534A);      // 'JSON'
    f.write(json.data(), (std::streamsize)json.size());
    w32((uint32_t)bin.data.size()); w32(0x004E4942);  // 'BIN'
    f.write((const char*)bin.data.data(), (std::streamsize)bin.data.size());

    if (!f) { error = "write failed"; return false; }
    return true;
}

