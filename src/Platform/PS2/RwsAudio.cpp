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

bool RwaVirtualVoice::Play(const AudioClip& clip, int targetRate, int targetChannels) {
    Stop();
    if (!clip.Valid()) return false;

    // Convert the whole clip to the device format once, here, and let the
    // callback do nothing but copy.
    //
    // Resampling inside the callback is what made the music break up. The old
    // code only refilled the SDL_AudioStream once it had run dry, and then
    // pushed 32768 frames at a time -- three quarters of a second of audio
    // resampled in a single callback, every three quarters of a second, while
    // every other callback did almost nothing. One spike over the buffer
    // deadline is one dropout, and it repeated for the whole track.
    //
    // It only ever showed on music: the cutscenes are already 48 kHz stereo,
    // so SDL_AudioStream copies them without resampling at all, and the level
    // sounds are short enough that their single spike falls at the start.
    m_clip = clip;
    m_channels = targetChannels;
    m_currentFrame = 0;

    if (clip.sampleRate == targetRate && clip.channels == targetChannels) {
        m_pcm = clip.pcm;                       // already in device format
    } else {
        SDL_AudioStream* conv = SDL_NewAudioStream(
            AUDIO_S16SYS, (Uint8)clip.channels, clip.sampleRate,
            AUDIO_S16SYS, (Uint8)targetChannels, targetRate);
        if (!conv) {
            std::cerr << "[audio] SDL_NewAudioStream failed: " << SDL_GetError() << "\n";
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

    // We no longer clear the decoded source (m_clip.pcm). The UI uses this copy
    // to export .wav files, so it must retain the original PCM data.


    m_playing = true;
    m_paused = false;
    return true;
}

void RwaVirtualVoice::Stop() {
    m_pcm.clear();
    m_pcm.shrink_to_fit();
    m_sourceFrames = 0;
    m_currentFrame = 0;
    m_playing = false;
    m_paused = false;
    m_clip = AudioClip();
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

namespace Audio {

CAudioRelay& CAudioRelay::GetInstance() {
    static CAudioRelay instance;
    return instance;
}

static void AudioCallbackStub(void* userdata, Uint8* stream, int len) {
    auto* relay = static_cast<CAudioRelay*>(userdata);
    relay->ProcessAudio(stream, len);
}

bool CAudioRelay::Init() {
    if (m_device) return true;

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq     = m_deviceRate;
    want.format   = AUDIO_S16SYS;
    want.channels = (Uint8)m_deviceChans;
    want.samples  = 8192; // Increased buffer to prevent stuttering
    want.callback = AudioCallbackStub;
    want.userdata = this;

    // Take whatever rate the hardware actually runs at and resample to that
    // ourselves, instead of asking SDL for a fixed 48 kHz and letting it
    // convert. Forcing the rate leaves a second, hidden conversion in the
    // output path -- and a non-integer ratio there is what makes a long track
    // warble while a 48 kHz cutscene, which needs no conversion at all, plays
    // clean.
    SDL_AudioSpec have;
    SDL_zero(have);
    m_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have,
                                   SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!m_device) {
        std::cerr << "[audio] SDL_OpenAudioDevice failed: " << SDL_GetError() << "\n";
        return false;
    }
    if (have.freq > 0) m_deviceRate = have.freq;
    std::cout << "[audio] device: " << m_deviceRate << " Hz, "
              << (int)have.channels << " ch, " << have.samples << "-frame buffer"
              << (have.freq != want.freq ? "  (rate chosen by the driver)" : "")
              << "\n";
    SDL_PauseAudioDevice(m_device, 0);
    return true;
}

void CAudioRelay::Shutdown() {
    if (m_device) {
        SDL_CloseAudioDevice(m_device);
        m_device = 0;
    }
}

CAudioRelay::~CAudioRelay() {
    Shutdown();
}

void CAudioRelay::PlayAudioClip(const AudioClip& clip) {
    Init();
    if (!m_device) return;

    SDL_LockAudioDevice(m_device);
    m_voice.Play(clip, m_deviceRate, m_deviceChans);
    SDL_UnlockAudioDevice(m_device);
}

void CAudioRelay::ToggleAudioPlayback() {
    if (!m_device || !m_voice.IsValid()) return;
    SDL_LockAudioDevice(m_device);
    if (m_voice.IsPlaying()) {
        m_voice.Pause(true);
    } else {
        if (!m_voice.IsValid()) {
            // Already finished, restart
            m_voice.SetProgress(0.0f);
        }
        m_voice.Pause(false);
    }
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

float CAudioRelay::GetAudioProgress() const {
    return m_voice.GetProgress();
}

bool CAudioRelay::IsAudioPlaying() const {
    return m_voice.IsPlaying();
}

void CAudioRelay::ProcessAudio(Uint8* stream, int len) {
    int16_t* out = (int16_t*)stream;
    const int frames = len / (int)(sizeof(int16_t) * m_deviceChans);

    if (!m_voice.IsPlaying()) {
        std::memset(stream, 0, (size_t)len);
        return;
    }

    int produced = m_voice.Process(out, frames, m_loop);
    
    // Apply volume
    if (produced > 0 && m_volume != 1.0f) {
        int samples = produced * m_deviceChans;
        for (int i = 0; i < samples; i++) {
            int32_t v = (int32_t)((float)out[i] * m_volume);
            out[i] = (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
        }
    }

    // Fill the rest with silence if EOF
    if (produced < frames) {
        std::memset(out + (produced * m_deviceChans), 0, (size_t)(frames - produced) * sizeof(int16_t) * m_deviceChans);
    }
}

} // namespace Audio
} // namespace ClimaxEngine
