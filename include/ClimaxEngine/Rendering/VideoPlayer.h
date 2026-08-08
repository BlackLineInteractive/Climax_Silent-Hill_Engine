#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// Plays one of the converted boot/menu movies (see tools/convert_movies.py)
// into a GL texture.
//
// Platform code, not core and not game: it links FFmpeg and OpenGL, so it lives
// only in climax-play (and could live in the toolkit too, but nothing there
// asks for it yet). Game/FrontEnd.h knows a video is playing only through
// MenuInput::mediaEnded, which the caller sets from Finished() here -- the
// front-end state machine has no idea what a codec is.
//
// Built only when libavformat/libavcodec/libswscale are found; CLIMAX_HAVE_FFMPEG
// gates every declaration here so a build without them simply omits the class
// rather than failing.
// ─────────────────────────────────────────────────────────────────────────────
#ifdef CLIMAX_HAVE_FFMPEG

#include <GL/glew.h>

#include <cstdint>
#include <string>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

class VideoPlayer {
public:
    VideoPlayer() = default;
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer &) = delete;
    VideoPlayer &operator=(const VideoPlayer &) = delete;

    // Opens `path` and seeks to the start. False on any failure -- missing
    // file, no video stream, codec climax-play was not built with. The caller
    // is expected to fall back to a static image or a plain colour.
    bool Open(const std::string &path);
    void Close();

    // Advances playback by `dt` seconds and uploads at most one new frame.
    // Frames whose presentation time has already passed when this is called
    // are decoded and discarded rather than shown, so a slow frame does not
    // leave the picture stuck in the past.
    void Update(float dt);

    // True once decoding has reached the end of the file and the last frame's
    // duration has elapsed. Does not reset on its own -- call Open() again (or
    // Restart()) to loop.
    bool Finished() const { return m_finished; }
    void Restart();

    GLuint Texture() const { return m_tex; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }
    bool IsOpen() const { return m_fmt != nullptr; }

private:
    bool DecodeNextFrame();   // fills m_frame with the next video frame
    void UploadFrame();       // converts and pushes it to m_tex

    std::string m_path;
    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    SwsContext *m_sws = nullptr;
    AVFrame *m_frame = nullptr;
    AVFrame *m_rgbFrame = nullptr;
    AVPacket *m_packet = nullptr;
    uint8_t *m_rgbBuf = nullptr;

    int m_streamIndex = -1;
    int m_width = 0, m_height = 0;
    GLuint m_tex = 0;

    double m_timeBase = 0.0;      // stream time_base, in seconds per tick
    double m_playbackTime = 0.0;  // seconds since Open()/Restart()
    double m_pendingPts = -1.0;   // pts of the frame currently decoded and unshown
    bool m_havePending = false;
    bool m_readEof = false;       // demuxer exhausted
    bool m_finished = false;
};

#endif // CLIMAX_HAVE_FFMPEG
