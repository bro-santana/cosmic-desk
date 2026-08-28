// Cosmic Desk — UI scale implementation. See scale.h for the contract.

#include "ui/scale.h"
#include "ui/fonts.h"
#include "ui/theme.h"

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer2.h>

#include <cmath>
#include <cstdio>

namespace cosmic::ui {
namespace {

// A 4K panel at 100% is a legitimate configuration (small text is then the
// user's choice), and no shipping desktop scales past 400%. Clamping keeps a
// bogus EDID from producing an unusable window or a huge font atlas.
constexpr float kMinScale = 1.0f;
constexpr float kMaxScale = 4.0f;

float g_scale = 1.0f;

// Scale of the display `window` currently sits on, as a multiple of 96 DPI.
float detect_scale(SDL_Window* window) {
    const int display = SDL_GetWindowDisplayIndex(window);
    if (display < 0) {
        return 1.0f;
    }

#ifdef _WIN32
    // With per-monitor-v2 awareness SDL reports the monitor's effective DPI,
    // which is exactly the "Scale" percentage from Display settings x 96.
    float ddpi = 0.0f;
    if (SDL_GetDisplayDPI(display, &ddpi, nullptr, nullptr) != 0 || ddpi <= 0.0f) {
        return 1.0f;
    }
    const float scale = ddpi / 96.0f;
#else
    // X11's SDL_GetDisplayDPI reports the panel's *physical* DPI from EDID,
    // which says nothing about the desktop's scaling factor and over-scales
    // badly on large 4K screens. Use the drawable/window ratio instead: it is
    // the real factor on the backends that have one (Wayland, macOS) and 1.0
    // on X11, where the toolkit does no scaling of its own.
    (void)display;
    int window_w = 0;
    int window_h = 0;
    int drawable_w = 0;
    int drawable_h = 0;
    SDL_GetWindowSize(window, &window_w, &window_h);
    SDL_GetRendererOutputSize(SDL_GetRenderer(window), &drawable_w, &drawable_h);
    if (window_w <= 0 || drawable_w <= 0) {
        return 1.0f;
    }
    const float scale = static_cast<float>(drawable_w) / static_cast<float>(window_w);
#endif

    if (!std::isfinite(scale)) {
        return 1.0f;
    }
    return scale < kMinScale ? kMinScale : (scale > kMaxScale ? kMaxScale : scale);
}

}  // namespace

float scale() {
    return g_scale;
}

bool apply(SDL_Window* window) {
    const float target = detect_scale(window);
    // Ignore sub-1% drift so moving between two same-DPI monitors does not
    // throw away the font atlas.
    if (std::fabs(target - g_scale) < 0.01f) {
        return false;
    }
    g_scale = target;

    // ScaleAllSizes multiplies the style in place, so it must start from the
    // unscaled defaults every time rather than compounding the previous scale.
    ImGui::GetStyle() = ImGuiStyle();
    StyleColorsDefault();
    ImGui::GetStyle().ScaleAllSizes(g_scale);

    // Rasterize the IBM Plex faces at the scaled size instead of stretching
    // the 13 px atlas (io.FontGlobalScale), which would only make it blurry.
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    LoadFonts(g_scale);
    // The SDLRenderer2 backend only rebuilds its font texture when it has
    // none, so drop the stale one; the next NewFrame() recreates it.
    ImGui_ImplSDLRenderer2_DestroyFontsTexture();

    std::printf("[ui] display scale %.2fx\n", g_scale);
    std::fflush(stdout);
    return true;
}

}  // namespace cosmic::ui
