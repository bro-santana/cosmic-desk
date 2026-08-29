// Cosmic Desk — current desktop wallpaper hash/bytes for the host's
// /serverinfo extension and authenticated wallpaper route (plan D10c,
// milestone W1 item 1). current_hash() lets /serverinfo advertise a content
// hash so a client can skip re-fetching an unchanged wallpaper; read_bytes()
// serves the actual image bytes over the authenticated HTTPS port (wired up
// by a later work item).
//
// This header is deliberately free of vendored Sunshine includes so nvhttp.cpp
// can include it without dragging the host's dependency graph into the app;
// wallpaper.cpp owns those includes (same pattern as displays.h/pin_bridge.h).

#pragma once

#include <string>

namespace cosmic::wallpaper {

// Host-side opt-out (settings' share_wallpaper). Defaults to true; while
// disabled, current_hash() and read_bytes() both report unavailable without
// touching the filesystem. Thread-safe.
void set_enabled(bool enabled);

// Lowercase hex SHA-256 (64 chars) of the current desktop wallpaper file.
// Returns "" when the wallpaper cannot be resolved (no desktop background,
// running off the interactive session with no recoverable fallback file),
// disabled via set_enabled(false), the file is empty, it exceeds the size
// cap, or it is not a JPEG, PNG or BMP (the only formats GET /cosmic/wallpaper
// can serve). Cached by (path, last-write-time, size): the file is only
// re-read and re-hashed when one of those changes. Thread-safe; safe to call
// from any HTTP server thread.
std::string current_hash();

// Raw bytes of the current desktop wallpaper file, or "" under the same
// conditions as current_hash(). Always re-reads the file from disk -- unlike
// the hash, the bytes are never cached. Thread-safe.
std::string read_bytes();

}  // namespace cosmic::wallpaper
