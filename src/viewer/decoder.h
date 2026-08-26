// Cosmic Desk — viewer video decoder (plan M2.3). The session callbacks in
// session.cpp call into these functions; the decode thread (M2-T3) fills a
// single "latest frame" slot that the main loop drains for rendering (plan D2).

#pragma once

#include <Limelight.h>

struct AVFrame;

namespace cosmic::viewer {

// Called from the video setup callback with the negotiated stream geometry.
// Returns 0 on success, non-zero on failure.
int decoder_init(const STREAM_CONFIGURATION& cfg);

// Called from the video cleanup callback. Idempotent.
void decoder_deinit();

// Called from the submitDecodeUnit callback. Returns DR_OK (0) on success or
// DR_NEED_IDR (-1) to request a keyframe.
int decoder_submit(PDECODE_UNIT decodeUnit);

// Takes ownership of the newest decoded frame, or returns nullptr if none is
// ready. Never blocks. The caller must hand the frame back with
// decoder_release_frame().
AVFrame* decoder_acquire_frame();

// Returns a frame previously taken with decoder_acquire_frame().
void decoder_release_frame(AVFrame* frame);

}  // namespace cosmic::viewer