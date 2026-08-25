// Cosmic Desk — pin_bridge implementation. See pin_bridge.h for the contract.

#include "hostglue/pin_bridge.h"

// Vendored Sunshine header; only this TU needs it (pin_bridge.h stays clean).
#include "src/nvhttp.h"

#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace cosmic::pin_bridge {
namespace {

std::mutex g_mutex;
std::deque<std::string> g_pending;
std::string g_latest_client_name;

}  // namespace

void notify_pair_request(const std::string &client_name) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.push_back(client_name);
    g_latest_client_name = client_name;
  }
  // Log to stdout so the pairing hook is visible in the console/log file
  // (used by the M1.4 smoke test). Flush explicitly: stdout is block-buffered
  // when redirected, and the app may be killed before the buffer drains.
  std::printf("[pin_bridge] pairing request from client: %s\n", client_name.c_str());
  std::fflush(stdout);
}

bool poll(std::string &client_name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pending.empty()) {
    return false;
  }
  client_name = std::move(g_pending.front());
  g_pending.pop_front();
  return true;
}

bool submit_pin(const std::string &pin) {
  std::string client_name;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    client_name = g_latest_client_name;
  }
  // Notes on the pairing handshake:
  //  - The paired client's stored name is the Moonlight uniqueid: nvhttp::pin()
  //    stores whatever name we pass through, and we pass the uniqueid that
  //    notify_pair_request() received from the /pair request.
  //  - With multiple concurrent pair requests, nvhttp::pin() always answers the
  //    FIRST pending session (std::begin(map_id_sess)) regardless of which name
  //    the dialog shows — inherited from upstream pin() semantics, acceptable
  //    for the v1 single-client flow.
  return nvhttp::pin(pin, client_name);
}

}  // namespace cosmic::pin_bridge