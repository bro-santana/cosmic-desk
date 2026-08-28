// Cosmic Desk — host presence polling (docs/UI_MIGRATION.md U6).
//
// A single background worker polls GET http://<address>:<port>/serverinfo for
// every saved host every ~10 s (2 s timeout) so the Bridge cards can show the
// design's LINK READY state. Results are cached in a mutex-protected map and
// read from the main thread; the worker never touches ImGui/SDL.
#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cosmic::presence {

// Hosts to poll: address + effective port (already resolved via
// Settings::port_for). Starts the worker on first call; subsequent calls
// just replace the poll set (used when settings change - see main.cpp).
void start(const std::vector<std::pair<std::string, int>>& hosts);

// Stops the worker and joins it. Safe to call even if never started.
void stop();

// Thread-safe snapshot: address -> reachable.
std::map<std::string, bool> snapshot();

}  // namespace cosmic::presence
