// Cosmic Desk — viewer top bar implementation (plan M4.1). ASCII-only strings:
// the default ImGui font has no glyphs beyond Basic Latin, so anything else
// renders as '?'.

#include "ui/viewer_topbar.h"

#include <cstdio>

#include <SDL.h>
#include <imgui.h>

namespace cosmic::ui {
namespace {

constexpr std::uint64_t kAutoHideMs = 2000;

// Height the row of widgets actually needs at the current font size, instead of
// a fixed 32 px that a DPI-scaled font overflows (at 225% the buttons alone are
// ~65 px). Must be called inside an ImGui frame.
float bar_height() {
  return ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f;
}

// Band below the top edge that reveals the bar: the bar itself plus slack, so
// the pointer does not have to land exactly on it.
float reveal_zone() {
  return bar_height() * 1.5f;
}

}  // namespace

TopBarAction draw_topbar(TopBarState* state, bool fullscreen,
                         const std::vector<MonitorInfo>& monitors,
                         int active_index) {
  TopBarAction action;
  action.kind = TopBarAction::None;
  action.monitor_index = 0;

  const ImGuiIO& io = ImGui::GetIO();
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  // Single-window app: the main viewport covers the whole window, so the
  // window-relative mouse position is the viewport-relative one.
  const float mouse_x = io.MousePos.x - viewport->Pos.x;
  const float mouse_y = io.MousePos.y - viewport->Pos.y;
  const bool in_zone = mouse_x >= 0.0f && mouse_x < viewport->Size.x &&
                       mouse_y >= 0.0f && mouse_y < reveal_zone();

  // Auto-hide. Two rules beyond "is the mouse near the top":
  //  - The monitor dropdown pins the bar open. Its popup list is drawn BELOW
  //    the bar, outside the top zone, so tracking the cursor alone would hide
  //    the bar — and the popup with it — the moment the mouse moved onto an
  //    entry, making a monitor impossible to pick.
  //  - Being in the zone is enough; motion is not required. Otherwise resting
  //    the pointer on the bar for 2 s hid it out from under the user.
  // monitor_combo_open is only ever updated while the bar is drawn, so keeping
  // the bar alive while it is set is also what lets it be cleared again.
  const bool pinned_open = state->monitor_combo_open;
  if (in_zone || pinned_open) {
    state->last_motion_time_ms = SDL_GetTicks64();
    state->visible = true;
  } else {
    state->visible = false;
  }
  if (state->visible && !pinned_open &&
      SDL_GetTicks64() - state->last_motion_time_ms > kAutoHideMs) {
    state->visible = false;
  }

  if (!state->visible) {
    return action;
  }

  // Fixed strip pinned to the top of the window, above the video. No focus or
  // nav so the stream keeps keyboard input; main.cpp's WantCaptureMouse gate
  // already stops forwarding while the cursor is over the bar.
  ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, bar_height()),
                           ImGuiCond_Always);
  ImGui::Begin("ViewerTopBar", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  if (ImGui::Button("Exit")) {
    action.kind = TopBarAction::Exit;
  }
  ImGui::SameLine();
  if (ImGui::Button(fullscreen ? "Windowed" : "Fullscreen")) {
    action.kind = TopBarAction::ToggleFullscreen;
  }
  ImGui::SameLine();
  // Monitor dropdown (plan M5.3): opening it re-fetches /serverinfo so hotplug
  // changes are picked up (docs/PROTOCOL.md); selecting a different entry
  // synthesizes Ctrl+Alt+Shift+F(1+i) on the host. Labels are "i+1: name (WxH)"
  // with a trailing " [active]" marker on the currently captured monitor.
  auto monitor_label = [](const MonitorInfo& m, int i, bool mark_active) {
    std::string label = std::to_string(i + 1) + ": " + m.name + " (" +
                        std::to_string(m.width) + "x" + std::to_string(m.height) +
                        ")";
    if (mark_active) {
      label += " [active]";
    }
    return label;
  };

  std::string preview = "Monitor";
  if (active_index >= 0 && active_index < static_cast<int>(monitors.size())) {
    preview = monitor_label(monitors[active_index], active_index, true);
  }
  if (ImGui::BeginCombo("Monitor", preview.c_str())) {
    // Closed->open transition: ask the caller to refresh the display list.
    if (!state->monitor_combo_open) {
      state->monitor_combo_open = true;
      action.kind = TopBarAction::RefreshDisplays;
      std::fprintf(stderr, "[topbar] combo opened: %zu monitors, active=%d\n",
                   monitors.size(), active_index);
      std::fflush(stderr);
    }
    for (int i = 0; i < static_cast<int>(monitors.size()); ++i) {
      const std::string label =
          monitor_label(monitors[i], i, i == active_index);
      if (ImGui::Selectable(label.c_str(), i == active_index)) {
        if (i != active_index) {
          action.kind = TopBarAction::SwitchMonitor;
          action.monitor_index = i;
        }
      }
    }
    ImGui::EndCombo();
  } else {
    state->monitor_combo_open = false;
  }

  ImGui::End();
  return action;
}

}  // namespace cosmic::ui
