#pragma once

#include "ClimaxEngine/Platform/PS2/AudioParser.h"
#include <SDL2/SDL.h>
#include <memory>
#include <vector>

namespace ClimaxEngine {
namespace RWS {
namespace RwsAudio {

class RwaVirtualVoice {
public:
    RwaVirtualVoice();
    ~RwaVirtualVoice();

    bool Play(const AudioClip& clip, int targetRate, int targetChannels);
    void Stop();
    void Pause(bool pause);
    
    // Returns how many frames were read. Fills `out` with interleaved S16 PCM.
    int Process(int16_t* out, int frames, bool loop);

    bool IsPlaying() const { return m_playing && !m_paused; }
    bool IsValid() const { return m_playing; }
    void SetProgress(float progress);
    float GetProgress() const;
    const AudioClip& GetClip() const { return m_clip; }

private:
    AudioClip m_clip;
    SDL_AudioStream* m_stream = nullptr;
    bool m_playing = false;
    bool m_paused = false;
    size_t m_sourceFrames = 0;
    size_t m_currentFrame = 0;
};

} // namespace RwsAudio
} // namespace RWS

namespace Audio {

class CAudioRelay {
public:
    static CAudioRelay& GetInstance();

    bool Init();
    void Shutdown();

    // Replaces the old main.cpp globals
    void PlayAudioClip(const AudioClip& clip);
    void ToggleAudioPlayback();
    void StopAudio();
    void SetAudioProgress(float progress);
    
    const AudioClip& CurrentAudioClip() const;
    float GetAudioProgress() const;
    bool IsAudioPlaying() const;

    // Called by SDL audio callback
    void ProcessAudio(Uint8* stream, int len);

    float GetVolume() const { return m_volume; }
    void SetVolume(float vol) { m_volume = vol; }
    bool GetLoop() const { return m_loop; }
    void SetLoop(bool loop) { m_loop = loop; }

private:
    CAudioRelay() = default;
    ~CAudioRelay();

    SDL_AudioDeviceID m_device = 0;
    int m_deviceRate = 48000;
    int m_deviceChans = 2;
    float m_volume = 1.0f;
    bool m_loop = false;

    ClimaxEngine::RWS::RwsAudio::RwaVirtualVoice m_voice;
};

} // namespace Audio
} // namespace ClimaxEngine
