#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// GameCube / Wii native textures
//
// The Hollywood GPU stores textures in tiles rather than scanlines, and in one
// of ten formats. A rwID_TEXDICTIONARY section of a Shattered Memories
// container holds ordinary RenderWare TextureNative chunks whose raster struct
// is big-endian and 108 bytes long; the pixels follow it.
//
// See docs/SHSM_ARC_FORMAT.md section 5 for the header layout and the mip-chain
// size formula this decoder is checked against.
// ---------------------------------------------------------------------------

enum GXTexFormat {
    GX_TF_I4     = 0,
    GX_TF_I8     = 1,
    GX_TF_IA4    = 2,
    GX_TF_IA8    = 3,
    GX_TF_RGB565 = 4,
    GX_TF_RGB5A3 = 5,
    GX_TF_RGBA8  = 6,
    GX_TF_C4     = 8,
    GX_TF_C8     = 9,
    GX_TF_C14X2  = 10,
    GX_TF_CMPR   = 14,
};

struct WiiTexture {
    std::string name;
    int      width  = 0;
    int      height = 0;
    int      format = -1;      // GXTexFormat
    int      mipCount = 0;
    bool     hasAlpha = false;
    uint32_t rasterFormat = 0;
    std::vector<uint8_t> rgba; // width*height*4, top level only
};

namespace Wii {

// Bytes one mip level occupies, rounded up to the format's tile size.
uint32_t LevelSize(int format, int w, int h);

// Total bytes of a whole mip chain -- the value the raster header declares.
uint32_t ChainSize(int format, int w, int h, int levels);

const char* FormatName(int format);

// Decodes one level into straight RGBA8888. False for the paletted formats,
// whose palette this decoder has not located yet.
bool Decode(int format, int w, int h, const uint8_t* data, size_t size,
            std::vector<uint8_t>& rgba);

// Parses a TextureNative raster struct (the payload of its 0x0001 chunk) and
// decodes the top mip level.
bool ReadTextureNative(const uint8_t* struct_, size_t size, WiiTexture& out);

// Walks a rwID_TEXDICTIONARY section payload (pointing at the 0x0016 chunk).
void ReadDictionary(const uint8_t* data, size_t size,
                    std::vector<WiiTexture>& out);

// How many TextureNative chunks a dictionary declares, decodable or not, so a
// caller can report the ones that were skipped.
size_t CountTextures(const uint8_t* data, size_t size);

} // namespace Wii
