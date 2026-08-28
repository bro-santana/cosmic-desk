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
    bool settings_open = false;       // U4: the Settings panel is open
    // U4: the Pair modal. pair_address_buf is prefilled by the card PAIR
    // buttons (with the host address) or cleared by the dock/empty-card
    // buttons; the nickname/port fields always start clean.
    bool pair_modal_open = false;
    char pair_address_buf[64] = {};
    char pair_nickname_buf[48] = {};
    bool pair_use_default_port = true;
    int pair_port_input = 0;
    // Seconds (time_s of the input) when pairing became visible; -1 = idle.
    // Anchors the Pair modal's slide-to-corner + scrim fade. The PIN panel has
    // its OWN anchor (pin_shown_at_s) so its entry animation plays from the
    // PIN's first frame even when pairing started much earlier.
    double pair_slide_at_s = -1.0;
    // Seconds (time_s of the input) when the pairing PIN first became visible;
    // -1 = PIN not showing. Anchors the PIN panel's slide-up and scan cycle.
    double pin_shown_at_s = -1.0;
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
    // Settings panel values (U4), applied by main.cpp when the panel emits a
    // Set* action.
    cosmic::ResolutionMode resolution_mode = cosmic::ResolutionMode::HostNative;
    int fps = 60;
    int bitrate_kbps = 20000;
    bool autostart = false;
    // Pairing feedback (U4), filled by main.cpp from the pair latch: drives
    // the in-scene PIN panel and the Pair modal's pairing state.
    bool pairing_active = false;
    bool pairing_show_pin = false;
    std::string pairing_pin;
    std::string pairing_error;
};

// User actions, applied by main.cpp AFTER the ImGui frame (same discipline as
// HostListAction).
struct BridgeAction {
    enum Kind {
        None,
        Connect,
        Edit,
        ToggleSettings,  // no longer emitted: the dock flips BridgeState directly
        Disconnect,
        SetResolution,
        SetFps,
        SetBitrate,
        SetPortBase,
        SetAutostart,
        CloseSettings,
        StartPair,   // address + nickname + port (0 = follow port_base)
        CancelPair,  // stop an in-flight handshake; the modal stays open
        ClosePair,   // dismiss the Pair modal and clear its sticky error
    } kind = None;
    std::string address;   // Connect / StartPair / Edit target
    std::string nickname;  // StartPair / Edit (Edit is already uppercased)
    cosmic::ResolutionMode resolution = cosmic::ResolutionMode::HostNative;  // SetResolution
    int value = 0;         // SetFps / SetBitrate / SetPortBase
    int port = 0;          // StartPair: 0 = follow port_base
    bool on = false;       // SetAutostart
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