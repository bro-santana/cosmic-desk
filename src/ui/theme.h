// Cosmic Desk — application theme.
//
// The default theme is a Horizon-style palette (https://github.com/aristocratos/btop)
// tuned toward the colors of the logo/hero artwork (assets/cosmicdesk-logo-hero.svg):
// a deep-indigo space background with the nebula's soft purple/green/blue and the
// sun's amber as accents. The theme lives in one place so more themes can be added
// alongside it later.

#pragma once

#include <imgui.h>

namespace cosmic::ui {

// Semantic accent used to tint action buttons (see AccentButton).
enum class Accent {
    Positive,     // sage green: Connect, OK, connected states
    Destructive,  // rose: Remove, disconnect, errors
};

// Applies the default Cosmic Desk theme to the current ImGui style. Call after
// ImGui::CreateContext() and again whenever the style is reset from scratch
// (scale.cpp apply() recreates the style before rescaling it).
void StyleColorsDefault();

// Returns the button-safe color for a semantic accent (dark enough for white
// text). Reusable with PushStyleColor for non-button highlights.
ImVec4 AccentColor(Accent accent);

// A Button tinted with a semantic accent: AccentColor() as the background,
// brightening on hover and darkening while pressed. Returns Button()'s result.
bool AccentButton(const char* label, Accent accent);

}  // namespace cosmic::ui
