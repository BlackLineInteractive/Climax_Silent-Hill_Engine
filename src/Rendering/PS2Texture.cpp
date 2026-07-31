#include "ClimaxEngine/Rendering/PS2Texture.h"
#include <algorithm>
#include <cstring>
#include <cmath>

std::vector<uint8_t> Unswizzle8(const std::vector<uint8_t>& buf, int w, int h) {
    std::vector<uint8_t> out(w * h);
    if (buf.size() < (size_t)(w * h)) return out;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int block_loc = (y & (~0xf)) * w + (x & (~0xf)) * 2;
            int swap_sel = (((y + 2) >> 2) & 0x1) * 4;
            int posY = (((y & (~3)) >> 1) + (y & 1)) & 0x7;
            int col_loc = posY * w * 2 + ((x + swap_sel) & 0x7) * 4;
            int byte_num = ((y >> 1) & 1) + ((x >> 2) & 2);
            int swizzleid = block_loc + col_loc + byte_num;
            if ((size_t)swizzleid < buf.size()) out[y * w + x] = buf[swizzleid];
        }
    }
    return out;
}

std::vector<uint8_t> UnswizzlePalette(const std::vector<uint8_t>& pal) {
    std::vector<uint8_t> newPal(1024, 0);
    if (pal.size() < 1024) return pal;
    for (int p = 0; p < 256; p++) {
        int pos = ((p & 0xE7) + ((p & 8) << 1) + ((p & 16) >> 1));
        if (pos < 256 && (pos * 4 + 3) < (int)newPal.size() && (p * 4 + 3) < (int)pal.size()) {
            for (int k = 0; k < 4; k++) newPal[pos * 4 + k] = pal[p * 4 + k];
        }
    }
    return newPal;
}

void ProcessAndUploadTexture(RawTexture& raw) {
    int w = raw.width, h = raw.height;
    if (w <= 0 || h <= 0) return;

    std::vector<uint8_t> indices;
    if (raw.depth == 8) {
        indices = Unswizzle8(raw.pixels, w, h);
    } else if (raw.depth == 4) {
        // Unpack nibbles to a byte-per-pixel buffer, then deswizzle at full w×h.
        std::vector<uint8_t> unpacked(w * h, 0);
        for (size_t i = 0; i < raw.pixels.size(); i++) {
            uint8_t val = raw.pixels[i];
            if (i * 2     < unpacked.size()) unpacked[i * 2]     = val & 0xF;
            if (i * 2 + 1 < unpacked.size()) unpacked[i * 2 + 1] = (val >> 4) & 0xF;
        }
        indices = Unswizzle8(unpacked, w, h);
    }

    // The GS stores 256-entry CLUTs interleaved; 16-entry ones are linear.
    // Trim to the raster's declared palette size first, so a 4-bit texture
    // cannot read colours belonging to the padding behind its 16 entries.
    std::vector<uint8_t> pal = raw.palette;
    const size_t wanted = (size_t)std::max(raw.paletteColors, 1) * 4;
    if (pal.size() > wanted) pal.resize(wanted);

    std::vector<uint8_t> finalPal;
    if (raw.paletteColors == 256) finalPal = UnswizzlePalette(pal);
    else finalPal = pal;

    std::vector<uint8_t> rgba(w * h * 4, 255);
    for (int i = 0; i < w * h; ++i) {
        if ((size_t)i >= indices.size()) break;
        uint8_t idx = indices[i];
        if (raw.depth == 4) idx &= 0xF;
        int pIdx = idx * 4;
        if ((size_t)pIdx + 3 < finalPal.size()) {
            rgba[i * 4 + 0] = finalPal[pIdx + 0];
            rgba[i * 4 + 1] = finalPal[pIdx + 1];
            rgba[i * 4 + 2] = finalPal[pIdx + 2];
            rgba[i * 4 + 3] = std::min((int)finalPal[pIdx + 3] * 2, 255);
        }
    }

    glGenTextures(1, &raw.glID);
    glBindTexture(GL_TEXTURE_2D, raw.glID);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    GLint wrapS = raw.clampU ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    GLint wrapT = raw.clampV ? GL_CLAMP_TO_EDGE : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    g_TextureMap[raw.name] = raw.glID;
    std::string upper = raw.name;
    for (auto& c : upper) c = toupper(c);
    g_TextureMap[upper] = raw.glID;

    raw.pixels = rgba;
    g_RawTextures.push_back(raw);

    // Store preview info for the TXD browser window
    TexPreviewInfo pi; pi.glID = raw.glID; pi.width = w; pi.height = h; pi.depth = raw.depth;
    g_TexInfo[raw.name]  = pi;
    g_TexInfo[upper]     = pi;
}
