// Cosmic Desk — UI scale implementation. See scale.h for the contract.

#include "ui/scale.h"
#include "ui/fonts.h"
#include "ui/theme.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdlrenderer3.h>

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
    // SDL3 is per-monitor-v2 aware on Windows, so this returns the monitor's
    // effective scale directly — exactly the "Scale" percentage from Display
    // settings. On X11 it is the desktop's content scale (Xft.dpi), not 1.0;
    // that divergence is intended.
    const float scale = SDL_GetWindowDisplayScale(window);
    if (scale <= 0.0f) {
        return 1.0f;
    }

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
    // The SDLRenderer3 backend only rebuilds its font texture when it has
    // none, so drop the stale one; the next NewFrame() recreates it.
    ImGui_ImplSDLRenderer3_DestroyFontsTexture();

    std::printf("[ui] display scale %.2fx\n", g_scale);
    std::fflush(stdout);
    return true;
}

}  // namespace cosmic::ui
