// ─────────────────────────────────────────────────────────────────────────────
// The GL half of texture loading, lifted out of the PS2 decoder.
//
// This is everything the decoder used to do itself and had no business doing:
// creating the GL object, registering the name aliases, and filling the panels'
// preview tables. Keeping it here is what lets PS2Texture.cpp sit in
// climax-core, which is checked to link without a single GL symbol.
// ─────────────────────────────────────────────────────────────────────────────
#include <GL/glew.h>

#include <cctype>
#include <string>

#include "ClimaxEngine/Platform/PS2/PS2Texture.h"
#include "ClimaxEngine/Render/ViewerState.h"

void InstallGLTextureSink() {
    SetTextureExists([](const std::string &name) {
        return g_TextureMap.find(name) != g_TextureMap.end();
    });

    SetTextureSink([](RawTexture &raw, const std::vector<uint8_t> &rgba, int w,
                      int h) {
        glGenTextures(1, &raw.glID);
        glBindTexture(GL_TEXTURE_2D, raw.glID);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba.data());

        const GLint wrapS = raw.clampU ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        const GLint wrapT = raw.clampV ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // Materials and texture dictionaries do not agree on capitalisation,
        // and registering only the original plus an all-caps alias missed any
        // spelling in between -- those meshes then bound texture 0 and came out
        // black even though the texture sat right there in the browser.
        std::string upper = raw.name, lower = raw.name;
        for (auto &c : upper) c = (char)toupper((unsigned char)c);
        for (auto &c : lower) c = (char)tolower((unsigned char)c);

        g_TextureMap[raw.name] = raw.glID;
        g_TextureMap[upper] = raw.glID;
        g_TextureMap[lower] = raw.glID;

        g_RawTextures.push_back(raw);

        TexPreviewInfo pi;
        pi.glID = raw.glID;
        pi.width = w;
        pi.height = h;
        pi.depth = raw.depth;
        g_TexOpaque[raw.name] = !raw.hasTransparentTexels;
        g_TexOpaque[upper] = !raw.hasTransparentTexels;
        g_TexOpaque[lower] = !raw.hasTransparentTexels;
        g_TexGradient[raw.name] = raw.hasAlphaGradient;
        g_TexGradient[upper] = raw.hasAlphaGradient;
        g_TexGradient[lower] = raw.hasAlphaGradient;
        g_TexInfo[raw.name] = pi;
        g_TexInfo[upper] = pi;
    });
}
