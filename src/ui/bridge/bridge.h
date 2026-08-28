// Cosmic Desk — Bridge UI overlay (docs/UI_MIGRATION.md U2-U4).
// Draws the fullscreen ImGui layer that lives above the parallax scene and
// below the classic control window during the migration: the monitor boot
// sequence, the hosting beacon, and (U3) the machine cards, bottom dock and
// session status.
#pragma once
#include "app/settings.h"
#include <string>
#include <vector>

namespace cosmic::ui::bridge {

// Caller-owned state, persists across frames (and across hide/show cycles).
struct BridgeState {
    // Seconds (time_s of the input) when the boot sequence started; -1 = not
    // started yet (first draw_bridge call starts it).
    double boot_start_s = -1.0;
    std::string selected;             // address of the selected card
    std::string renaming;             // address being renamed inline ("" = none)
    char rename_buf[48] = {};         // inline rename buffer
};

// Per-frame inputs, filled by main.cpp.
struct BridgeInput {
    bool hosting_ok = true;
    int port_base = 47989;
    int paired_count = 0;
    double time_s = 0.0;
    std::vector<cosmic::SavedHost> hosts;   // settings.hosts_snapshot()
    std::string session_label = "IDLE";     // IDLE/PAIRING/CONNECTING/STREAMING
    bool session_busy = false;              // g_session->busy()
    bool connected_or_connecting = false;   // show DISCONNECT
    std::string connecting_address;         // address a Connect is in flight to ("" = none)
};

// User actions, applied by main.cpp AFTER the ImGui frame (same discipline as
// HostListAction).
struct BridgeAction {
    enum Kind { None, Connect, OpenPairModal, Edit, ToggleSettings, Disconnect } kind = None;
    std::string address;   // Connect / OpenPairModal prefill / Edit target
    std::string nickname;  // Edit (already uppercased)
};

struct BridgeDrawResult {
    BridgeAction action;
    float screen_logo_alpha = 1.0f;   // as in U2
};

// Draws the fullscreen Bridge window and its in-scene monitor UI. Call once
// per frame in MainWindow mode BEFORE the classic window is drawn (so the
// classic window stays on top). The fullscreen window has no background
// (the scene shows through). Returns the screen-logo opacity the caller
// should pass to the scene (1 during boot, fading to 0 over 1.4s after boot
// completes at 4.4s) so the scene can alpha-mod the logo texture.
BridgeDrawResult draw_bridge(const BridgeInput& in, BridgeState* state);

}  // namespace cosmic::ui::bridge