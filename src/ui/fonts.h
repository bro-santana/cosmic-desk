// Cosmic Desk — IBM Plex font atlas.
//
// Loads the IBM Plex Sans/Mono faces committed to assets/fonts/ into the
// ImGui font atlas at the current UI scale (see ui/scale.h). The atlas is
// rebuilt whenever the display scale changes; LoadFonts() replaces the
// AddFontDefault() call that used to live in scale.cpp. The Font*() getters
// let Bridge UI PushFont() the exact face the design calls for instead of
// relying on io.FontDefault.

#pragma once
#include <imgui.h>

namespace cosmic::ui {

// Loads the IBM Plex faces into the current ImGui font atlas at the given UI
// scale and builds the atlas (replaces AddFontDefault in scale.cpp). Missing
// font files fall back to ImGui's default font and log to stderr. Sets
// Sans Regular as io.FontDefault.
void LoadFonts(float ui_scale);
ImFont* FontSansRegular();
ImFont* FontSansMedium();
ImFont* FontSansSemiBold();
ImFont* FontMonoRegular();
ImFont* FontMonoMedium();
ImFont* FontMonoBold();

}  // namespace cosmic::ui