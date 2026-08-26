// Cosmic Desk — autostart at logon (plan M6.1). Windows uses the per-user
// HKCU\...\Run key; Linux writes an XDG autostart .desktop file. Both launch
// the app with --hidden so it starts minimized to the tray.
//
// Note: this is a per-user logon autostart, NOT a Windows service. A real
// service runs in session 0 and cannot capture the interactive desktop or
// inject input (plan 3.1 session-0 limitation); the realistic v1 approach is
// the tray app started at login.

#pragma once

namespace cosmic::autostart {

// Returns whether autostart is currently enabled in the OS.
bool enabled();

// Applies (enable=true) or removes (enable=false) autostart. Returns false on
// failure (e.g. registry/desktop file could not be written).
bool set_enabled(bool enable);

}  // namespace cosmic::autostart
