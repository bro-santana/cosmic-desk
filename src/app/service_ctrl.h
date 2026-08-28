// Cosmic Desk - Windows service control helpers (plan M9.1). Ported from
// upstream Sunshine src/entry_handler.cpp (namespace service_ctrl) into our
// own main() flow: the vendored shim in host/sunshine/src/entry_handler_shim.h
// keeps no-op stubs because hostglue calls config::parse with a synthetic
// argc=1, so the vendored --shortcut branch in config.cpp can never run.
// Non-Windows builds are no-ops: the service model is Windows-only (plan D7).

#pragma once

#include <cstdint>

namespace cosmic::service_ctrl {

// Service name as registered by packaging/windows/install-service.ps1
// (tools/cosmicsvc.cpp SERVICE_NAME).
inline constexpr const char* kServiceName = "CosmicDeskService";

// True when the Cosmic Desk service is in the SERVICE_RUNNING state.
bool is_service_running();

// Starts the service and waits until it leaves SERVICE_START_PENDING.
bool start_service();

// Polls the TCP table for a listener on `port` (up to 30 s).
bool wait_for_ui_ready(uint16_t port);

}  // namespace cosmic::service_ctrl