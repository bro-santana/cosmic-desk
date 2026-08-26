// Cosmic Desk — viewer top bar (plan M4.1). ImGui strip pinned to the top of
// the window while streaming: exit, fullscreen toggle, and the monitor
// selector (placeholder until M5). Auto-hides after 2 s without mouse motion
// near the top and reappears when the mouse moves into the top ~48 px.

#pragma once

#include <cstdint>

namespace cosmic::ui {

// Action the caller applies after the ImGui frame: toggling fullscreen or
// ending the session mid-frame would corrupt ImGui state.
struct TopBarAction {
  enum { None, Exit, ToggleFullscreen } kind;
  int monitor_index;
};

// Auto-hide state, owned by the caller so it survives across frames.
struct TopBarState {
  std::uint64_t last_motion_time_ms = 0;  // SDL_GetTicks64() of the last motion
  bool visible = false;                   // bar is currently drawn
};

// Draws the top bar. Returns the action the caller applies after the frame.
//   state         — auto-hide state (persists across frames).
//   fullscreen    — current window mode; picks the "Fullscreen"/"Windowed" label.
//   monitor_count — host monitor count; unused until M5 populates the combo.
TopBarAction draw_topbar(TopBarState* state, bool fullscreen, int monitor_count);

}  // namespace cosmic::ui