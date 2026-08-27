// Cosmic Desk — viewer video renderer (plan M2.4). SDL IYUV texture upload +
// present, mirroring moonlight-embedded src/video/sdl.c: the decoded YUV420P
// frame is uploaded with SDL_UpdateYUVTexture and stretched to the window.

#include "viewer/vrenderer.h"

// MSYS2's FFmpeg headers have no extern "C" guards; wrap them like the
// vendored host does (host/sunshine/src/video.h).
extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstdio>

namespace cosmic::viewer {
namespace {

SDL_Texture* g_texture = nullptr;
int g_stream_width = 0;
int g_stream_height = 0;

// Centered destination rect inside `area` that fits a src_w x src_h source
// without stretching: the scale is the smaller of the two axis ratios.
SDL_Rect fit_rect(const SDL_Rect& area, int src_w, int src_h) {
    if (area.w <= 0 || area.h <= 0 || src_w <= 0 || src_h <= 0) {
        return area;
    }
    const float scale = std::min(
        static_cast<float>(area.w) / static_cast<float>(src_w),
        static_cast<float>(area.h) / static_cast<float>(src_h));
    SDL_Rect dst;
    dst.w = static_cast<int>(src_w * scale + 0.5f);
    dst.h = static_cast<int>(src_h * scale + 0.5f);
    dst.x = area.x + (area.w - dst.w) / 2;
    dst.y = area.y + (area.h - dst.h) / 2;
    return dst;
}

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
    g_stream_width = stream_width;
    g_stream_height = stream_height;
    return 0;
}

void vrenderer_render(SDL_Renderer* renderer, AVFrame* frame,
                      const SDL_Rect& video_area) {
    if (frame == nullptr || g_texture == nullptr) {
        vrenderer_present_no_frame(renderer, video_area);
        return;
    }
    SDL_UpdateYUVTexture(g_texture, nullptr, frame->data[0], frame->linesize[0],
                         frame->data[1], frame->linesize[1], frame->data[2],
                         frame->linesize[2]);
    SDL_RenderClear(renderer);
    const SDL_Rect dst = fit_rect(video_area, g_stream_width, g_stream_height);
    SDL_RenderCopy(renderer, g_texture, nullptr, &dst);
}

void vrenderer_present_no_frame(SDL_Renderer* renderer,
                                const SDL_Rect& video_area) {
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
        const SDL_Rect dst = fit_rect(video_area, g_stream_width, g_stream_height);
        SDL_RenderCopy(renderer, g_texture, nullptr, &dst);
    }
}

void vrenderer_deinit() {
    if (g_texture != nullptr) {
        SDL_DestroyTexture(g_texture);
        g_texture = nullptr;
    }
}

}  // namespace cosmic::viewer