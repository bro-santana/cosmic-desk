// Cosmic Desk — viewer video decoder (plan M2.3). Real avcodec H.264 decode
// thread: the submit callback copies each decode unit into an owned buffer and
// queues it; the decode thread feeds the Annex-B stream to avcodec, converts
// NV12 frames to YUV420P, and swaps the result into a single "latest frame"
// slot that the main loop drains without blocking (plan D2).
//
// Pattern: moonlight-embedded src/video/ffmpeg.c (decode loop, in-band SPS/PPS
// from the decode unit's buffer chain) adapted to a separate decode thread and
// a mutex-protected frame slot.

#include "viewer/decoder.h"

// MSYS2's FFmpeg headers have no extern "C" guards; wrap them like the
// vendored host does (host/sunshine/src/video.h).
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace cosmic::viewer {
namespace {

// Decode units queued beyond this bound mean the decoder is falling behind;
// return DR_NEED_IDR so the host sends a keyframe and the backlog resets.
constexpr size_t kMaxQueuedUnits = 24;

// A decode unit copied out of the library-owned DU chain. The library frees
// the DU right after submitDecodeUnit() returns, so the decode thread works
// on its own copy.
struct QueuedUnit {
    uint8_t* data = nullptr;
    int length = 0;
};

struct DecoderState {
    AVCodecContext* codec_ctx = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws_ctx = nullptr;
    AVPixelFormat sws_src_fmt = AV_PIX_FMT_NONE;

    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<QueuedUnit> queue;
    bool stop = false;

    // Newest converted frame, guarded by mutex. Ownership moves to the caller
    // via decoder_acquire_frame().
    AVFrame* latest = nullptr;

    bool initialized = false;
};

DecoderState g_state;

AVFrame* convert_to_yuv420p(const AVFrame* src) {
    if (src->format != AV_PIX_FMT_NV12 && src->format != AV_PIX_FMT_YUV420P) {
        std::fprintf(stderr, "Decoder produced unsupported pixel format %d\n",
                     src->format);
        return nullptr;
    }

    // The sws context is only touched by the decode thread, so no locking.
    if (g_state.sws_ctx == nullptr || g_state.sws_src_fmt != src->format) {
        sws_freeContext(g_state.sws_ctx);
        g_state.sws_ctx = sws_getContext(src->width, src->height,
                                         static_cast<AVPixelFormat>(src->format),
                                         src->width, src->height,
                                         AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                         nullptr, nullptr, nullptr);
        g_state.sws_src_fmt = static_cast<AVPixelFormat>(src->format);
        if (g_state.sws_ctx == nullptr) {
            std::fprintf(stderr, "sws_getContext failed\n");
            return nullptr;
        }
    }

    AVFrame* dst = av_frame_alloc();
    if (dst == nullptr) {
        return nullptr;
    }
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width = src->width;
    dst->height = src->height;
    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        return nullptr;
    }
    sws_scale(g_state.sws_ctx, src->data, src->linesize, 0, src->height,
              dst->data, dst->linesize);
    return dst;
}

void decode_unit(const uint8_t* data, int length) {
    g_state.packet->data = const_cast<uint8_t*>(data);
    g_state.packet->size = length;

    const int send_err = avcodec_send_packet(g_state.codec_ctx, g_state.packet);
    if (send_err < 0) {
        char errbuf[128];
        av_strerror(send_err, errbuf, sizeof(errbuf));
        std::fprintf(stderr, "avcodec_send_packet failed: %s\n", errbuf);
        return;
    }

    while (true) {
        AVFrame* frame = av_frame_alloc();
        if (frame == nullptr) {
            return;
        }
        const int recv_err = avcodec_receive_frame(g_state.codec_ctx, frame);
        if (recv_err == AVERROR(EAGAIN) || recv_err == AVERROR_EOF) {
            av_frame_free(&frame);
            return;
        }
        if (recv_err < 0) {
            char errbuf[128];
            av_strerror(recv_err, errbuf, sizeof(errbuf));
            std::fprintf(stderr, "avcodec_receive_frame failed: %s\n", errbuf);
            av_frame_free(&frame);
            return;
        }

        AVFrame* converted = convert_to_yuv420p(frame);
        av_frame_free(&frame);
        if (converted == nullptr) {
            continue;
        }

        std::lock_guard lock(g_state.mutex);
        if (g_state.latest != nullptr) {
            av_frame_free(&g_state.latest);
        }
        g_state.latest = converted;
    }
}

void decode_thread() {
    while (true) {
        QueuedUnit unit;
        {
            std::unique_lock lock(g_state.mutex);
            g_state.cv.wait(lock, [&] {
                return g_state.stop || !g_state.queue.empty();
            });
            if (g_state.stop && g_state.queue.empty()) {
                return;
            }
            unit = std::move(g_state.queue.front());
            g_state.queue.pop_front();
        }
        decode_unit(unit.data, unit.length);
        delete[] unit.data;
    }
}

}  // namespace

