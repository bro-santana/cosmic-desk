/**
 * @file src/entry_handler_shim.h
 * @brief COSMIC MODIFICATION: replacement for the deleted upstream entry_handler.h.
 *
 * Upstream's entry_handler.{h,cpp} handled the Sunshine service entry point, the
 * web UI launcher and process lifetime. Cosmic Desk has its own main() and its own
 * tray, and the web UI was dropped (plan D5), so the original file was deleted.
 * This shim keeps the handful of symbols the remaining vendored sources reference:
 *   - config.cpp: launch_ui(), service_ctrl::* (Windows service model)
 *   - platform/{windows,linux}/misc.cpp: lifetime::exit_sunshine(), lifetime::get_argv()
 */
#pragma once

// standard includes
#include <atomic>
#include <csignal>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
  #include <windows.h>
#endif

/**
 * @brief Launch the Web UI.
 * @details The web UI was dropped (plan D5); this is a no-op.
 */
inline void launch_ui(const std::optional<std::string> & = std::nullopt) {
}

/**
 * @brief Functions for handling the lifetime of Sunshine.
 */
namespace lifetime {
  inline char **argv = nullptr;
  inline std::atomic_int desired_exit_code {0};

  /**
   * @brief Terminates the process gracefully with the provided exit code.
   * @details Mirrors upstream: raise SIGINT to start termination; block until
   *          termination completes when synchronous behavior is requested.
   */
  inline void exit_sunshine(int exit_code, bool async) {
    int zero = 0;
    desired_exit_code.compare_exchange_strong(zero, exit_code);

    std::raise(SIGINT);

    while (!async) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  /**
   * @brief Get the argv array passed to main().
   */
  inline char **get_argv() {
    return argv;
  }

  /**
   * @brief Breaks into the debugger or terminates the process if no debugger is attached.
   */
  inline void debug_trap() {
#ifdef _WIN32
    DebugBreak();
#else
    std::raise(SIGTRAP);
#endif
  }
}  // namespace lifetime

#ifdef _WIN32
/**
 * @brief Namespace for controlling the Sunshine service model on Windows.
 * @details These stubs intentionally stay no-ops: hostglue calls config::parse
 *          with a synthetic argc=1 (see src/hostglue/host.cpp), so the vendored
 *          --shortcut branch in config.cpp can never run. The real service
 *          control lives in src/app/service_ctrl.cpp (plan M9).
 */
namespace service_ctrl {
  inline bool is_service_running() {
    return false;
  }

  inline bool start_service() {
    return false;
  }

  inline bool wait_for_ui_ready() {
    return false;
  }
}  // namespace service_ctrl
#endif