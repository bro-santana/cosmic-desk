// Cosmic Desk — viewer top bar (plan M4.1). ImGui strip pinned to the top of
// the window while streaming: exit, fullscreen toggle, and the monitor
// selector (placeholder until M5). Auto-hides after 2 s without mouse motion
// near the top and reappears when the mouse moves into the top ~48 px.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cosmic::ui {

// Action the caller applies after the ImGui frame: toggling fullscreen or
// ending the session mid-frame would corrupt ImGui state.
struct TopBarAction {
  enum {
    None,
    Exit,
    ToggleFullscreen,
    RefreshDisplays,  // COSMIC MODIFICATION (M5): monitor dropdown opened.
    SwitchMonitor,    // COSMIC MODIFICATION (M5): a different monitor selected.
  } kind;
  int monitor_index;
};

// COSMIC MODIFICATION (M5): one monitor entry for the dropdown, filled by the
// caller from the session's DisplayInfo snapshot each frame.
struct MonitorInfo {
  std::string name;
  int width = 0;
  int height = 0;
  bool active = false;
};

// Height (in window pixels) the bar reserves at the top of the window. The
// caller subtracts this from the window to size the video area so the stream
// never overlaps the bar. Valid once an ImGui context + scaled style exist.
float topbar_height();

// Auto-hide state, owned by the caller so it survives across frames.
struct TopBarState {
  std::uint64_t last_motion_time_ms = 0;  // SDL_GetTicks() of the last motion
  bool visible = false;                   // bar is currently drawn
  // COSMIC MODIFICATION (M5): monitor dropdown open state, used to detect the
  // closed->open transition (which triggers a /serverinfo refresh).
  bool monitor_combo_open = false;
};

// Draws the top bar. Returns the action the caller applies after the frame.
//   state         — auto-hide state (persists across frames).
//   fullscreen    — current window mode; picks the "Fullscreen"/"Windowed" label.
//   monitors      — host monitor list (from the session's display snapshot).
//   active_index  — index of the currently captured monitor in `monitors`.
// COSMIC MODIFICATION (M5): the placeholder combo is replaced by a real one.
// Opening it returns RefreshDisplays (the caller re-fetches /serverinfo);
// selecting a different entry returns SwitchMonitor with monitor_index set.
TopBarAction draw_topbar(TopBarState* state, bool fullscreen,
                         const std::vector<MonitorInfo>& monitors,
                         int active_index);

}  // namespace cosmic::ui
