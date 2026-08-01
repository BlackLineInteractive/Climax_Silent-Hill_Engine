#include "ClimaxEngine/Core/AudioParser.h"
#include <fstream>
#include <iostream>
#include <cmath>
#include <algorithm>
#include <cstring>

static const double VAG_f[5][2] = {
    { 0.0, 0.0 },
    {  60.0 / 64.0,  0.0 },
    { 115.0 / 64.0, -52.0 / 64.0 },
    {  98.0 / 64.0, -55.0 / 64.0 },
    { 122.0 / 64.0, -60.0 / 64.0 }
};

void AudioParser::DecodeADPCMBlock(const uint8_t* block, int16_t* out, double& s1, double& s2) {
    uint8_t shift = block[0] & 0x0F;
    uint8_t filter = (block[0] >> 4) & 0x0F;
    if (filter > 4) filter = 0;

    for (int i = 0; i < 28; i++) {
        uint8_t b = block[2 + (i / 2)];
        int16_t nibble = (i % 2 == 0) ? (b & 0x0F) : ((b >> 4) & 0x0F);
        
        // Sign extend
        nibble = (nibble << 12) >> 12;
        
        double sample = (double)nibble * (1 << (12 - shift));
        sample += s1 * VAG_f[filter][0] + s2 * VAG_f[filter][1];
        
        s2 = s1;
        s1 = sample;
        
        double clamped = std::max(-32768.0, std::min(32767.0, sample));
        out[i] = (int16_t)std::floor(clamped + 0.5);
    }
}

AudioStream AudioParser::Load(const std::string& path) {
    AudioStream stream;
    
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return stream;
    
    size_t fileSize = file.tellg();
    file.seekg(0);
    
    std::vector<uint8_t> data(fileSize);
    file.read((char*)data.data(), fileSize);
    if (data.empty()) return stream;

    // Check for ADS header: "SShd" or similar? ADS format often has "SShd" (0x64685353) or no header.
    // Let's assume standard Interleave of 0x200 bytes per channel for PS2.
    int interleave = 0x200;
    int dataStart = 0;
    
    // Very simple header detection (ADS often starts with SShd or Sony ADPCM)
    if (fileSize > 0x20 && data[0] == 'S' && data[1] == 'S' && data[2] == 'h' && data[3] == 'd') {
        uint32_t headerSize; std::memcpy(&headerSize, &data[4], 4);
        uint32_t sampleRate; std::memcpy(&sampleRate, &data[12], 4);
        uint32_t channels; std::memcpy(&channels, &data[16], 4);
        uint32_t intSize; std::memcpy(&intSize, &data[20], 4);
        
        stream.sampleRate = sampleRate;
        stream.channels = channels;
        interleave = intSize;
        dataStart = headerSize;
    } else {
        // Assume raw PS2 ADPCM stereo, 48000 Hz, 0x200 interleave
        // Some exported files might strip the header
        stream.sampleRate = 48000;
        stream.channels = 2;
        dataStart = 0;
    }
    
    size_t dataLen = fileSize - dataStart;
    int blocksPerInterleave = interleave / 16;
    int samplesPerInterleave = blocksPerInterleave * 28;
    
    // Allocate max possible size
    stream.pcmData.reserve(dataLen / 16 * 28 * stream.channels);
    
    double s1[2] = {0, 0};
    double s2[2] = {0, 0};
    
    size_t cursor = dataStart;
    
    while (cursor < fileSize) {
        for (int ch = 0; ch < stream.channels; ch++) {
            size_t chStart = cursor + ch * interleave;
            for (int b = 0; b < blocksPerInterleave; b++) {
                size_t bOff = chStart + b * 16;
                if (bOff + 16 > fileSize) break;
                
                int16_t decoded[28];
                DecodeADPCMBlock(&data[bOff], decoded, s1[ch], s2[ch]);
                
                // We need to write interleaved output, so we need to calculate index
                // but since we read blocks channel-wise, it's easier to append to individual channel buffers, then interleave.
            }
        }
        cursor += stream.channels * interleave;
    }
    
    // Wait, the above loop doesn't write to pcmData correctly because it's not interleaved.
    // Let's rewrite it to interleave correctly.
    
    std::vector<std::vector<int16_t>> chData(stream.channels);
    
    cursor = dataStart;
    while (cursor < fileSize) {
        bool readAny = false;
        for (int ch = 0; ch < stream.channels; ch++) {
            size_t chStart = cursor + ch * interleave;
            for (int b = 0; b < blocksPerInterleave; b++) {
                size_t bOff = chStart + b * 16;
                if (bOff + 16 > fileSize) continue;
                
                int16_t decoded[28];
                DecodeADPCMBlock(&data[bOff], decoded, s1[ch], s2[ch]);
                for (int i = 0; i < 28; i++) {
                    chData[ch].push_back(decoded[i]);
                }
                readAny = true;
            }
        }
        if (!readAny) break;
        cursor += stream.channels * interleave;
    }
    
    size_t numSamples = chData[0].size();
    stream.pcmData.resize(numSamples * stream.channels);
    for (size_t i = 0; i < numSamples; i++) {
        for (int ch = 0; ch < stream.channels; ch++) {
            if (i < chData[ch].size()) {
                stream.pcmData[i * stream.channels + ch] = chData[ch][i];
            } else {
                stream.pcmData[i * stream.channels + ch] = 0;
            }
        }
    }
    
    stream.valid = true;
    return stream;
}
