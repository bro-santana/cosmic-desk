// Cosmic Desk — viewer audio renderer (plan M2.5). Opus multistream decode +
// SDL audio stream, mirroring moonlight-embedded src/audio/sdl.c: the decoder
// is created from the negotiated multistream configuration (stereo negotiates
// to 48 kHz, 2 channels, 1 stream, 1 coupled stream, mapping {0,1}) and
// decoded PCM is pushed into an SDL_AudioStream bound to the default playback
// device.

#include "viewer/audio.h"

#include <SDL3/SDL.h>
#include <opus_multistream.h>

#include <cstdio>
#include <cstdlib>

namespace cosmic::viewer {
namespace {

OpusMSDecoder* g_decoder = nullptr;
short* g_pcm_buffer = nullptr;
int g_channel_count = 0;
int g_samples_per_frame = 0;
SDL_AudioStream* g_stream = nullptr;

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

    SDL_AudioSpec spec = {};
    spec.format = SDL_AUDIO_S16;
    spec.channels = g_channel_count;
    spec.freq = opus_config.sampleRate;
    g_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_stream == nullptr) {
        std::fprintf(stderr, "SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        std::free(g_pcm_buffer);
        g_pcm_buffer = nullptr;
        opus_multistream_decoder_destroy(g_decoder);
        g_decoder = nullptr;
        return -1;
    }
    // Device streams open paused: resume to start playback. Result is
    // ignored (init still succeeds), but a failure here is logged since it
    // would otherwise silently leave the session without audio.
    if (!SDL_ResumeAudioStreamDevice(g_stream)) {
        std::fprintf(stderr, "SDL_ResumeAudioStreamDevice failed: %s\n", SDL_GetError());
    }
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
        // Return value intentionally ignored: failure just drops audio (v1).
        SDL_PutAudioStreamData(
            g_stream, g_pcm_buffer,
            static_cast<int>(decoded * g_channel_count * sizeof(short)));
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
    if (g_stream != nullptr) {
        SDL_DestroyAudioStream(g_stream);
        g_stream = nullptr;
    }
    g_channel_count = 0;
    g_samples_per_frame = 0;
}

}  // namespace cosmic::viewer