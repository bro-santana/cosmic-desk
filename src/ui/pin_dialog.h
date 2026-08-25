// Cosmic Desk — native PIN pairing dialog (plan M1.4). Renders an ImGui modal
// that asks the user for the 4-digit PIN shown on the pairing client and hands
// it to cosmic::pin_bridge::submit_pin(), which completes the parked /pair
// request server-side.

#pragma once

#include <string>

namespace cosmic::ui {

// Draws the pairing modal. Call once per frame while `open` is true.
//   client_name — the unique ID of the client requesting pairing.
//   open        — in/out: set to false when the dialog is dismissed (OK or
//                 Cancel). The caller owns it.
//   result_ok   — set to true when the PIN was accepted and the client is
//                 paired. The caller should treat the session as paired.
void draw_pin_dialog(const std::string &client_name, bool &open, bool &result_ok);

}  // namespace cosmic::ui