int decoder_init(const STREAM_CONFIGURATION& cfg) {
    (void)cfg;
    if (g_state.initialized) {
        return 0;  // Double-init guard: the setup callback may fire again.
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        std::fprintf(stderr, "H.264 decoder not found\n");
        return -1;
    }
    g_state.codec_ctx = avcodec_alloc_context3(codec);
    if (g_state.codec_ctx == nullptr) {
        return -1;
    }
    // Low-delay decode: moonlight-embedded uses no frame threads for latency.
    g_state.codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(g_state.codec_ctx, codec, nullptr) < 0) {
        std::fprintf(stderr, "avcodec_open2 failed\n");
        avcodec_free_context(&g_state.codec_ctx);
        g_state.codec_ctx = nullptr;
        return -1;
    }
    g_state.packet = av_packet_alloc();
    if (g_state.packet == nullptr) {
        avcodec_free_context(&g_state.codec_ctx);
        g_state.codec_ctx = nullptr;
        return -1;
    }

    g_state.stop = false;
    g_state.thread = std::thread(decode_thread);
    g_state.initialized = true;
    return 0;
}

void decoder_deinit() {
    if (!g_state.initialized) {
        return;
    }
    {
        std::lock_guard lock(g_state.mutex);
        g_state.stop = true;
    }
    g_state.cv.notify_all();
    if (g_state.thread.joinable()) {
        g_state.thread.join();
    }

    if (g_state.packet != nullptr) {
        av_packet_free(&g_state.packet);
    }
    if (g_state.codec_ctx != nullptr) {
        avcodec_free_context(&g_state.codec_ctx);
    }
    sws_freeContext(g_state.sws_ctx);
    g_state.sws_ctx = nullptr;
    g_state.sws_src_fmt = AV_PIX_FMT_NONE;

    {
        std::lock_guard lock(g_state.mutex);
        if (g_state.latest != nullptr) {
            av_frame_free(&g_state.latest);
        }
        g_state.latest = nullptr;
        for (QueuedUnit& unit : g_state.queue) {
            delete[] unit.data;
        }
        g_state.queue.clear();
    }
    g_state.initialized = false;
}

int decoder_submit(PDECODE_UNIT decodeUnit) {
    if (decodeUnit == nullptr || decodeUnit->bufferList == nullptr) {
        return DR_OK;
    }
    {
        std::lock_guard lock(g_state.mutex);
        if (!g_state.initialized) {
            return DR_OK;  // Not decoding (yet): drop the frame.
        }
        if (g_state.queue.size() >= kMaxQueuedUnits) {
            return DR_NEED_IDR;  // Falling behind: ask the host for a keyframe.
        }
    }

    // Copy the library-owned buffer chain: the library frees the DU as soon as
    // this callback returns, so the decode thread must work on its own copy.
    uint8_t* data = new uint8_t[static_cast<size_t>(decodeUnit->fullLength) +
                                 AV_INPUT_BUFFER_PADDING_SIZE];
    size_t offset = 0;
    for (PLENTRY entry = decodeUnit->bufferList; entry != nullptr;
         entry = entry->next) {
        std::memcpy(data + offset, entry->data,
                    static_cast<size_t>(entry->length));
        offset += static_cast<size_t>(entry->length);
    }
    std::memset(data + offset, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    {
        std::lock_guard lock(g_state.mutex);
        g_state.queue.push_back({data, decodeUnit->fullLength});
    }
    g_state.cv.notify_one();
    return DR_OK;
}

AVFrame* decoder_acquire_frame() {
    std::lock_guard lock(g_state.mutex);
    AVFrame* frame = g_state.latest;
    g_state.latest = nullptr;
    return frame;
}

void decoder_release_frame(AVFrame* frame) {
    av_frame_free(&frame);
}

}  // namespace cosmic::viewer