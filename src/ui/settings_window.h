// Cosmic Desk — settings window (plan M4.4). Edits the live app-wide Settings
// object: resolution mode, custom resolution, FPS, bitrate, port base, and the
// autostart flag. Changes apply immediately and are persisted to cosmic.json
// when the window closes.

#pragma once

#include "app/settings.h"

namespace cosmic::ui {

// Draws the settings window. Call once per frame, unconditionally: the
// function early-returns when `open` is false and saves pending edits on
// the close transition.
//   settings — the live Settings object owned by main.cpp; edits write
//              straight into it.
//   open     — in/out: set to false when the window is closed (X button).
//              The caller owns it.
void draw_settings_window(Settings &settings, bool &open);

}  // namespace cosmic::ui