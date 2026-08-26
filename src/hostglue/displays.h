// Cosmic Desk — display enumeration for the host's /serverinfo extension
// (plan M5.1, D3a). list_displays() returns the host's monitors in
// platf::display_names() order, enriched with resolution/refresh-rate/primary
// info from display_device::enumerate_devices() and the currently captured
// display.
//
// This header is deliberately free of vendored Sunshine includes so nvhttp.cpp
// can include it without dragging the host's dependency graph into the app;
// displays.cpp owns those includes (same pattern as pin_bridge.h).

#pragma once

#include <string>
#include <vector>

namespace cosmic::displays {

struct DisplayInfo {
  std::string name;  // e.g. "\\.\DISPLAY1"
  int width = 0, height = 0, fps = 0;
  bool primary = false;
  bool active = false;
};

// Returns the host's displays in platf::display_names() order (see displays.cpp
// for the ordering contract). Cached for 3 seconds: serverinfo may be polled
// and enumeration is not free.
std::vector<DisplayInfo> list_displays();

}  // namespace cosmic::displays