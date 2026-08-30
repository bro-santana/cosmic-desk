// Cosmic Desk — UI scale for HiDPI displays.
//
// SDL3 is per-monitor-v2 DPI aware on Windows, so the OS hands us the full
// pixel grid and does no bitmap stretching of its own. Everything ImGui draws
// is therefore in device pixels: without this module the default 13 px font is
// 13 real pixels, which is unreadable on a 4K panel running at 150-225%
// scaling.
//
// scale() is the factor every hardcoded pixel dimension in the UI must be
// multiplied by; apply() rebuilds the font atlas and the ImGui style at the
// current display's scale.

#pragma once

struct SDL_Window;

namespace cosmic::ui {

// Current UI scale (1.0 = 96 DPI). Valid after the first apply(); 1.0 before.
float scale();

// Detects the scale of the display `window` is on, and — when it differs from
// the active one — rescales the ImGui style and rebuilds the font atlas at the
// new size. The SDLRenderer3 backend recreates its font texture on the next
// NewFrame(), so no renderer work is needed here.
//
// Call once after ImGui is initialized and again on
// SDL_EVENT_WINDOW_DISPLAY_CHANGED. Returns true when the scale changed.
bool apply(SDL_Window* window);

}  // namespace cosmic::ui
