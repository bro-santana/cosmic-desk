// Cosmic Desk — Bridge UI overlay (docs/UI_MIGRATION.md U2-U4).
// Draws the fullscreen ImGui layer that lives above the parallax scene and
// below the classic control window during the migration: the monitor boot
// sequence, the hosting beacon, and (later milestones) cards/dock/panels.
#pragma once

namespace cosmic::ui::bridge {

// Caller-owned state, persists across frames (and across hide/show cycles).
struct BridgeState {
    // Seconds (time_s of the input) when the boot sequence started; -1 = not
    // started yet (first draw_bridge call starts it).
    double boot_start_s = -1.0;
};

// Per-frame inputs, filled by main.cpp.
struct BridgeInput {
    bool hosting_ok = true;   // hostglue started; FAIL shows on the boot lines
    int port_base = 47989;    // shown in the boot line + beacon pill
    int paired_count = 0;     // hostglue::paired_client_count()
    double time_s = 0.0;      // SDL_GetTicks64()/1000.0
};

// Draws the fullscreen Bridge window and its in-scene monitor UI. Call once
// per frame in MainWindow mode BEFORE the classic window is drawn (so the
// classic window stays on top). The fullscreen window has no background
// (the scene shows through). Returns the screen-logo opacity the caller
// should pass to the scene (1 during boot, fading to 0 over 1.4s after boot
// completes at 4.4s) so the scene can alpha-mod the logo texture.
float draw_bridge(const BridgeInput& in, BridgeState* state);

}  // namespace cosmic::ui::bridge