// Cosmic Desk — displays implementation. See displays.h for the contract.

#include "hostglue/displays.h"

// Vendored Sunshine headers; only this TU depends on them (displays.h stays
// clean, same pattern as pin_bridge.cpp).
#include "src/config.h"
#include "src/display_device.h"
#include "src/platform/common.h"

// libdisplaydevice types for the FloatingPoint refresh-rate visit.
#include <display_device/types.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cosmic::displays {
namespace {

std::mutex g_mutex;
std::vector<DisplayInfo> g_cached;
std::chrono::steady_clock::time_point g_cache_time;

// Display order last seen while nothing was being captured. platf::display_names()
// probes every output with DuplicateOutput() (display_base.cpp
// test_dxgi_duplication), and that probe fails for every output that is not the
// one currently being duplicated -- 0x887A0004 DXGI_ERROR_UNSUPPORTED on
// multi-adapter setups. So mid-stream it collapses to just the captured display,
// which emptied the viewer's monitor dropdown of every other entry and left no
// index to switch back to. video.cpp does not hit this because it calls
// disp.reset() before re-enumerating; /serverinfo cannot, so it keeps the last
// unfiltered ordering instead. Ordering matters: it is the index contract (D3c)
// that apply_shortcut()'s F(1+i) handler consumes.
std::vector<std::string> g_stable_names;

// m_refresh_rate is a FloatingPoint (double or Rational); round to int.
int fps_from_refresh_rate(const display_device::FloatingPoint &refresh_rate) {
  return std::visit([](const auto &value) -> int {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, double>) {
      return static_cast<int>(std::lround(value));
    } else {
      // Rational: numerator / denominator
      if (value.m_denominator == 0) {
        return 0;
      }
      return static_cast<int>(std::lround(static_cast<double>(value.m_numerator) / value.m_denominator));
    }
  }, refresh_rate);
}

}  // namespace

std::vector<DisplayInfo> list_displays() {
  std::lock_guard<std::mutex> lock(g_mutex);

  const auto now = std::chrono::steady_clock::now();
  if (now - g_cache_time < std::chrono::seconds(3)) {
    return g_cached;
  }

  // Ordering contract (plan D3c): this vector is in platf::display_names()
  // order, the same order consumed by apply_shortcut()'s Ctrl+Alt+Shift+F(1+i)
  // handler in input.cpp and by video.cpp's switch_display path. index i in the
  // /serverinfo <CosmicDisplays> XML == F(1+i). Any change to how displays are
  // enumerated must update all three call sites together.
  const auto names = platf::display_names(platf::mem_type_e::dxgi);
  const auto devices = display_device::enumerate_devices();
  const auto output_name = display_device::map_output_name(config::video.output_name);

  // Did the duplication probe filter the list, or is this a genuine change?
  const bool shrunk =
      !g_stable_names.empty() && names.size() < g_stable_names.size() &&
      std::all_of(names.begin(), names.end(), [](const std::string &name) {
        return std::find(g_stable_names.begin(), g_stable_names.end(), name) !=
               g_stable_names.end();
      });
  if (!names.empty() && !shrunk) {
    // An unfiltered view: adopt it wholesale so a genuine hotplug re-orders
    // and re-sizes the list the same way platf::display_names() would.
    g_stable_names = names;
  }

  // Enumerate over the stable ordering, but only for displays the OS still
  // reports as attached -- libdisplaydevice does no DXGI duplication, so it
  // tells apart "filtered by the probe" from "actually unplugged".
  std::vector<std::string> effective;
  effective.reserve(g_stable_names.size());
  for (const auto &name : g_stable_names) {
    const bool attached =
        std::any_of(devices.begin(), devices.end(), [&name](const auto &device) {
          return device.m_display_name == name && device.m_info.has_value();
        });
    if (attached) {
      effective.push_back(name);
    }
  }
  if (effective.empty()) {
    effective = names;  // Nothing corroborated; report what DXGI gave us.
  }

  std::vector<DisplayInfo> displays;
  displays.reserve(effective.size());
  for (const auto &name : effective) {
    DisplayInfo info;
    info.name = name;

    for (const auto &device : devices) {
      if (device.m_display_name != name) {
        continue;
      }
      if (device.m_info) {
        info.width = static_cast<int>(device.m_info->m_resolution.m_width);
        info.height = static_cast<int>(device.m_info->m_resolution.m_height);
        info.fps = fps_from_refresh_rate(device.m_info->m_refresh_rate);
        info.primary = device.m_info->m_primary;
      }
      break;
    }

    displays.push_back(std::move(info));
  }

  // active: the display currently captured. While a capture is running the
  // only display that survives the DuplicateOutput() probe is the captured
  // one, so a filtered `names` identifies it exactly -- better than the
  // config hint below, which a mid-stream switch never updates.
  if (shrunk && names.size() == 1) {
    for (auto &info : displays) {
      info.active = (info.name == names.front());
    }
  } else if (!output_name.empty()) {
    for (auto &info : displays) {
      if (info.name == output_name) {
        info.active = true;
        break;
      }
    }
  }
  if (!displays.empty() && !displays.front().active) {
    // output_name empty or no match -> index 0 is active.
    displays.front().active = true;
  }

  g_cached = std::move(displays);
  g_cache_time = now;
  return g_cached;
}

}  // namespace cosmic::displays