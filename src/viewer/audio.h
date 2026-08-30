// Cosmic Desk — viewer audio renderer (plan M2.5). Opus multistream decode +
// SDL audio stream (pattern: moonlight-embedded src/audio/sdl.c). The session
// callbacks in session.cpp call into these functions.

#pragma once

#include <Limelight.h>

namespace cosmic::viewer {

// Called from the audio init callback with the negotiated Opus multistream
// parameters. Returns 0 on success, non-zero on failure.
int audio_init(const OPUS_MULTISTREAM_CONFIGURATION& opus_config);

// Called from the decodeAndPlaySample callback. data holds len bytes of Opus
// data. Returns 0 on success.
int audio_decode_sample(char* data, int len);

// Called from the audio cleanup callback. Idempotent.
void audio_deinit();

}  // namespace cosmic::viewer