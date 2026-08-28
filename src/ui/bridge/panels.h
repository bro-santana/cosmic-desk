// Cosmic Desk — Bridge UI panels (docs/UI_MIGRATION.md U4).
//
// The Settings panel: a floating glass child window pinned to the bridge
// window (right 4% / top 10%, 348 px wide), drawn manually like the machine
// cards. The panel emits BridgeAction Set* actions when the user changes a
// value; main.cpp applies them to Settings immediately and marks the settings
// dirty; save() fires on the panel-close transition (plus shutdown).
//
// The Pair modal (372 px, depth 80): a centered child window with a full-
// viewport scrim that click-closes when idle; while pairing it slides to the
// lower-left and the scrim fades out so the in-scene PIN stays prominent. It
// emits StartPair/CancelPair/ClosePair actions.
//
// The PIN panel: the viewer-side pairing PIN drawn inside the monitor screen
// rect (scene::screen_rect), fed by the real handshake latch. It emits no
// actions.
#pragma once

namespace cosmic::ui::bridge {

struct BridgeInput;
struct BridgeState;
struct BridgeAction;

// Draws the Settings panel when state->settings_open. Emits SetX actions when
// the user changes values (main.cpp applies them to Settings + saves).
void draw_settings_panel(const BridgeInput& in, BridgeState* state, BridgeAction* out_action);

// Draws the Pair modal when state->pair_modal_open: scrim + centered/lower-left
// panel with the address/nickname/port inputs and the pairing feedback. Emits
// StartPair / CancelPair / ClosePair.
void draw_pair_modal(const BridgeInput& in, BridgeState* state, BridgeAction* out_action);

// Draws the pairing PIN on the in-scene monitor when in.pairing_show_pin.
// Emits no actions.
void draw_pin_panel(const BridgeInput& in, BridgeState* state);

}  // namespace cosmic::ui::bridge