// Cosmic Desk — viewer video renderer (plan M2.4). SDL IYUV texture upload +
// present, mirroring moonlight-embedded src/video/sdl.c. The main loop calls
// vrenderer_render() with a frame acquired from the decoder and owns the SDL
// renderer; this module only owns the streaming texture.

#pragma once

#include <SDL.h>

struct AVFrame;

namespace cosmic::viewer {

// Creates the streaming IYUV texture for the negotiated stream dimensions.
// Returns 0 on success, non-zero on failure. Idempotent.
int vrenderer_init(SDL_Renderer* renderer, int stream_width, int stream_height);

// Uploads frame and draws it into `video_area`, letterboxed to preserve the
// stream's aspect ratio (M2 stretch fill became aspect fit). A null frame
// renders a black clear instead.
void vrenderer_render(SDL_Renderer* renderer, AVFrame* frame,
                      const SDL_Rect& video_area);

// Black clear of the whole output, then re-presents the last decoded frame
// into `video_area` (letterboxed) for the "no frame yet" case. The placeholder
// overlay text stays owned by main.cpp.
void vrenderer_present_no_frame(SDL_Renderer* renderer,
                                const SDL_Rect& video_area);

// Destroys the texture. Idempotent.
void vrenderer_deinit();

}  // namespace cosmic::viewer