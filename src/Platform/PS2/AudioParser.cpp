#include "ClimaxEngine/Platform/PS2/AudioParser.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace {

inline uint32_t ru32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

inline int16_t clamp16(int32_t v) {
    return (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

// A name field inside a chunk is NUL-terminated but padded out to a 16-byte
// boundary, so it cannot be taken as a string of the whole field.
std::string cstr(const uint8_t* p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) n++;
    return std::string((const char*)p, n);
}

// RenderWare chunk header: [u32 type][u32 size][u32 version], size excluding
// the 12 header bytes. The audio chunks (0x08xx) use the same encoding as the
// graphics ones.
struct Chunk {
    uint32_t type = 0, size = 0, version = 0;
    const uint8_t* payload = nullptr;
};

bool ReadChunk(const uint8_t* d, size_t size, size_t off, Chunk& c) {
    if (off + 12 > size) return false;
    c.type = ru32(d + off);
    c.size = ru32(d + off + 4);
    c.version = ru32(d + off + 8);
    if ((size_t)c.size + off + 12 > size) return false;
    c.payload = d + off + 12;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Sony 4-bit ADPCM
// ---------------------------------------------------------------------------

void Audio::DecodeVAG(const uint8_t* d, size_t size, std::vector<int16_t>& out,
                      Audio::VagState& state) {
    static const int kF0[5] = {0, 60, 115, 98, 122};
    static const int kF1[5] = {0, 0, -52, -55, -60};

    out.reserve(out.size() + size / 16 * 28);
    int32_t s1 = state.s1, s2 = state.s2;

    for (size_t o = 0; o + 16 <= size; o += 16) {
        int shift = d[o] & 0x0F;
        int filter = (d[o] >> 4) & 0x0F;
        if (shift > 12) shift = 9;
        if (filter > 4) filter = 0;

        for (int i = 0; i < 28; ++i) {
            const uint8_t b = d[o + 2 + i / 2];
            int nibble = (i & 1) ? (b >> 4) : (b & 0x0F);
            if (nibble & 8) nibble -= 16;

            const int32_t v = (nibble << (12 - shift)) +
                              ((kF0[filter] * s1 + kF1[filter] * s2 + 32) >> 6);
            const int16_t s = clamp16(v);
            s2 = s1;
            s1 = s;
            out.push_back(s);
        }
    }
    state.s1 = s1;
    state.s2 = s2;
}

void Audio::DecodeVAG(const uint8_t* d, size_t size, std::vector<int16_t>& out) {
    Audio::VagState st;
    DecodeVAG(d, size, out, st);
}

// ---------------------------------------------------------------------------
// Sony ADS
//
//   'SShd' u32 headerSize            (always 0x18)
//     +0x00 u32 codec                1 = 16-bit PCM, 0x10 = Sony ADPCM
//     +0x04 u32 sampleRate
//     +0x08 u32 channels
//     +0x0C u32 interleave           bytes of one channel per block
//     +0x10 u32 loopStart
//     +0x14 u32 loopEnd
//   'SSbd' u32 bodySize, then bodySize bytes
//
// Channels are block-interleaved rather than sample-interleaved: `interleave`
// bytes of the left channel, then as many of the right. Confirmed on the 35
// IGC cutscene streams -- blocks k and k+1 correlate at +0.30 while k and k+2
// only reach +0.12, which is what an L/R pair of the same time window does.
// ---------------------------------------------------------------------------

bool Audio::LoadADS(const uint8_t* d, size_t size, AudioClip& out) {
    if (size < 0x30 || std::memcmp(d, "SShd", 4) != 0) return false;

    const uint32_t codec = ru32(d + 0x08);
    const uint32_t rate = ru32(d + 0x0C);
    const uint32_t chans = ru32(d + 0x10);
    uint32_t interleave = ru32(d + 0x14);

    if (rate < 1000 || rate > 192000) return false;
    if (chans < 1 || chans > 8) return false;
    if (interleave == 0 || interleave % 16) interleave = 2048;

    // The body follows the header chunk, whose length the header itself gives.
    size_t bodyOff = 8 + ru32(d + 0x04);
    if (bodyOff + 8 > size || std::memcmp(d + bodyOff, "SSbd", 4) != 0) {
        bodyOff = 0;
        for (size_t i = 8; i + 8 < size && i < 0x400; ++i) {
            if (std::memcmp(d + i, "SSbd", 4) == 0) { bodyOff = i; break; }
        }
        if (!bodyOff) return false;
    }

    const uint32_t declared = ru32(d + bodyOff + 4);
    const uint8_t* body = d + bodyOff + 8;
    const size_t avail = size - (bodyOff + 8);
    const size_t bodyLen = std::min<size_t>(declared, avail);
    if (bodyLen < (size_t)interleave * chans) return false;

    out.sampleRate = (int)rate;
    out.channels = (int)chans;
    out.pcm.clear();

    if (codec == 0x01) {
        out.codec = "PCM16";
        const size_t stride = (size_t)interleave * chans;
        const size_t perCh = interleave / 2; // samples in one channel block
        const size_t blocks = bodyLen / stride;
        out.pcm.assign(blocks * perCh * chans, 0);
        for (size_t b = 0; b < blocks; ++b) {
            for (uint32_t c = 0; c < chans; ++c) {
                const uint8_t* src = body + b * stride + (size_t)c * interleave;
                int16_t* dst = out.pcm.data() + (b * perCh) * chans + c;
                for (size_t s = 0; s < perCh; ++s)
                    dst[s * chans] = (int16_t)((uint16_t)src[s * 2] |
                                               ((uint16_t)src[s * 2 + 1] << 8));
            }
        }
    } else {
        // 0x10, and the few other values the authoring tools emit, are all
        // Sony ADPCM. Each channel carries its own predictor across blocks.
        out.codec = "VAG (Sony ADPCM)";
        std::vector<std::vector<int16_t>> ch((size_t)chans);
        std::vector<Audio::VagState> st((size_t)chans);
        const size_t stride = (size_t)interleave * chans;
        for (size_t off = 0; off + stride <= bodyLen; off += stride)
            for (uint32_t c = 0; c < chans; ++c)
                Audio::DecodeVAG(body + off + (size_t)c * interleave, interleave,
                                 ch[c], st[c]);
        if (ch[0].empty()) return false;
        const size_t n = ch[0].size();
        out.pcm.assign(n * chans, 0);
        for (size_t i = 0; i < n; ++i)
            for (uint32_t c = 0; c < chans; ++c)
                out.pcm[i * chans + c] = i < ch[c].size() ? ch[c][i] : 0;
    }

    return out.Valid();
}

// ---------------------------------------------------------------------------
// RenderWare Audio stream (.RWS)
//
//   0x080D  file chunk, covering the whole file
//     0x080E  header, 2012 bytes on every retail track
//       +0x78 u32 padded data length   (== fileSize - 2048 for all 75 tracks)
//       +0x80 u32 real data length     (16-byte aligned, no trailing padding)
//       +0xC0 u32 channels             (1 throughout)
//       +0xCC u32 sample rate          (44094, or 32000 for MENU and SCN01)
//   audio data at a fixed offset of 2048
//
// The header also carries a plain-text parameter block naming the codec as
// "VAG (Sony ADPCM)" with a numchannels / samplerate / audioframesize schema,
// but that block is missing from some tracks, so the binary fields above are
// the ones read here. Every one of the 335 966 ADPCM blocks in APRTMENT.RWS
// has a valid shift and filter nibble, and the mean sample-to-sample step at
// the 2048-byte boundaries (55.5) matches the overall mean (58.6) -- there is
// no splice there, which is what rules out a stereo interleave.
// ---------------------------------------------------------------------------

bool Audio::LoadRWS(const uint8_t* d, size_t size, AudioClip& out) {
    Chunk root;
    if (!ReadChunk(d, size, 0, root) || root.type != 0x080D) return false;

    Chunk hdr;
    if (!ReadChunk(d, size, 12, hdr) || hdr.type != 0x080E || hdr.size < 0xD8)
        return false;

    const uint32_t chans = ru32(hdr.payload + 0xC0);
    const uint32_t rate = ru32(hdr.payload + 0xCC);
    const uint32_t padded = ru32(hdr.payload + 0x78);
    uint32_t real = ru32(hdr.payload + 0x80);

    if (rate < 1000 || rate > 192000) return false;
    if (chans < 1 || chans > 2) return false;

    const size_t dataStart = 2048;
    if (dataStart >= size) return false;
    if (real == 0 || real > size - dataStart) real = padded;
    const size_t len = std::min<size_t>(real, size - dataStart);
    if (len < 16) return false;

    out.sampleRate = (int)rate;
    out.channels = (int)chans;
    out.codec = "VAG (Sony ADPCM)";
    out.pcm.clear();

    if (chans == 1) {
        Audio::DecodeVAG(d + dataStart, len, out.pcm);
    } else {
        // No retail track uses this, but the header allows it and the frame
        // size sits right beside the channel count.
        uint32_t interleave = ru32(hdr.payload + 0xC4);
        if (interleave == 0 || interleave % 16) interleave = 2048;
        std::vector<std::vector<int16_t>> ch(2);
        Audio::VagState st[2];
        const size_t stride = (size_t)interleave * 2;
        for (size_t off = 0; off + stride <= len; off += stride) {
            Audio::DecodeVAG(d + dataStart + off, interleave, ch[0], st[0]);
            Audio::DecodeVAG(d + dataStart + off + interleave, interleave, ch[1],
                             st[1]);
        }
        const size_t n = ch[0].size();
        out.pcm.assign(n * 2, 0);
        for (size_t i = 0; i < n; ++i) {
            out.pcm[i * 2] = ch[0][i];
            out.pcm[i * 2 + 1] = i < ch[1].size() ? ch[1][i] : 0;
        }
    }

    return out.Valid();
}

// ---------------------------------------------------------------------------
// IGC cutscene stream
//
// A flat sequence of records, each [u16 tag][u16 payloadSize][payload]:
//
//   0xFF10  the file header; the source path sits at +0x10 of its payload,
//           e.g. "Movie_10/movie10.ads"
//   0x0000..0x00FF
//           32-byte camera and bone keyframes, one tag per animated node
//   0xA000  1024 bytes of the audio stream
//
// The audio is therefore *not* contiguous: it is cut into 1024-byte pieces and
// multiplexed with the animation. Concatenating every 0xA000 payload yields an
// ordinary ADS -- 'SShd' + 'SSbd' + body -- and on all 35 streams in IGC.ARC
// the records walk to exactly EOF and the reassembled body matches the length
// 'SSbd' declares, to the byte.
//
// Reading the body without removing the 4-byte record headers looks almost
// right (the pieces are 1024 bytes, a whole stereo frame) but splices four
// bytes of garbage in every 1028, which drops the lag-1 autocorrelation of the
// result from 0.997 to 0.14. Each archive entry also declares
// uncompressedSize = 0, meaning the payload is stored raw rather than deflated.
// ---------------------------------------------------------------------------

bool Audio::LoadIGCStream(const uint8_t* d, size_t size, AudioClip& out) {
    if (size < 0x40) return false;

    const std::string embedded = cstr(d + 0x14, 64);

    std::vector<uint8_t> ads;
    for (size_t o = 0; o + 4 <= size;) {
        const uint16_t tag = (uint16_t)((uint16_t)d[o] | ((uint16_t)d[o + 1] << 8));
        const uint16_t len =
            (uint16_t)((uint16_t)d[o + 2] | ((uint16_t)d[o + 3] << 8));
        if (o + 4 + len > size) break;
        if (tag == 0xA000) ads.insert(ads.end(), d + o + 4, d + o + 4 + len);
        o += 4 + len;
    }

    if (ads.size() >= 0x30 && Audio::LoadADS(ads.data(), ads.size(), out)) {
        if (!embedded.empty()) out.source = embedded;
        return true;
    }

    // Not an IGC after all, or a variant with a different tag. Fall back to a
    // bounded search for a plain ADS block.
    const size_t limit = std::min<size_t>(size - 8, 0x20000);
    for (size_t i = 0; i + 8 < limit; i += 4) {
        if (std::memcmp(d + i, "SShd", 4) != 0) continue;
        if (!Audio::LoadADS(d + i, size - i, out)) continue;
        if (!embedded.empty()) out.source = embedded;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Level sound bank (rwaID_WAVEDICT)
//
//   0x0809  dictionary
//     0x080A  84 bytes; the bank's name at +0x34 ("AudioMotelGenRoom", ...)
//     0x080C  data
//       u32 waveCount
//       0x0802  one per sample
//         0x0803  header
//           +0x04 u32 sample rate
//           +0x0C u32 data length     (always equal to the 0x0804 size)
//           +0x20 GUID of the codec   (a single value across the archive:
//                                      9897ead9 bcbb7b44 96b26547 59102e16,
//                                      the same GUID the .RWS parameter block
//                                      spells out as "VAG (Sony ADPCM)")
//           +0x70 char name[]         "door_jammed", "footstep_carpet1", ...
//         0x0804  the ADPCM data
//
// All 255 dictionaries in the retail archive walk to exactly their declared
// end, giving 2980 samples whose every ADPCM block header is valid. Sample
// rates run from 6000 to 32000 Hz, most of them 22050.
// ---------------------------------------------------------------------------

void Audio::ParseWaveDictionary(const uint8_t* d, size_t size,
                                std::vector<AudioClip>& out) {
    Chunk dict;
    if (!ReadChunk(d, size, 0, dict) || dict.type != 0x0809) return;

    std::string bankName;
    const uint8_t* block = nullptr;
    size_t blockLen = 0;

    for (size_t o = 12; o + 12 <= 12 + (size_t)dict.size;) {
        Chunk c;
        if (!ReadChunk(d, size, o, c)) break;
        if (c.type == 0x080A && c.size >= 0x44)
            bankName = cstr(c.payload + 0x34, c.size - 0x34);
        else if (c.type == 0x080C) { block = c.payload; blockLen = c.size; }
        o += 12 + c.size;
    }
    if (!block || blockLen < 4) return;

    for (size_t o = 4; o + 12 <= blockLen;) {
        Chunk bank;
        if (!ReadChunk(block, blockLen, o, bank) || bank.type != 0x0802) break;

        AudioClip clip;
        clip.codec = "VAG (Sony ADPCM)";
        clip.source = bankName;
        bool haveHeader = false;

        for (size_t p = 0; p + 12 <= (size_t)bank.size;) {
            Chunk sub;
            if (!ReadChunk(bank.payload, bank.size, p, sub)) break;
            if (sub.type == 0x0803 && sub.size >= 0x74) {
                clip.sampleRate = (int)ru32(sub.payload + 0x04);
                clip.name = cstr(sub.payload + 0x70, sub.size - 0x70);
                haveHeader = clip.sampleRate >= 1000 && clip.sampleRate <= 192000;
            } else if (sub.type == 0x0804 && haveHeader) {
                Audio::DecodeVAG(sub.payload, sub.size, clip.pcm);
                if (clip.Valid()) {
                    if (clip.name.empty())
                        clip.name = "wave_" + std::to_string(out.size());
                    out.push_back(std::move(clip));
                }
                break;
            }
            p += 12 + sub.size;
        }
        o += 12 + bank.size;
    }
}

// ---------------------------------------------------------------------------
// Sniffing and file loading
// ---------------------------------------------------------------------------

bool Audio::LoadBuffer(const uint8_t* d, size_t size, AudioClip& out) {
    if (size < 16) return false;

    if (std::memcmp(d, "SShd", 4) == 0) return Audio::LoadADS(d, size, out);
    if (ru32(d) == 0x080D) return Audio::LoadRWS(d, size, out);
    if (d[0] == 0x10 && d[1] == 0xFF) return Audio::LoadIGCStream(d, size, out);

    // Plain VAGp: a 0x30-byte big-endian header, then the ADPCM.
    if (std::memcmp(d, "VAGp", 4) == 0 && size > 0x40) {
        const uint32_t rate = ((uint32_t)d[0x10] << 24) | ((uint32_t)d[0x11] << 16) |
                              ((uint32_t)d[0x12] << 8) | (uint32_t)d[0x13];
        out.sampleRate = (rate >= 1000 && rate <= 192000) ? (int)rate : 44100;
        out.channels = 1;
        out.codec = "VAG (Sony ADPCM)";
        out.pcm.clear();
        Audio::DecodeVAG(d + 0x30, size - 0x30, out.pcm);
        return out.Valid();
    }

    // Basic WAV (PCM 16-bit) exported by this tool
    if (std::memcmp(d, "RIFF", 4) == 0 && size >= 44 && std::memcmp(d + 8, "WAVEfmt ", 8) == 0) {
        out.channels = d[22] | (d[23] << 8);
        out.sampleRate = d[24] | (d[25] << 8) | (d[26] << 16) | (d[27] << 24);
        out.codec = "WAV (PCM16)";
        uint32_t dataBytes = d[40] | (d[41] << 8) | (d[42] << 16) | (d[43] << 24);
        if (44 + dataBytes <= size) {
            out.pcm.resize(dataBytes / 2);
            std::memcpy(out.pcm.data(), d + 44, dataBytes);
        }
        return out.Valid();
    }

    // Last resort for headerless dumps: an ADS block sitting further in than
    // the sniffs above expect.
    return Audio::LoadIGCStream(d, size, out);
}

bool Audio::LoadFile(const std::string& path, AudioClip& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size <= 0) return false;
    f.seekg(0);

    std::vector<uint8_t> data((size_t)size);
    if (!f.read((char*)data.data(), size)) return false;

    const size_t slash = path.find_last_of("/\\");
    out.name = slash == std::string::npos ? path : path.substr(slash + 1);
    out.source = path;
    return Audio::LoadBuffer(data.data(), data.size(), out);
}

// ---------------------------------------------------------------------------
// WAV export
// ---------------------------------------------------------------------------

bool Audio::WriteWav(const std::string& path, const AudioClip& clip) {
    if (!clip.Valid()) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    const uint32_t dataBytes = (uint32_t)(clip.pcm.size() * 2);
    auto u32 = [&](uint32_t v) {
        const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                              (uint8_t)(v >> 24)};
        f.write((const char*)b, 4);
    };
    auto u16 = [&](uint16_t v) {
        const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
        f.write((const char*)b, 2);
    };

    f.write("RIFF", 4);     u32(36 + dataBytes);
    f.write("WAVEfmt ", 8); u32(16);
    u16(1);                 u16((uint16_t)clip.channels);
    u32((uint32_t)clip.sampleRate);
    u32((uint32_t)(clip.sampleRate * clip.channels * 2));
    u16((uint16_t)(clip.channels * 2));
    u16(16);
    f.write("data", 4);     u32(dataBytes);
    f.write((const char*)clip.pcm.data(), dataBytes);
    return (bool)f;
}
