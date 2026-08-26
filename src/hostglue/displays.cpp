// Cosmic Desk — displays implementation. See displays.h for the contract.

#include "hostglue/displays.h"

// Vendored Sunshine headers; only this TU depends on them (displays.h stays
// clean, same pattern as pin_bridge.cpp).
#include "src/config.h"
#include "src/display_device.h"
#include "src/platform/common.h"

// libdisplaydevice types for the FloatingPoint refresh-rate visit.
#include <display_device/types.h>

#include <chrono>
#include <cmath>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace cosmic::displays {
namespace {

std::mutex g_mutex;
std::vector<DisplayInfo> g_cached;
std::chrono::steady_clock::time_point g_cache_time;

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

  std::vector<DisplayInfo> displays;
  displays.reserve(names.size());
  for (const auto &name : names) {
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

  // active: the display currently captured. Mid-stream switches are tracked
  // locally by the viewer (M5.3); this field is the connect-time hint.
  if (!output_name.empty()) {
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