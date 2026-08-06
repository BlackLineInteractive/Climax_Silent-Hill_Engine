#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Climax Silent Hill audio
//
// Four containers ship on the retail PS2 disc, and all of them end in one of
// two codecs:
//
//   rwaID_WAVEDICT   a section inside every level container: the level's own
//                    sound bank. 2980 named samples across the 255 containers,
//                    all mono Sony 4-bit ADPCM ("VAG").
//   MUSIC/*.RWS      RenderWare Audio streams, 75 music tracks, mono VAG.
//   IGC.ARC/*.IGCStream
//                    35 cutscene streams; each embeds a Sony ADS block that is
//                    48 kHz 16-bit stereo PCM with a 512-byte interleave.
//   *.ads / *.vag    the same two formats as loose files.
//
// See docs/SH_FORMAT.md section 8 for the byte layouts and the verification
// figures behind each claim.
// ---------------------------------------------------------------------------

// One decoded sound, ready to hand to SDL. `pcm` is interleaved when
// channels == 2.
struct AudioClip {
    std::string          name;              // sample or track name
    std::string          source;            // where it came from, for the panel
    std::string          codec;             // "VAG (Sony ADPCM)" / "PCM16"
    int                  sampleRate = 44100;
    int                  channels   = 1;
    std::vector<int16_t> pcm;

    bool  Valid()   const { return !pcm.empty() && sampleRate > 0 && channels > 0; }
    float Seconds() const {
        return Valid() ? (float)pcm.size() / (float)(sampleRate * channels) : 0.0f;
    }
};

namespace Audio {

// Sony 4-bit ADPCM. 16-byte block -> 28 samples; one contiguous mono stream.
// The block decode follows PS2Recomp's ps2_audio_vag.cpp.
void DecodeVAG(const uint8_t* data, size_t size, std::vector<int16_t>& out);

// Sony ADS: 'SShd' header + 'SSbd' body. `data` must point at the 'SShd'.
bool LoadADS(const uint8_t* data, size_t size, AudioClip& out);

// RenderWare Audio stream — the MUSIC/*.RWS tracks.
bool LoadRWS(const uint8_t* data, size_t size, AudioClip& out);

// Cutscene stream from IGC.ARC: an IGC header with an ADS block inside it.
bool LoadIGCStream(const uint8_t* data, size_t size, AudioClip& out);

// Sniff a buffer and decode whatever it turns out to be.
bool LoadBuffer(const uint8_t* data, size_t size, AudioClip& out);
bool LoadFile(const std::string& path, AudioClip& out);

// rwaID_WAVEDICT payload (pointing at the 0x0809 chunk) -> every named sample.
void ParseWaveDictionary(const uint8_t* data, size_t size,
                         std::vector<AudioClip>& out);

// 16-bit PCM .wav, for the panel's export button.
bool WriteWav(const std::string& path, const AudioClip& clip);

} // namespace Audio
