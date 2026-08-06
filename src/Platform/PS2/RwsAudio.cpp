#include "ClimaxEngine/Platform/PS2/RwsAudio.h"
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

    m_clip = clip;
    m_sourceFrames = clip.pcm.size() / clip.channels;
    m_currentFrame = 0;
    
    m_stream = SDL_NewAudioStream(
        AUDIO_S16SYS, clip.channels, clip.sampleRate,
        AUDIO_S16SYS, targetChannels, targetRate
    );
    
    if (!m_stream) {
        std::cerr << "[audio] SDL_NewAudioStream failed: " << SDL_GetError() << "\n";
        return false;
    }
    
    m_playing = true;
    m_paused = false;
    return true;
}

void RwaVirtualVoice::Stop() {
    if (m_stream) {
        SDL_FreeAudioStream(m_stream);
        m_stream = nullptr;
    }
    m_playing = false;
    m_paused = false;
    m_clip = AudioClip();
}

void RwaVirtualVoice::Pause(bool pause) {
    m_paused = pause;
}

int RwaVirtualVoice::Process(int16_t* out, int frames, bool loop) {
    if (!m_playing || m_paused || !m_stream) return 0;
    
    // Hardcoded to device format in CAudioRelay (S16 stereo)
    const int targetChans = 2;
    const int targetFrameSize = targetChans * sizeof(int16_t);
    const int sourceFrameSize = m_clip.channels * sizeof(int16_t);
    
    int framesNeeded = frames;
    int framesProduced = 0;
    
    while (framesNeeded > 0) {
        // How many bytes are ready to be read?
        int availBytes = SDL_AudioStreamAvailable(m_stream);
        int availFrames = availBytes / targetFrameSize;
        
        if (availFrames == 0) {
            // Need to push more source data into the stream
            if (m_currentFrame >= m_sourceFrames) {
                if (loop) {
                    m_currentFrame = 0;
                } else {
                    // EOF
                    SDL_AudioStreamFlush(m_stream);
                    if (SDL_AudioStreamAvailable(m_stream) == 0) {
                        m_playing = false;
                        break;
                    }
                }
            }
            
            // Push up to 32768 frames at a time to prevent stuttering
            if (m_currentFrame < m_sourceFrames) {
                size_t pushFrames = std::min<size_t>(32768, m_sourceFrames - m_currentFrame);
                const int16_t* srcPtr = m_clip.pcm.data() + m_currentFrame * m_clip.channels;
                if (SDL_AudioStreamPut(m_stream, srcPtr, (int)pushFrames * sourceFrameSize) == -1) {
                    std::cerr << "[audio] SDL_AudioStreamPut failed: " << SDL_GetError() << "\n";
                    break;
                }
                m_currentFrame += pushFrames;
            }
            continue; // Now we should have something available
        }
        
        int getFrames = std::min(framesNeeded, availFrames);
        int getBytes = getFrames * targetFrameSize;
        int gotBytes = SDL_AudioStreamGet(m_stream, out + (framesProduced * targetChans), getBytes);
        
        if (gotBytes > 0) {
            int gotFrames = gotBytes / targetFrameSize;
            framesProduced += gotFrames;
            framesNeeded -= gotFrames;
        } else if (gotBytes == -1) {
            std::cerr << "[audio] SDL_AudioStreamGet failed: " << SDL_GetError() << "\n";
            break;
        }
    }
    
    return framesProduced;
}

void RwaVirtualVoice::SetProgress(float progress) {
    if (!m_stream || !m_playing) return;
    progress = std::max(0.0f, std::min(1.0f, progress));
    m_currentFrame = (size_t)(progress * (float)m_sourceFrames);
    SDL_AudioStreamClear(m_stream);
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

    m_device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (!m_device) {
        std::cerr << "[audio] SDL_OpenAudioDevice failed: " << SDL_GetError() << "\n";
        return false;
    }
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
