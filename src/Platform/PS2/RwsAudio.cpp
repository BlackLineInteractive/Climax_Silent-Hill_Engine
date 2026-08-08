#include "ClimaxEngine/Platform/PS2/RwsAudio.h"
#include <algorithm>
#include <cstring>
#include <iostream>

namespace ClimaxEngine {
namespace RWS {
namespace RwsAudio {

RwaVirtualVoice::RwaVirtualVoice() {}

RwaVirtualVoice::~RwaVirtualVoice() {
    Stop();
}

// ---------------------------------------------------------------------------
// Prepare — heavy path, NO lock held
// ---------------------------------------------------------------------------

bool RwaVirtualVoice::Prepare(const AudioClip& clip, int targetRate,
                               int targetChannels) {
    if (!clip.Valid()) return false;

    // Store the original clip (metadata + pcm) for WAV export even after the
    // resampled buffer has been swapped in.
    m_clip     = clip;
    m_channels = targetChannels;

    if (clip.sampleRate == targetRate && clip.channels == targetChannels) {
        m_pcm = clip.pcm;
    } else {
        // All the heavy resampling work happens here, outside any audio lock.
        SDL_AudioStream* conv = SDL_NewAudioStream(
            AUDIO_S16SYS, (Uint8)clip.channels,   clip.sampleRate,
            AUDIO_S16SYS, (Uint8)targetChannels,  targetRate);
        if (!conv) {
            std::cerr << "[audio] SDL_NewAudioStream: " << SDL_GetError() << "\n";
            return false;
        }
        const int srcBytes = (int)(clip.pcm.size() * sizeof(int16_t));
        if (SDL_AudioStreamPut(conv, clip.pcm.data(), srcBytes) == -1 ||
            SDL_AudioStreamFlush(conv) == -1) {
            std::cerr << "[audio] conversion failed: " << SDL_GetError() << "\n";
            SDL_FreeAudioStream(conv);
            return false;
        }
        const int outBytes = SDL_AudioStreamAvailable(conv);
        m_pcm.resize((size_t)outBytes / sizeof(int16_t));
        if (outBytes > 0)
            SDL_AudioStreamGet(conv, m_pcm.data(), outBytes);
        SDL_FreeAudioStream(conv);
    }

    m_sourceFrames = m_channels ? m_pcm.size() / (size_t)m_channels : 0;
    if (m_sourceFrames == 0) return false;

    m_clip.durationSeconds = m_clip.Seconds();
    m_currentFrame = 0;
    m_playing = true;
    m_paused  = false;
    return true;
}

// ---------------------------------------------------------------------------
// SwapFrom — light path, MUST be called under SDL_LockAudioDevice
// ---------------------------------------------------------------------------

void RwaVirtualVoice::SwapFrom(RwaVirtualVoice&& other) {
    m_clip         = std::move(other.m_clip);
    m_pcm          = std::move(other.m_pcm);
    m_channels     = other.m_channels;
    m_sourceFrames = other.m_sourceFrames;
    m_currentFrame = 0;
    m_playing      = other.m_playing;
    m_paused       = other.m_paused;

    // Leave `other` in a stopped state so its destructor is a no-op.
    other.m_playing = false;
    other.m_sourceFrames = 0;
}

// ---------------------------------------------------------------------------
// Play — convenience wrapper (Prepare then SwapFrom yourself)
// ---------------------------------------------------------------------------

bool RwaVirtualVoice::Play(const AudioClip& clip, int targetRate,
                            int targetChannels) {
    Stop();
    return Prepare(clip, targetRate, targetChannels);
}

void RwaVirtualVoice::Stop() {
    m_pcm.clear();
    m_pcm.shrink_to_fit();
    m_sourceFrames = 0;
    m_currentFrame = 0;
    m_playing      = false;
    m_paused       = false;
    m_clip         = AudioClip();
}

void RwaVirtualVoice::Pause(bool pause) {
    m_paused = pause;
}

int RwaVirtualVoice::Process(int16_t* out, int frames, bool loop) {
    if (!m_playing || m_paused || m_sourceFrames == 0) return 0;

    const int chans = m_channels;
    int produced = 0;
    while (produced < frames) {
        if (m_currentFrame >= m_sourceFrames) {
            if (!loop) { m_playing = false; break; }
            m_currentFrame = 0;
        }
        const size_t avail = m_sourceFrames - m_currentFrame;
        const int take = (int)std::min<size_t>(avail, (size_t)(frames - produced));
        std::memcpy(out + (size_t)produced * chans,
                    m_pcm.data() + m_currentFrame * (size_t)chans,
                    (size_t)take * chans * sizeof(int16_t));
        m_currentFrame += (size_t)take;
        produced += take;
    }
    return produced;
}

void RwaVirtualVoice::SetProgress(float progress) {
    if (!m_playing || m_sourceFrames == 0) return;
    progress = std::max(0.0f, std::min(1.0f, progress));
    m_currentFrame = (size_t)(progress * (float)m_sourceFrames);
    if (m_currentFrame >= m_sourceFrames) m_currentFrame = m_sourceFrames - 1;
}

float RwaVirtualVoice::GetProgress() const {
    if (!m_playing || m_sourceFrames == 0) return 0.0f;
    return (float)m_currentFrame / (float)m_sourceFrames;
}

} // namespace RwsAudio
} // namespace RWS

// ---------------------------------------------------------------------------
// CAudioRelay
// ---------------------------------------------------------------------------

namespace Audio {

CAudioRelay& CAudioRelay::GetInstance() {
    static CAudioRelay instance;
    return instance;
}

static void AudioCallbackStub(void* userdata, Uint8* stream, int len) {
    static_cast<CAudioRelay*>(userdata)->ProcessAudio(stream, len);
}

bool CAudioRelay::Init() {
    if (m_device) return true;

    SDL_AudioSpec want, have;
    SDL_zero(want); SDL_zero(have);
    want.freq     = m_deviceRate;
    want.format   = AUDIO_S16SYS;
    want.channels = (Uint8)m_deviceChans;
    want.samples  = 4096;           // ~85 ms at 48 kHz — small enough to feel responsive
    want.callback = AudioCallbackStub;
    want.userdata = this;

    m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have,
                                   SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!m_device) {
        std::cerr << "[audio] SDL_OpenAudioDevice: " << SDL_GetError() << "\n";
        return false;
    }
    if (have.freq > 0) m_deviceRate = have.freq;
    std::cout << "[audio] device: " << m_deviceRate << " Hz, "
              << (int)have.channels << " ch, " << have.samples << "-frame buffer"
              << (have.freq != want.freq ? "  (driver rate)" : "") << "\n";
    SDL_PauseAudioDevice(m_device, 0);
    return true;
}

void CAudioRelay::Shutdown() {
    if (m_device) {
        SDL_CloseAudioDevice(m_device);
        m_device = 0;
    }
}

CAudioRelay::~CAudioRelay() { Shutdown(); }

// ── SFX / one-shot ─────────────────────────────────────────────────────────

void CAudioRelay::PlayAudioClip(const AudioClip& clip) {
    Init();
    if (!m_device) return;

    // Prepare OUTSIDE the lock — this is the stuttering fix.
    // For short SFX the resampling is fast; for long clips callers should use
    // PrepareClip() + CommitClip() on a worker thread.
    RWS::RwsAudio::RwaVirtualVoice tmp;
    if (!tmp.Prepare(clip, m_deviceRate, m_deviceChans)) return;

    SDL_LockAudioDevice(m_device);
    m_voice.SwapFrom(std::move(tmp));
    SDL_UnlockAudioDevice(m_device);
}

RWS::RwsAudio::RwaVirtualVoice CAudioRelay::PrepareClip(const AudioClip& clip) {
    Init();
    RWS::RwsAudio::RwaVirtualVoice tmp;
    tmp.Prepare(clip, m_deviceRate, m_deviceChans);
    return tmp;
}

void CAudioRelay::CommitClip(RWS::RwsAudio::RwaVirtualVoice&& prepared) {
    if (!m_device) return;
    SDL_LockAudioDevice(m_device);
    m_voice.SwapFrom(std::move(prepared));
    SDL_UnlockAudioDevice(m_device);
}

void CAudioRelay::ToggleAudioPlayback() {
    if (!m_device || !m_voice.IsValid()) return;
    SDL_LockAudioDevice(m_device);
    if (m_voice.IsPlaying())
        m_voice.Pause(true);
    else
        m_voice.Pause(false);
    SDL_UnlockAudioDevice(m_device);
}

void CAudioRelay::StopAudio() {
    if (m_device) SDL_LockAudioDevice(m_device);
    m_voice.Stop();
    if (m_device) SDL_UnlockAudioDevice(m_device);
}

void CAudioRelay::SetAudioProgress(float progress) {
    if (m_device) SDL_LockAudioDevice(m_device);
    m_voice.SetProgress(progress);
    if (m_device) SDL_UnlockAudioDevice(m_device);
}

const AudioClip& CAudioRelay::CurrentAudioClip() const {
    return m_voice.GetClip();
}
float CAudioRelay::GetAudioProgress() const { return m_voice.GetProgress(); }
bool  CAudioRelay::IsAudioPlaying()   const { return m_voice.IsPlaying(); }

// ── Background music ────────────────────────────────────────────────────────

void CAudioRelay::PlayMusic(const AudioClip& clip, float volume) {
    Init();
    if (!m_device) return;

    m_musicVolume = volume;

    // All the heavy lifting before taking the lock.
    RWS::RwsAudio::RwaVirtualVoice tmp;
    if (!tmp.Prepare(clip, m_deviceRate, m_deviceChans)) {
        std::cerr << "[audio] PlayMusic: prepare failed\n";
        return;
    }

    SDL_LockAudioDevice(m_device);
    m_music.SwapFrom(std::move(tmp));
    SDL_UnlockAudioDevice(m_device);

    std::cout << "[audio] music: " << clip.name << " ("
              << clip.sampleRate << " Hz, " << clip.Seconds() << " s)\n";
}

void CAudioRelay::StopMusic() {
    if (!m_device) return;
    SDL_LockAudioDevice(m_device);
    m_music.Stop();
    SDL_UnlockAudioDevice(m_device);
}

bool CAudioRelay::IsMusicPlaying() const {
    return m_music.IsPlaying();
}

// ── SDL callback ────────────────────────────────────────────────────────────

void CAudioRelay::ProcessAudio(Uint8* stream, int len) {
    int16_t* out    = (int16_t*)stream;
    const int frames = len / (int)(sizeof(int16_t) * m_deviceChans);

    // Clear to silence first.
    std::memset(stream, 0, (size_t)len);

    // Music voice — looping, lower volume, mixed additively.
    if (m_music.IsPlaying()) {
        // Temporary buffer for music so we can scale it before mixing.
        static thread_local std::vector<int16_t> musicBuf;
        const int musicSamples = frames * m_deviceChans;
        musicBuf.assign((size_t)musicSamples, 0);

        int produced = m_music.Process(musicBuf.data(), frames, /*loop=*/true);
        if (produced > 0) {
            const float mv = m_musicVolume;
            for (int i = 0; i < produced * m_deviceChans; ++i) {
                int32_t v = (int32_t)out[i] + (int32_t)((float)musicBuf[i] * mv);
                out[i] = (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
            }
        }
    }

    // SFX / one-shot voice — full volume, mixed additively.
    if (m_voice.IsPlaying()) {
        static thread_local std::vector<int16_t> sfxBuf;
        const int sfxSamples = frames * m_deviceChans;
        sfxBuf.assign((size_t)sfxSamples, 0);

        int produced = m_voice.Process(sfxBuf.data(), frames, m_loop);
        if (produced > 0) {
            const float sv = m_volume;
            for (int i = 0; i < produced * m_deviceChans; ++i) {
                int32_t v = (int32_t)out[i] + (int32_t)((float)sfxBuf[i] * sv);
                out[i] = (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
            }
        }
    }
}

} // namespace Audio
} // namespace ClimaxEngine
