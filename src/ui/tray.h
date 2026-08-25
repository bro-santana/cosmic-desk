#pragma once

#include <functional>
#include <string>

namespace cosmic::ui {

struct TrayCallbacks {
    std::function<void()> on_show;  // "Show Cosmic Desk"
    std::function<void()> on_quit;  // "Quit"
};

// Installs the tray icon. icon_path must point at an .ico on Windows and a
// .png on Linux. Returns false when the platform has no usable tray, which is
// not fatal: the caller keeps running with a visible window instead.
bool tray_start(const std::string& icon_path, TrayCallbacks callbacks);

// Pumps pending tray events without blocking. Call once per frame.
// Returns false once the tray loop has ended (e.g. the menu asked to quit).
bool tray_pump();

void tray_stop();

bool tray_available();

}  // namespace cosmic::ui
