#pragma once
#include "ClimaxEngine/Core/Types.h"

#include <functional>

#include "ClimaxEngine/Core/ITextureDecoder.h"

std::vector<uint8_t> Unswizzle8(const std::vector<uint8_t>& buf, int w, int h);
std::vector<uint8_t> UnswizzlePalette(const std::vector<uint8_t>& pal);
void ProcessAndUploadTexture(RawTexture& raw);

// Where decoded pixels go.
//
// The decoder used to call glTexImage2D itself, which is why this file could
// not live in climax-core: a texture decoder has no business knowing what a GPU
// is. It now hands finished RGBA to whoever registered a sink -- the toolkit
// installs one that uploads and registers aliases, climax-play installs its
// own, and a headless converter can install none at all.
//
// With no sink registered the pixels are simply kept on the RawTexture.
using TextureSink =
    std::function<void(RawTexture& raw, const std::vector<uint8_t>& rgba,
                       int width, int height)>;
void SetTextureSink(TextureSink sink);

// "Do you already have this one?" -- the decoder skips a texture the caller
// says it holds. Knowing what is already loaded is the caller's business, not
// the decoder's; with no predicate registered nothing is skipped.
using TextureExistsFn = std::function<bool(const std::string& name)>;
void SetTextureExists(TextureExistsFn fn);

// Uploads pixels that are already straight RGBA8888 and need no PS2 alpha
// conversion -- the GameCube/Wii decoder produces those directly.
void UploadDecodedTexture(RawTexture& raw, const std::vector<uint8_t>& rgba);

namespace ClimaxEngine {
namespace Platform {
namespace PS2 {

class PS2TextureDecoder : public Core::ITextureDecoder {
public:
    void LoadDictionary(const std::vector<uint8_t>& data,
                        const std::vector<std::string>& allowedNames,
                        bool fallback) override;
};

} // namespace PS2
} // namespace Platform
} // namespace ClimaxEngine
