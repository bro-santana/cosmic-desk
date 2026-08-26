// Cosmic Desk — viewer video renderer (plan M2.4). SDL IYUV texture upload +
// present, mirroring moonlight-embedded src/video/sdl.c: the decoded YUV420P
// frame is uploaded with SDL_UpdateYUVTexture and stretched to the window.

#include "viewer/vrenderer.h"

// MSYS2's FFmpeg headers have no extern "C" guards; wrap them like the
// vendored host does (host/sunshine/src/video.h).
extern "C" {
#include <libavutil/frame.h>
}

#include <cstdio>

namespace cosmic::viewer {
namespace {

SDL_Texture* g_texture = nullptr;

}  // namespace

int vrenderer_init(SDL_Renderer* renderer, int stream_width, int stream_height) {
    if (g_texture != nullptr) {
        return 0;  // Double-init guard.
    }
    g_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                  SDL_TEXTUREACCESS_STREAMING, stream_width,
                                  stream_height);
    if (g_texture == nullptr) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }
    return 0;
}

void vrenderer_render(SDL_Renderer* renderer, AVFrame* frame) {
    if (frame == nullptr || g_texture == nullptr) {
        vrenderer_present_no_frame(renderer);
        return;
    }
    SDL_UpdateYUVTexture(g_texture, nullptr, frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1], frame->data[2],
                         frame->linesize[2]);
    SDL_RenderClear(renderer);
    // NULL src/dst rects stretch the texture to the full renderer output size.
    SDL_RenderCopy(renderer, g_texture, nullptr, nullptr);
}

void vrenderer_present_no_frame(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    // Redraw the last decoded frame instead of leaving the cleared black.
    // decoder_acquire_frame() hands out each frame once, and the main loop
    // presents at the display's refresh rate (120 Hz) while video arrives at
    // 60 fps, so most iterations have no new frame. Without this the stream
    // alternates image/black and flickers at half the refresh rate. The
    // streaming texture still holds the last upload, so re-presenting it is
    // free. Before the first frame there is no texture and the black stands.
    if (g_texture != nullptr) {
        SDL_RenderCopy(renderer, g_texture, nullptr, nullptr);
    }
}

void vrenderer_deinit() {
    if (g_texture != nullptr) {
        SDL_DestroyTexture(g_texture);
        g_texture = nullptr;
    }
}

}  // namespace cosmic::viewer