#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct AudioStream {
    int sampleRate = 48000;
    int channels = 2;
    std::vector<int16_t> pcmData; // Interleaved PCM (L R L R ...)
    bool valid = false;
};

class AudioParser {
public:
    // Load audio from an .ads, .abc, or .IGC file
    static AudioStream Load(const std::string& path);

private:
    // Decodes a single 16-byte PS2 ADPCM block into 28 PCM samples
    static void DecodeADPCMBlock(const uint8_t* block, int16_t* out, double& s1, double& s2);
};
