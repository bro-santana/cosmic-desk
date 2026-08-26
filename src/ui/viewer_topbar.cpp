// Cosmic Desk — viewer top bar implementation (plan M4.1). ASCII-only strings:
// the default ImGui font has no glyphs beyond Basic Latin, so anything else
// renders as '?'.

#include "ui/viewer_topbar.h"

#include <SDL.h>
#include <imgui.h>

namespace cosmic::ui {
namespace {

constexpr float kTopBarHeight = 32.0f;
constexpr float kTopBarZone = 48.0f;  // px from the top where the bar reappears
constexpr std::uint64_t kAutoHideMs = 2000;

}  // namespace

TopBarAction draw_topbar(TopBarState* state, bool fullscreen, int monitor_count) {
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
                       mouse_y >= 0.0f && mouse_y < kTopBarZone;

  // Auto-hide: the bar shows while the mouse is inside the top zone and hides
  // 2 s after the last motion there (or immediately when the mouse leaves).
  if (in_zone) {
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
      state->last_motion_time_ms = SDL_GetTicks64();
      state->visible = true;
    }
  } else {
    state->visible = false;
  }
  if (state->visible &&
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
  ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, kTopBarHeight), ImGuiCond_Always);
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
  // M5: populate from CosmicDisplays + send Ctrl+Alt+Shift+F1+i.
  int monitor_index = 0;
  ImGui::BeginDisabled(true);
  ImGui::Combo("Monitor", &monitor_index, "1 (primary)\0\0");
  ImGui::EndDisabled();
  action.monitor_index = monitor_index;

  ImGui::End();
  return action;
}

}  // namespace cosmic::ui