// Cosmic Desk — viewer audio renderer (plan M2.5). Opus multistream decode +
// SDL_QueueAudio, mirroring moonlight-embedded src/audio/sdl.c: the decoder is
// created from the negotiated multistream configuration (stereo negotiates to
// 48 kHz, 2 channels, 1 stream, 1 coupled stream, mapping {0,1}) and decoded
// PCM is queued to a non-blocking SDL audio device.

#include "viewer/audio.h"

#include <SDL.h>
#include <opus_multistream.h>

#include <cstdio>
#include <cstdlib>

namespace cosmic::viewer {
namespace {

OpusMSDecoder* g_decoder = nullptr;
short* g_pcm_buffer = nullptr;
int g_channel_count = 0;
int g_samples_per_frame = 0;
SDL_AudioDeviceID g_device = 0;

}  // namespace

int audio_init(const OPUS_MULTISTREAM_CONFIGURATION& opus_config) {
    if (g_decoder != nullptr) {
        return 0;  // Double-init guard.
    }

    int err = 0;
    g_decoder = opus_multistream_decoder_create(
        opus_config.sampleRate, opus_config.channelCount, opus_config.streams,
        opus_config.coupledStreams, opus_config.mapping, &err);
    if (err != OPUS_OK || g_decoder == nullptr) {
        std::fprintf(stderr, "opus_multistream_decoder_create failed: %d\n", err);
        g_decoder = nullptr;
        return -1;
    }
    // Slight volume boost (moonlight-qt applies the same gain).
    opus_multistream_decoder_ctl(g_decoder, OPUS_SET_GAIN(100));

    g_channel_count = opus_config.channelCount;
    g_samples_per_frame = opus_config.samplesPerFrame;
    g_pcm_buffer = static_cast<short*>(
        std::malloc(sizeof(short) * g_channel_count * g_samples_per_frame));
    if (g_pcm_buffer == nullptr) {
        opus_multistream_decoder_destroy(g_decoder);
        g_decoder = nullptr;
        return -1;
    }

    SDL_InitSubSystem(SDL_INIT_AUDIO);  // Idempotent.

    SDL_AudioSpec want = {};
    want.freq = opus_config.sampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = static_cast<Uint8>(g_channel_count);
    want.samples = 4096;
    SDL_AudioSpec have = {};
    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_device == 0) {
        std::fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        std::free(g_pcm_buffer);
        g_pcm_buffer = nullptr;
        opus_multistream_decoder_destroy(g_decoder);
        g_decoder = nullptr;
        return -1;
    }
    SDL_PauseAudioDevice(g_device, 0);  // Start playback.
    return 0;
}

int audio_decode_sample(char* data, int len) {
    if (g_decoder == nullptr) {
        return -1;  // Not initialized: drop the sample.
    }
    const int decoded = opus_multistream_decode(
        g_decoder, reinterpret_cast<unsigned char*>(data), len, g_pcm_buffer,
        g_samples_per_frame, 0);
    if (decoded > 0) {
        // Return value intentionally ignored: overflow just drops audio (v1).
        SDL_QueueAudio(g_device, g_pcm_buffer,
                       decoded * g_channel_count * sizeof(short));
    } else if (decoded < 0) {
        std::fprintf(stderr, "Opus decode error: %d\n", decoded);
    }
    return 0;
}

void audio_deinit() {
    if (g_decoder != nullptr) {
        opus_multistream_decoder_destroy(g_decoder);
        g_decoder = nullptr;
    }
    if (g_pcm_buffer != nullptr) {
        std::free(g_pcm_buffer);
        g_pcm_buffer = nullptr;
    }
    if (g_device != 0) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    g_channel_count = 0;
    g_samples_per_frame = 0;
}

}  // namespace cosmic::viewer