#include "ClimaxEngine/Platform/Wii/WiiTexture.h"

#include <algorithm>
#include <cstring>

namespace {

inline uint32_t ru32le(const uint8_t* p) {
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

// Tile geometry per format: a level is stored as whole tiles, so its size is
// the dimensions rounded up to these.
struct Tile { int w, h, bits; };
Tile TileOf(int fmt) {
    switch (fmt) {
        case GX_TF_I4:     return {8, 8, 4};
        case GX_TF_C4:     return {8, 8, 4};
        case GX_TF_CMPR:   return {8, 8, 4};
        case GX_TF_I8:     return {8, 4, 8};
        case GX_TF_IA4:    return {8, 4, 8};
        case GX_TF_C8:     return {8, 4, 8};
        case GX_TF_IA8:    return {4, 4, 16};
        case GX_TF_RGB565: return {4, 4, 16};
        case GX_TF_RGB5A3: return {4, 4, 16};
        case GX_TF_C14X2:  return {4, 4, 16};
        case GX_TF_RGBA8:  return {4, 4, 32};
        default:           return {0, 0, 0};
    }
}

inline int RoundUp(int v, int to) { return to ? ((v + to - 1) / to) * to : v; }

inline void Put(std::vector<uint8_t>& rgba, int w, int h, int x, int y,
                uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t* p = rgba.data() + ((size_t)y * w + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

// RGB5A3: the top bit picks the encoding. Set means 5 bits per channel and
// full opacity; clear means 3 bits of alpha and 4 bits per channel.
inline void Rgb5a3(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (v & 0x8000) {
        r = (uint8_t)(((v >> 10) & 0x1F) * 255 / 31);
        g = (uint8_t)(((v >> 5) & 0x1F) * 255 / 31);
        b = (uint8_t)((v & 0x1F) * 255 / 31);
        a = 255;
    } else {
        a = (uint8_t)(((v >> 12) & 0x07) * 255 / 7);
        r = (uint8_t)(((v >> 8) & 0x0F) * 255 / 15);
        g = (uint8_t)(((v >> 4) & 0x0F) * 255 / 15);
        b = (uint8_t)((v & 0x0F) * 255 / 15);
    }
}

inline void Rgb565(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = (uint8_t)(((v >> 11) & 0x1F) * 255 / 31);
    g = (uint8_t)(((v >> 5) & 0x3F) * 255 / 63);
    b = (uint8_t)((v & 0x1F) * 255 / 31);
}

// One 8-byte CMPR sub-block, the GameCube's variant of DXT1: the two endpoint
// colours are big-endian, and the 2-bit indices run from the most significant
// pair of each byte rather than the least, which is the opposite of DXT1 on
// every other platform.
void DecodeCmprBlock(const uint8_t* b, std::vector<uint8_t>& rgba, int w, int h,
                     int x0, int y0) {
    const uint16_t c0 = rbe16(b), c1 = rbe16(b + 2);
    uint8_t pr[4], pg[4], pb[4], pa[4];
    Rgb565(c0, pr[0], pg[0], pb[0]); pa[0] = 255;
    Rgb565(c1, pr[1], pg[1], pb[1]); pa[1] = 255;
    if (c0 > c1) {
        for (int i = 0; i < 3; i++) {
            const uint8_t* a = i == 0 ? pr : (i == 1 ? pg : pb);
            uint8_t* d = i == 0 ? pr : (i == 1 ? pg : pb);
            d[2] = (uint8_t)((2 * a[0] + a[1]) / 3);
            d[3] = (uint8_t)((a[0] + 2 * a[1]) / 3);
        }
        pa[2] = pa[3] = 255;
    } else {
        pr[2] = (uint8_t)((pr[0] + pr[1]) / 2);
        pg[2] = (uint8_t)((pg[0] + pg[1]) / 2);
        pb[2] = (uint8_t)((pb[0] + pb[1]) / 2);
        pa[2] = 255;
        pr[3] = pg[3] = pb[3] = 0;
        pa[3] = 0;                       // index 3 is the transparent slot
    }
    for (int y = 0; y < 4; y++) {
        const uint8_t row = b[4 + y];
        for (int x = 0; x < 4; x++) {
            const int i = (row >> (6 - 2 * x)) & 3;
            Put(rgba, w, h, x0 + x, y0 + y, pr[i], pg[i], pb[i], pa[i]);
        }
    }
}

} // namespace

const char* Wii::FormatName(int f) {
    switch (f) {
        case GX_TF_I4:     return "I4";
        case GX_TF_I8:     return "I8";
        case GX_TF_IA4:    return "IA4";
        case GX_TF_IA8:    return "IA8";
        case GX_TF_RGB565: return "RGB565";
        case GX_TF_RGB5A3: return "RGB5A3";
        case GX_TF_RGBA8:  return "RGBA8";
        case GX_TF_C4:     return "C4";
        case GX_TF_C8:     return "C8";
        case GX_TF_C14X2:  return "C14X2";
        case GX_TF_CMPR:   return "CMPR";
        default:           return "?";
    }
}

uint32_t Wii::LevelSize(int fmt, int w, int h) {
    const Tile t = TileOf(fmt);
    if (!t.bits) return 0;
    return (uint32_t)RoundUp(w, t.w) * (uint32_t)RoundUp(h, t.h) * (uint32_t)t.bits / 8;
}

uint32_t Wii::ChainSize(int fmt, int w, int h, int levels) {
    uint32_t total = 0;
    for (int i = 0; i < levels; i++) {
        total += LevelSize(fmt, w, h);
        w = std::max(1, w >> 1);
        h = std::max(1, h >> 1);
    }
    return total;
}

bool Wii::Decode(int fmt, int w, int h, const uint8_t* d, size_t size,
                 std::vector<uint8_t>& rgba) {
    if (w <= 0 || h <= 0) return false;
    const Tile t = TileOf(fmt);
    if (!t.bits) return false;
    if (size < LevelSize(fmt, w, h)) return false;

    rgba.assign((size_t)w * h * 4, 0);
    const int tw = RoundUp(w, t.w) / t.w;
    const int th = RoundUp(h, t.h) / t.h;
    size_t o = 0;

    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {
            const int x0 = tx * t.w, y0 = ty * t.h;
            switch (fmt) {
                case GX_TF_CMPR:
                    // Four DXT1 sub-blocks per 8x8 tile, in reading order.
                    DecodeCmprBlock(d + o + 0,  rgba, w, h, x0,     y0);
                    DecodeCmprBlock(d + o + 8,  rgba, w, h, x0 + 4, y0);
                    DecodeCmprBlock(d + o + 16, rgba, w, h, x0,     y0 + 4);
                    DecodeCmprBlock(d + o + 24, rgba, w, h, x0 + 4, y0 + 4);
                    o += 32;
                    break;

                case GX_TF_RGBA8: {
                    // 64 bytes: 32 of interleaved alpha/red, then 32 of green/blue.
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) {
                            const size_t i = (size_t)(y * 4 + x) * 2;
                            Put(rgba, w, h, x0 + x, y0 + y, d[o + i + 1],
                                d[o + 32 + i], d[o + 32 + i + 1], d[o + i]);
                        }
                    o += 64;
                    break;
                }

                case GX_TF_RGB5A3:
                case GX_TF_RGB565:
                case GX_TF_IA8: {
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) {
                            const uint16_t v = rbe16(d + o + ((size_t)y * 4 + x) * 2);
                            uint8_t r = 0, g = 0, b = 0, a = 255;
                            if (fmt == GX_TF_RGB5A3) Rgb5a3(v, r, g, b, a);
                            else if (fmt == GX_TF_RGB565) Rgb565(v, r, g, b);
                            else { r = g = b = (uint8_t)(v & 0xFF); a = (uint8_t)(v >> 8); }
                            Put(rgba, w, h, x0 + x, y0 + y, r, g, b, a);
                        }
                    o += 32;
                    break;
                }

                case GX_TF_I8:
                case GX_TF_IA4: {
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 8; x++) {
                            const uint8_t v = d[o + (size_t)y * 8 + x];
                            uint8_t l, a;
                            if (fmt == GX_TF_I8) { l = v; a = 255; }
                            else { l = (uint8_t)((v & 0x0F) * 17); a = (uint8_t)((v >> 4) * 17); }
                            Put(rgba, w, h, x0 + x, y0 + y, l, l, l, a);
                        }
                    o += 32;
                    break;
                }

                case GX_TF_I4: {
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x += 2) {
                            const uint8_t v = d[o + (size_t)y * 4 + x / 2];
                            const uint8_t hi = (uint8_t)((v >> 4) * 17);
                            const uint8_t lo = (uint8_t)((v & 0x0F) * 17);
                            Put(rgba, w, h, x0 + x,     y0 + y, hi, hi, hi, 255);
                            Put(rgba, w, h, x0 + x + 1, y0 + y, lo, lo, lo, 255);
                        }
                    o += 32;
                    break;
                }

                default:
                    return false;   // paletted: handled by DecodePaletted
            }
            if (o > size) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Paletted rasters
//
// C4 and C8 use the very same 108-byte header, but the word at +0x68 is not a
// data size for them, and a 16-bit TLUT sits between the header and the pixels:
// 16 entries for C4, 256 for C8. The arithmetic is exact --
// FX_EN_Glow01 (C8, 64x64, 4 mips) is 108 + 512 + 5440 = 6060 bytes and
// EN_TL_ICETRANSITION_3 (C8, 256x256, 1 mip) is 108 + 512 + 65536 = 66156,
// both matching their declared struct size to the byte.
//
// Skipping these is why a handful of surfaces rendered black: the texture was
// never registered, so the mesh bound texture 0.
// ---------------------------------------------------------------------------
[[maybe_unused]] static bool DecodePaletted(int fmt, int w, int h, const uint8_t* pal,
                           int tlutFormat, const uint8_t* d, size_t size,
                           std::vector<uint8_t>& rgba) {
    const Tile t = TileOf(fmt);
    if (!t.bits) return false;
    if (size < Wii::LevelSize(fmt, w, h)) return false;

    auto entry = [&](uint32_t i, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
        const uint16_t v = rbe16(pal + (size_t)i * 2);
        switch (tlutFormat) {
            case 0:  r = g = b = (uint8_t)(v & 0xFF); a = (uint8_t)(v >> 8); break;
            case 1:  Rgb565(v, r, g, b); a = 255; break;
            default: Rgb5a3(v, r, g, b, a); break;
        }
    };

    rgba.assign((size_t)w * h * 4, 0);
    const int tw = RoundUp(w, t.w) / t.w;
    const int th = RoundUp(h, t.h) / t.h;
    size_t o = 0;
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {
            const int x0 = tx * t.w, y0 = ty * t.h;
            if (fmt == GX_TF_C8) {
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 8; x++) {
                        uint8_t r, g, b, a;
                        entry(d[o + (size_t)y * 8 + x], r, g, b, a);
                        Put(rgba, w, h, x0 + x, y0 + y, r, g, b, a);
                    }
                o += 32;
            } else {                                   // C4
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x += 2) {
                        const uint8_t v = d[o + (size_t)y * 4 + x / 2];
                        uint8_t r, g, b, a;
                        entry((uint8_t)(v >> 4), r, g, b, a);
                        Put(rgba, w, h, x0 + x, y0 + y, r, g, b, a);
                        entry((uint8_t)(v & 0x0F), r, g, b, a);
                        Put(rgba, w, h, x0 + x + 1, y0 + y, r, g, b, a);
                    }
                o += 32;
            }
            if (o > size) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// TextureNative raster struct, 108 bytes, big-endian
// ---------------------------------------------------------------------------

bool Wii::ReadTextureNative(const uint8_t* s, size_t size, WiiTexture& out) {
    if (size < 108) return false;
    if (rbe32(s) != 6) return false;             // platform: GameCube / Wii

    out.name         = std::string((const char*)s + 0x18,
                                   strnlen((const char*)s + 0x18, 32));
    out.rasterFormat = rbe32(s + 0x58);
    out.width        = (int)((s[0x5C] << 8) | s[0x5D]);
    out.height       = (int)((s[0x5E] << 8) | s[0x5F]);
    out.mipCount     = s[0x61];
    out.format       = s[0x62];
    out.hasAlpha     = rbe32(s + 0x64) != 0;

    const uint8_t* pixels = s + 0x6C;
    const size_t avail = size - 0x6C;
    if (out.width <= 0 || out.height <= 0 || out.mipCount <= 0) return false;

    // Paletted rasters are NOT solved, and are deliberately skipped rather than
    // guessed at: a wrong palette turns a black surface into rainbow noise,
    // which is worse. What is known:
    //
    //   * the sizes work out for a 16-bit TLUT of 16 (C4) or 256 (C8) entries
    //     sitting between the header and the pixels -- FX_EN_Glow01 is
    //     108 + 512 + 5440 = 6060 and EN_TL_ICETRANSITION_3 is
    //     108 + 512 + 65536 = 66156, both matching their struct size exactly;
    //   * a palette at +0x68 read as RGB565 is the smoothest of the six
    //     offset/format combinations tried (mean neighbour difference 33.9
    //     against 95-158), but it still decodes EN_TL_ICETRANSITION_3 to noise,
    //     so the palette is somewhere else or is not a plain 16-bit TLUT.
    //
    // 32 of the 4053 textures in the archive are affected; they render black.
    if (out.format == GX_TF_C4 || out.format == GX_TF_C8 ||
        out.format == GX_TF_C14X2)
        return false;

    return Decode(out.format, out.width, out.height, pixels, avail, out.rgba);
}

void Wii::ReadDictionary(const uint8_t* d, size_t size,
                         std::vector<WiiTexture>& out) {
    // Chunk headers are little-endian even though the payload is not.
    if (size < 12 || ru32le(d) != 0x0016) return;
    const uint32_t dictSize = ru32le(d + 4);
    size_t o = 12;
    const size_t end = std::min<size_t>(size, 12 + dictSize);

    if (o + 12 <= end && ru32le(d + o) == 0x0001) o += 12 + ru32le(d + o + 4);

    while (o + 12 <= end) {
        const uint32_t type = ru32le(d + o);
        const uint32_t sz = ru32le(d + o + 4);
        if (o + 12 + sz > end) break;
        if (type == 0x0015) {
            const uint8_t* tn = d + o + 12;
            if (sz >= 12 && ru32le(tn) == 0x0001) {
                const uint32_t ssz = ru32le(tn + 4);
                WiiTexture t;
                if (ReadTextureNative(tn + 12, std::min<size_t>(ssz, sz - 12), t))
                    out.push_back(std::move(t));
            }
        }
        o += 12 + sz;
    }
}

size_t Wii::CountTextures(const uint8_t* d, size_t size) {
    if (size < 12 || ru32le(d) != 0x0016) return 0;
    const uint32_t dictSize = ru32le(d + 4);
    const size_t end = std::min<size_t>(size, 12 + dictSize);
    size_t o = 12, n = 0;
    if (o + 12 <= end && ru32le(d + o) == 0x0001) o += 12 + ru32le(d + o + 4);
    while (o + 12 <= end) {
        const uint32_t sz = ru32le(d + o + 4);
        if (o + 12 + sz > end) break;
        if (ru32le(d + o) == 0x0015) n++;
        o += 12 + sz;
    }
    return n;
}
