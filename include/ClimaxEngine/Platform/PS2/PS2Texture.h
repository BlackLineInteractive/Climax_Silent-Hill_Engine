#pragma once
#include "ClimaxEngine/Core/Common.h"

std::vector<uint8_t> Unswizzle8(const std::vector<uint8_t>& buf, int w, int h);
std::vector<uint8_t> UnswizzlePalette(const std::vector<uint8_t>& pal);
void ProcessAndUploadTexture(RawTexture& raw);

// Uploads pixels that are already straight RGBA8888 and need no PS2 alpha
// conversion -- the GameCube/Wii decoder produces those directly.
void UploadDecodedTexture(RawTexture& raw, const std::vector<uint8_t>& rgba);
