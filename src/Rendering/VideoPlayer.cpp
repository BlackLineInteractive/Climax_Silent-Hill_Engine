#include "ClimaxEngine/Rendering/VideoPlayer.h"

#ifdef CLIMAX_HAVE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstdio>
#include <vector>

VideoPlayer::~VideoPlayer() { Close(); }

void VideoPlayer::Close() {
    if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_rgbBuf) { av_free(m_rgbBuf); m_rgbBuf = nullptr; }
    if (m_rgbFrame) { av_frame_free(&m_rgbFrame); }
    if (m_frame) { av_frame_free(&m_frame); }
    if (m_packet) { av_packet_free(&m_packet); }
    if (m_codec) { avcodec_free_context(&m_codec); }
    if (m_fmt) { avformat_close_input(&m_fmt); }
    if (m_tex) { glDeleteTextures(1, &m_tex); m_tex = 0; }

    m_streamIndex = -1;
    m_width = m_height = 0;
    m_timeBase = 0.0;
    m_playbackTime = 0.0;
    m_pendingPts = -1.0;
    m_havePending = false;
    m_readEof = false;
    m_finished = false;
    m_path.clear();
}

bool VideoPlayer::Open(const std::string &path) {
    Close();

    if (avformat_open_input(&m_fmt, path.c_str(), nullptr, nullptr) < 0) {
        std::fprintf(stderr, "[video] cannot open %s\n", path.c_str());
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0) {
        std::fprintf(stderr, "[video] no stream info in %s\n", path.c_str());
        Close();
        return false;
    }

    const AVCodec *decoder = nullptr;
    m_streamIndex = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
                                        &decoder, 0);
    if (m_streamIndex < 0 || !decoder) {
        std::fprintf(stderr, "[video] no video stream in %s\n", path.c_str());
        Close();
        return false;
    }

    m_codec = avcodec_alloc_context3(decoder);
    AVStream *stream = m_fmt->streams[m_streamIndex];
    if (!m_codec ||
        avcodec_parameters_to_context(m_codec, stream->codecpar) < 0 ||
        avcodec_open2(m_codec, decoder, nullptr) < 0) {
        std::fprintf(stderr, "[video] cannot open decoder for %s\n", path.c_str());
        Close();
        return false;
    }

    m_width = m_codec->width;
    m_height = m_codec->height;
    m_timeBase = av_q2d(stream->time_base);

    m_frame = av_frame_alloc();
    m_rgbFrame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_rgbFrame || !m_packet) {
        Close();
        return false;
    }

    // RGBA, not RGB24. Three bytes per pixel makes the row stride a multiple
    // of 3 rather than of 4, and that odd stride is what produced the
    // combed, channel-shifted picture (a KONAMI screenshot showed it clearly):
    // each row landed a few bytes off where GL expected it, so R/G/B read from
    // the wrong offsets a little further into every subsequent line. Four
    // bytes per pixel makes every row's byte length a whole number of pixels
    // by construction, for any width, so the whole class of stride bugs goes
    // away rather than being patched with GL_UNPACK_ROW_LENGTH.
    const int rgbaBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, m_width,
                                                    m_height, 1);
    m_rgbBuf = (uint8_t *)av_malloc((size_t)rgbaBytes);
    av_image_fill_arrays(m_rgbFrame->data, m_rgbFrame->linesize, m_rgbBuf,
                         AV_PIX_FMT_RGBA, m_width, m_height, 1);

    m_sws = sws_getContext(m_width, m_height, m_codec->pix_fmt, m_width,
                           m_height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                           nullptr, nullptr);
    if (!m_sws) {
        Close();
        return false;
    }

    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    // Cleared to opaque black so the first frame or two of B-frame reorder
    // delay do not show whatever GL happened to leave in that texture slot.
    std::vector<uint8_t> blank((size_t)m_width * m_height * 4, 0);
    for (size_t i = 3; i < blank.size(); i += 4) blank[i] = 255;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_width, m_height, 0, GL_RGBA,
                GL_UNSIGNED_BYTE, blank.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_path = path;
    m_playbackTime = 0.0;
    m_pendingPts = -1.0;
    m_havePending = false;
    m_readEof = false;
    m_finished = false;
    return true;
}

void VideoPlayer::Restart() {
    if (m_path.empty())
        return;
    // Reopening is simpler and more robust than seeking a stream that may not
    // carry an index -- these clips are a few tens of seconds, so the cost of
    // a fresh open is not worth avoiding.
    const std::string p = m_path;
    Open(p);
}

bool VideoPlayer::DecodeNextFrame() {
    for (;;) {
        const int recv = avcodec_receive_frame(m_codec, m_frame);
        if (recv == 0) {
            const int64_t pts = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                                     ? m_frame->best_effort_timestamp
                                     : 0;
            m_pendingPts = (double)pts * m_timeBase;
            m_havePending = true;
            return true;
        }
        if (recv != AVERROR(EAGAIN)) {
            // AVERROR_EOF or a real decode error: nothing more to hand out.
            return false;
        }

        // Decoder wants another packet.
        if (m_readEof)
            return false;   // already told it there is nothing left

        int read;
        do {
            read = av_read_frame(m_fmt, m_packet);
        } while (read >= 0 && m_packet->stream_index != m_streamIndex &&
                (av_packet_unref(m_packet), true));

        if (read < 0) {
            m_readEof = true;
            avcodec_send_packet(m_codec, nullptr);   // flush
            continue;
        }
        avcodec_send_packet(m_codec, m_packet);
        av_packet_unref(m_packet);
    }
}

void VideoPlayer::UploadFrame() {
    sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, m_height,
             m_rgbFrame->data, m_rgbFrame->linesize);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    // linesize[0] is exactly width*4 here (alignment was requested as 1, and
    // 4 already divides any width), so the default GL_UNPACK_ROW_LENGTH of 0
    // -- meaning "packed tightly" -- is simply correct. No stride workaround
    // needed, which is the point of using RGBA over RGB24 in the first place.
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_width, m_height, GL_RGBA,
                    GL_UNSIGNED_BYTE, m_rgbFrame->data[0]);
}

void VideoPlayer::Update(float dt) {
    if (!m_fmt || m_finished)
        return;
    m_playbackTime += dt;

    bool shown = false;
    for (;;) {
        if (!m_havePending && !DecodeNextFrame()) {
            if (m_readEof) {
                // Nothing left to decode. The last frame already on screen is
                // the final one; there is nothing more to wait for.
                m_finished = true;
            }
            break;
        }
        if (m_pendingPts > m_playbackTime)
            break;   // this frame is still in the future

        // Due or overdue. If several frames are overdue at once -- the app was
        // paused, or a hitch -- only the last one is actually drawn; the ones
        // before it are decoded and thrown away, which is what any player does
        // rather than fall further behind trying to show every dropped frame.
        UploadFrame();
        shown = true;
        m_havePending = false;
    }
    (void)shown;
}

#endif // CLIMAX_HAVE_FFMPEG
