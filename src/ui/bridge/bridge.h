// Cosmic Desk — Bridge UI overlay (docs/UI_MIGRATION.md U2-U4).
// Draws the fullscreen ImGui layer that sits above the parallax scene: the
// monitor boot sequence, the hosting beacon, and (U3) the machine cards,
// bottom dock and session status.
#pragma once
#include "app/settings.h"
#include <map>
#include <string>
#include <vector>

namespace cosmic::ui {

// Live pair-worker feedback for the Pair modal. main.cpp fills this from
// viewer::SessionStatus: the ui layer must not include viewer/session.h (same
// bridging as MonitorInfo / to_monitor_info in main.cpp).
// Moved here from the deleted ui/host_list.h (U7).
struct PairProgress {
  bool active = false;      // a pair worker is running
  bool show_pin = false;    // state is PairingNeedPin/PairingInProgress
  std::string pin;          // 4 digits, shown large
  std::string message;
  std::string error;        // sticky; non-empty = the last attempt failed
};

}  // namespace cosmic::ui

namespace cosmic::ui::bridge {

// Caller-owned state, persists across frames (and across hide/show cycles).
struct BridgeState {
    // Seconds (time_s of the input) when the logo splash started; -1 = not
    // started yet (the next draw_bridge call starts it). main.cpp resets it
    // to -1 whenever the window is unhidden from the tray so the splash
    // replays.
    double boot_start_s = -1.0;
    std::string selected;             // address of the selected card
    // PLAN.md D10(e): set when the app leaves Viewing (main.cpp's
    // Viewing-exit transition, and the close-to-tray path that ends the
    // session) so the card that is still selected (from the click that
    // launched the stream) stops feeding the steady backdrop weight;
    // cleared by the next explicit card click.
    bool backdrop_selection_muted = false;
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
    // Delete-confirmation modal (handoff README §4). Address of the host
    // awaiting confirmation ("" = closed), and the ImGui frame it opened on.
    // A state field rather than a BridgeAction because the click that opens it
    // also commits an in-progress rename (which emits Edit) in the same frame.
    // The frame stamp exists because the trash button opens the modal on the
    // mouse PRESS: without it the modal's own scrim would see that same press
    // on its first frame and close immediately.
    std::string delete_modal_address;
    int delete_modal_frame = -1;
    // Inline stepper editing (Settings panel): id of the stepper whose value
    // is being typed directly ("" = none, else "fps"/"port"), and the digits
    // buffer. Entered by clicking the value between the -/+ buttons; committed
    // on Enter or focus loss (clamped to the stepper's range), Esc cancels.
    std::string editing_stepper;
    char stepper_edit_buf[8] = {};
    // Hover scale (polish pass 2): per-card current scale factor, keyed by
    // address (missing = 1.0). Eased toward the hover target each frame.
    std::map<std::string, float> card_scale;
    // Seconds (time_s of the input) of the previous frame's scale easing;
    // -1 = first frame (treated as 1/60 s). Drives the hover-scale dt.
    double last_scale_time_s = -1.0;
};

// Per-frame inputs, filled by main.cpp.
struct BridgeInput {
    bool hosting_ok = true;
    int port_base = 47989;
    int paired_count = 0;
    double time_s = 0.0;
    std::vector<cosmic::SavedHost> hosts;   // settings.hosts_snapshot()
    std::map<std::string, bool> presence;   // address -> reachable (U6)
    std::string session_label = "IDLE";     // IDLE/PAIRING/CONNECTING/STREAMING
    bool session_busy = false;              // g_session->busy()
    bool connected_or_connecting = false;   // show DISCONNECT
    std::string connecting_address;         // address a Connect is in flight to ("" = none)
    float warp = 0.0f;  // U5: scene warp progress (0 = bridge, 1 = streaming);
                        // drives the cards' exit and scale-up during the warp
    // Settings panel values (U4), applied by main.cpp when the panel emits a
    // Set* action.
    cosmic::ResolutionMode resolution_mode = cosmic::ResolutionMode::HostNative;
    int fps = 60;
    int bitrate_kbps = 20000;
    bool autostart = false;
    bool share_wallpaper = true;  // settings.share_wallpaper: host-side opt-out (PLAN.md D10)
    bool share_clipboard = true;  // settings.share_clipboard: host-side opt-out
    // True when cosmicsvc spawned us (main.cpp --service). The service already
    // starts the host at boot, and autostart.cpp's HKCU Run key would land in
    // the SYSTEM profile's hive rather than the logged-on user's, so the
    // Settings panel renders the autostart toggle locked in this mode.
    bool service_mode = false;
    // Pairing feedback (U4), filled by main.cpp from the pair latch: drives
    // the in-scene PIN panel and the Pair modal's pairing state.
    bool pairing_active = false;
    bool pairing_show_pin = false;
    std::string pairing_pin;
    std::string pairing_error;
};

// User actions, applied by main.cpp AFTER the ImGui frame (the codebase's
// no-side-effects-mid-frame discipline).
struct BridgeAction {
    enum Kind {
        None,
        Connect,
        Edit,
        Remove,  // address: forget the host locally
        Disconnect,
        SetResolution,
        SetFps,
        SetBitrate,
        SetPortBase,
        SetAutostart,
        SetShareWallpaper,
        SetShareClipboard,
        CloseSettings,
        StartPair,   // address + nickname + port (0 = follow port_base)
        CancelPair,  // stop an in-flight handshake; the modal stays open
        ClosePair,   // dismiss the Pair modal and clear its sticky error
    } kind = None;
    std::string address;   // Connect / StartPair / Edit / Remove target
    std::string nickname;  // StartPair / Edit (Edit is already uppercased)
    cosmic::ResolutionMode resolution = cosmic::ResolutionMode::HostNative;  // SetResolution
    int value = 0;         // SetFps / SetBitrate / SetPortBase
    int port = 0;          // StartPair: 0 = follow port_base
    bool on = false;       // SetAutostart / SetShareWallpaper / SetShareClipboard
};

struct BridgeDrawResult {
    BridgeAction action;
    float screen_logo_alpha = 1.0f;   // as in U2
    // PLAN.md D10(e): the scene backdrop is the focused host's cached desktop
    // wallpaper. Address of the focused host, or "" for no backdrop.
    std::string backdrop_address;
    // PLAN.md D10(e): backdrop strength, 0..1, derived from the focused
    // card's eased hover scale so the wallpaper fades in as the card does.
    float backdrop_weight = 0.0f;
};

// Draws the fullscreen Bridge window and its in-scene monitor UI. Call once
// per frame in MainWindow mode. The fullscreen window has no background
// (the scene shows through). Returns the screen-logo opacity the caller
// should pass to the scene (1 during boot, fading to 0 over 1.4s after boot
// completes at 4.4s) so the scene can alpha-mod the logo texture.
BridgeDrawResult draw_bridge(const BridgeInput& in, BridgeState* state);

}  // namespace cosmic::ui::bridge