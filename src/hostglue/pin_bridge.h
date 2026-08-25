// Cosmic Desk — bridge between the vendored Sunshine host thread and the app's
// UI thread for the native PIN pairing dialog (plan M1.4). The nvhttp thread
// parks a /pair?phrase=getservercert request and calls notify_pair_request();
// the main thread polls each frame and, when the user submits a PIN, calls
// submit_pin() which completes the parked request via nvhttp::pin().
//
// This header is deliberately free of vendored Sunshine includes so nvhttp.cpp
// can include it without dragging the host's dependency graph into the app.
// submit_pin() is declared here and implemented in pin_bridge.cpp, which owns
// the nvhttp.h include.

#pragma once

#include <string>

namespace cosmic::pin_bridge {

// Called from the nvhttp thread when a /pair getservercert request parks
// (host/sunshine/src/nvhttp.cpp). Thread-safe: appends to the pending queue and
// remembers the client name for submit_pin().
void notify_pair_request(const std::string &client_name);

// Called from the main thread each frame. Pops the oldest pending pairing
// request, if any, into client_name and returns true.
bool poll(std::string &client_name);

// Called from the UI thread when the user submits the 4-digit PIN. Completes
// the parked pairing request via nvhttp::pin(pin, latest_client_name) and
// returns its result.
//
// NOTE: nvhttp::pin() is called from the UI thread while pair requests arrive
// on the nvhttp thread. This mirrors upstream Sunshine, where the web UI
// (confighttp) called pin() from another thread; accepted for v1.
bool submit_pin(const std::string &pin);

}  // namespace cosmic::pin_bridge