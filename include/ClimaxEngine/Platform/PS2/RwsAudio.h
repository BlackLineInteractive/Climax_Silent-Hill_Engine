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

    // Heavy path — resample / convert to device format.
    // Call this BEFORE taking SDL_LockAudioDevice; it may block for several
    // milliseconds on a long music track and must not run inside the callback
    // lock (doing so starves the device and causes the very dropout it aims to
    // prevent).
    bool Prepare(const AudioClip& clip, int targetRate, int targetChannels);

    // Light path — atomically replace the playing clip with a pre-prepared
    // voice.  Call this INSIDE SDL_LockAudioDevice; it only swaps buffers.
    void SwapFrom(RwaVirtualVoice&& other);

    // Convenience: Prepare + SwapFrom in one call for callers that can tolerate
    // a brief stall (e.g. short SFX on the main thread, not music).
    bool Play(const AudioClip& clip, int targetRate, int targetChannels);

    void Stop();
    void Pause(bool pause);

    // Returns how many frames were read. Fills `out` with interleaved S16 PCM.
    int Process(int16_t* out, int frames, bool loop);

    bool IsPlaying() const { return m_playing && !m_paused; }
    bool IsValid()   const { return m_playing; }
    void SetProgress(float progress);
    float GetProgress() const;
    const AudioClip& GetClip() const { return m_clip; }

private:
    AudioClip            m_clip;            // metadata + original pcm for WAV export
    std::vector<int16_t> m_pcm;            // clip already in device format
    int    m_channels    = 2;
    bool   m_playing     = false;
    bool   m_paused      = false;
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

    // ── SFX / one-shot ───────────────────────────────────────────────────────
    // Replaces whatever is playing and starts the new clip immediately.
    // For short clips (< ~0.5 s) the internal conversion is fast enough that
    // the brief lock is acceptable.  For long clips, call PrepareClip() first.
    void PlayAudioClip(const AudioClip& clip);

    // Prepare a clip for playback on ANY thread without holding the device
    // lock.  Returns an opaque cookie; pass it to CommitClip() to start
    // playing.  This is the path to use for music tracks.
    RWS::RwsAudio::RwaVirtualVoice PrepareClip(const AudioClip& clip);
    void CommitClip(RWS::RwsAudio::RwaVirtualVoice&& prepared);

    void ToggleAudioPlayback();
    void StopAudio();
    void SetAudioProgress(float progress);

    const AudioClip& CurrentAudioClip() const;
    float GetAudioProgress() const;
    bool  IsAudioPlaying()   const;

    // ── Background music ─────────────────────────────────────────────────────
    // Music plays in a second voice that loops automatically and is mixed with
    // the SFX voice.  Loading is done off the lock; only the swap is locked.
    void PlayMusic(const AudioClip& clip, float volume = 0.6f);
    void StopMusic();
    bool IsMusicPlaying() const;

    // Called by SDL audio callback — do not call directly.
    void ProcessAudio(Uint8* stream, int len);

    float GetVolume() const        { return m_volume; }
    void  SetVolume(float vol)     { m_volume = vol; }
    bool  GetLoop() const          { return m_loop; }
    void  SetLoop(bool loop)       { m_loop = loop; }
    float GetMusicVolume() const   { return m_musicVolume; }
    void  SetMusicVolume(float v)  { m_musicVolume = v; }

private:
    CAudioRelay() = default;
    ~CAudioRelay();

    SDL_AudioDeviceID m_device      = 0;
    int   m_deviceRate  = 48000;
    int   m_deviceChans = 2;
    float m_volume      = 1.0f;
    float m_musicVolume = 0.6f;
    bool  m_loop        = false;

    ClimaxEngine::RWS::RwsAudio::RwaVirtualVoice m_voice;   // SFX / one-shot
    ClimaxEngine::RWS::RwsAudio::RwaVirtualVoice m_music;   // looping music
};

} // namespace Audio
} // namespace ClimaxEngine
