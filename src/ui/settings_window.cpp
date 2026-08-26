// Cosmic Desk — settings window implementation (plan M4.4). ASCII-only
// strings: the default ImGui font has no glyphs beyond Basic Latin, so
// anything else renders as '?'.

#include "ui/settings_window.h"

#include <imgui.h>

#include <algorithm>

namespace cosmic::ui {
namespace {

// Combo labels for ResolutionMode, in enum order. Persistence reuses
// to_string() (app/settings.cpp) via Settings::save(), so the two orderings
// must stay in sync.
const char* kResolutionLabels[] = {"Host native", "1080p", "1440p", "4K", "Custom WxH"};

constexpr int kCustomWidthMin = 320;
constexpr int kCustomWidthMax = 7680;
constexpr int kCustomHeightMin = 240;
constexpr int kCustomHeightMax = 4320;
constexpr int kFpsMin = 10;
constexpr int kFpsMax = 240;
constexpr int kBitrateMbpsMin = 5;
constexpr int kBitrateMbpsMax = 150;
constexpr int kPortBaseMin = 1024;
constexpr int kPortBaseMax = 65400;

}  // namespace

void draw_settings_window(Settings &settings, bool &open) {
  // Persistence model (plan M4.4): edits land in `settings` immediately; a
  // dirty flag triggers Settings::save() once when the window closes — either
  // via its X button or by the caller flipping `open` to false. main.cpp also
  // saves at shutdown, so a clean exit never loses changes either way.
  static bool was_open = false;
  static bool dirty = false;

  if (open && !was_open) {
    dirty = false;  // Freshly opened: nothing changed yet.
  }
  if (!open) {
    if (was_open && dirty) {
      settings.save();
      dirty = false;
    }
    was_open = open;
    return;
  }
  was_open = open;

  ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Settings", &open, ImGuiWindowFlags_NoSavedSettings)) {
    // Resolution mode combo: the index maps 1:1 onto the ResolutionMode enum.
    int mode_index = static_cast<int>(settings.resolution_mode);
    if (ImGui::Combo("Resolution", &mode_index, kResolutionLabels,
                     IM_ARRAYSIZE(kResolutionLabels))) {
      settings.resolution_mode = static_cast<ResolutionMode>(mode_index);
      dirty = true;
    }

    if (settings.resolution_mode == ResolutionMode::Custom) {
      if (ImGui::InputInt("Custom width", &settings.custom_width)) {
        settings.custom_width =
            std::clamp(settings.custom_width, kCustomWidthMin, kCustomWidthMax);
        dirty = true;
      }
      if (ImGui::InputInt("Custom height", &settings.custom_height)) {
        settings.custom_height =
            std::clamp(settings.custom_height, kCustomHeightMin, kCustomHeightMax);
        dirty = true;
      }
    }

    if (ImGui::InputInt("FPS", &settings.fps)) {
      settings.fps = std::clamp(settings.fps, kFpsMin, kFpsMax);
      dirty = true;
    }

    // Bitrate is stored in kbps but edited in Mbps; the slider works on a
    // local copy so the stored value stays a clean multiple of 1000.
    int bitrate_mbps = settings.bitrate_kbps / 1000;
    if (ImGui::SliderInt("Bitrate", &bitrate_mbps, kBitrateMbpsMin, kBitrateMbpsMax,
                         "Bitrate: %d Mbps")) {
      settings.bitrate_kbps = bitrate_mbps * 1000;
      dirty = true;
    }

    if (ImGui::InputInt("Port base", &settings.port_base)) {
      settings.port_base = std::clamp(settings.port_base, kPortBaseMin, kPortBaseMax);
      dirty = true;
    }
    ImGui::TextWrapped("All six ports derive from this; restart to apply to hosting.");

    if (ImGui::Checkbox("Autostart", &settings.autostart)) {
      dirty = true;
    }
    ImGui::TextWrapped("Wired in a later milestone.");

    ImGui::Separator();
    ImGui::TextWrapped("Changes are saved to cosmic.json when this window closes.");
  }
  ImGui::End();
}

}  // namespace cosmic::ui